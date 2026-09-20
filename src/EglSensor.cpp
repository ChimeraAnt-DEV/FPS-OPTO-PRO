#include "EglSensor.h"

#include <array>
#include <atomic>
#include <chrono>
#include <string>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <android/log.h>


#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>

#include "GlInterceptor.h"

namespace fpsopto {
namespace {


// Probing GPU completion costs a sync round trip, so it runs on roughly one
// frame in twenty. That is dense enough for a 120-frame window to collect a
// usable number of measurements, and sparse enough that the probe itself is
// not a measurable part of the frame.
constexpr int kProbePeriodFrames = 20;

std::uint64_t nowNs() {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(
      duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
          .count());
}

// The EGL 1.5 core sync entry points. eglCreateSync/eglClientWaitSync are
// aliases that some drivers export only under the extension name, so whichever
// resolves is used.
PFNEGLCREATESYNCKHRPROC gCreateSync = nullptr;
PFNEGLCLIENTWAITSYNCKHRPROC gClientWaitSync = nullptr;
PFNEGLDESTROYSYNCKHRPROC gDestroySync = nullptr;

PFNEGLGETCURRENTDISPLAYPROC gGetCurrentDisplay = nullptr;
PFNEGLGETCURRENTCONTEXTPROC gGetCurrentContext = nullptr;

std::atomic<int> gProbeCountdown{kProbePeriodFrames};

// The hook replaces the exported EGL symbol itself, so a detour that called the
// symbol by name would re-enter its own detour forever. The trampolines the
// hook API hands back are the only safe way through.
enum EglEntryIndex : std::size_t {
  kEglSwapBuffers,
  kEglSwapBuffersWithDamage,
  kEglCreateContext,
  kEglMakeCurrent,
  kEglDestroyContext,
  kEglSwapInterval,
  kEglEntryCount,
};

std::array<pl::memory::FuncPtr, kEglEntryCount> gEglOriginals{};

pl::memory::FuncPtr eglOriginal(EglEntryIndex index) {
  return gEglOriginals[index];
}

// Returns the GPU time for the work submitted by the last swap, or false when
// the driver offers no usable sync object.
//
// `overran` is set when the fence was already signalled-free at submit time
// with a zero timeout, which means the GPU had not finished the previous frame
// and is therefore the limiting resource.
bool probeGpuCompletion(EGLDisplay display, std::uint64_t &gpuNs, bool &overran) {
  gpuNs = 0;
  overran = false;
  if (display == EGL_NO_DISPLAY || gCreateSync == nullptr ||
      gClientWaitSync == nullptr || gDestroySync == nullptr) {
    return false;
  }

  const EGLSyncKHR sync =
      gCreateSync(display, EGL_SYNC_FENCE_KHR, nullptr);
  if (sync == EGL_NO_SYNC_KHR) {
    return false;
  }

  const EGLint polled =
      gClientWaitSync(display, sync, 0, static_cast<EGLTimeKHR>(0));
  overran = polled == EGL_TIMEOUT_EXPIRED_KHR;

  const std::uint64_t start = nowNs();
  const EGLint waited = gClientWaitSync(
      display, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,
      static_cast<EGLTimeKHR>(50ull * 1000ull * 1000ull));
  gpuNs = nowNs() - start;
  gDestroySync(display, sync);
  return waited == EGL_CONDITION_SATISFIED_KHR;
}

void observeSwap(std::uint64_t cpuNs) {
  auto &sensor = EglSensor::instance();
  auto &interceptor = GlInterceptor::instance();

  std::uint64_t gpuNs = 0;
  bool gpuMeasured = false;
  bool overran = false;

  if (--gProbeCountdown <= 0) {
    gProbeCountdown = kProbePeriodFrames;
    if (interceptor.installed()) {
      // Reading the fence here keeps the probe out of the hot path on devices
      // whose driver cannot report completion at all.
      const EGLDisplay display =
          gGetCurrentDisplay != nullptr ? gGetCurrentDisplay() : EGL_NO_DISPLAY;
      gpuMeasured = probeGpuCompletion(display, gpuNs, overran);
    }
  }

  sensor.endFrame(cpuNs, gpuMeasured, gpuNs, overran);
}

// Every swap entry point funnels through here so the three of them cannot
// drift apart.
template <typename Swap>
EGLBoolean doSwap(Swap &&swap) {
  auto &sensor = EglSensor::instance();
  sensor.beginFrame();
  const std::uint64_t start = nowNs();
  const EGLBoolean result = swap();
  observeSwap(nowNs() - start);
  return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Detours
// ---------------------------------------------------------------------------

extern "C" {

EGLBoolean fpsopto_eglSwapBuffers(EGLDisplay display, EGLSurface surface) {
  return doSwap([&]() {
    const auto swap = reinterpret_cast<PFNEGLSWAPBUFFERSPROC>(
        eglOriginal(kEglSwapBuffers));
    return swap != nullptr ? swap(display, surface)
                           : static_cast<EGLBoolean>(EGL_FALSE);
  });
}

EGLBoolean fpsopto_eglSwapBuffersWithDamageKHR(EGLDisplay display,
                                               EGLSurface surface,
                                               const EGLint *rects,
                                               EGLint nRects) {
  return doSwap([&]() -> EGLBoolean {
    // The trampoline is the hooked symbol's own body, so it must be used here.
    // eglGetProcAddress would return this detour again -- the export table is
    // what was patched -- and calling it would recurse until the stack dies.
    auto swap = reinterpret_cast<PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC>(
        eglOriginal(kEglSwapBuffersWithDamage));
    if (swap == nullptr) {
      // The driver never provided the entry point, so behave exactly like a
      // plain swap rather than failing the present.
      const auto plain =
          reinterpret_cast<PFNEGLSWAPBUFFERSPROC>(eglOriginal(kEglSwapBuffers));
      return plain != nullptr ? plain(display, surface) : EGL_FALSE;
    }
    // The driver's own prototype takes a non-const rects pointer, so the
    // const from our detour signature is cast away here. The driver only
    // reads the rects, so the cast does not permit a write.
    return swap(display, surface, const_cast<EGLint *>(rects), nRects);
  });
}

EGLContext fpsopto_eglCreateContext(EGLDisplay display, EGLConfig config,
                                    EGLContext shareContext,
                                    const EGLint *attribs) {
  const auto create = reinterpret_cast<PFNEGLCREATECONTEXTPROC>(
      eglOriginal(kEglCreateContext));
  if (create == nullptr) {
    return EGL_NO_CONTEXT;
  }
  const EGLContext created = create(display, config, shareContext, attribs);
  if (created != EGL_NO_CONTEXT) {
    // Registering the context (rather than resetting on the next make-current)
    // gives it a shadow that starts at the spec defaults -- which is the only
    // shadow this mod will filter against. A context created before the hooks
    // is never registered, so its calls stay unfiltered.
    GlInterceptor::instance().noteContextCreated(
        reinterpret_cast<std::uintptr_t>(created));
  }
  return created;
}

EGLBoolean fpsopto_eglMakeCurrent(EGLDisplay display, EGLSurface draw,
                                  EGLSurface read, EGLContext context) {
  const auto makeCurrent = reinterpret_cast<PFNEGLMAKECURRENTPROC>(
      eglOriginal(kEglMakeCurrent));
  if (makeCurrent == nullptr) {
    return EGL_FALSE;
  }
  // Nothing is reset here. The shadow belongs to the context and is looked up
  // per call, so making a context current -- on any thread, including a loader
  // thread -- cannot disturb another thread's state.
  return makeCurrent(display, draw, read, context);
}

EGLBoolean fpsopto_eglDestroyContext(EGLDisplay display, EGLContext context) {
  const auto destroy = reinterpret_cast<PFNEGLDESTROYCONTEXTPROC>(
      eglOriginal(kEglDestroyContext));
  if (destroy == nullptr) {
    return EGL_FALSE;
  }
  const EGLBoolean result = destroy(display, context);
  GlInterceptor::instance().noteContextDestroyed(
      reinterpret_cast<std::uintptr_t>(context));
  return result;
}

EGLBoolean fpsopto_eglSwapInterval(EGLDisplay display, EGLint interval) {
  EglSensor::instance().noteSwapInterval(interval);
  const auto setInterval = reinterpret_cast<PFNEGLSWAPINTERVALPROC>(
      eglOriginal(kEglSwapInterval));
  return setInterval != nullptr ? setInterval(display, interval)
                                : static_cast<EGLBoolean>(EGL_FALSE);
}

} // extern "C"

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------

bool EglSensor::install(const std::string &moduleName) {
  if (mInstalledCount > 0) {
    return true;
  }

  mFrames.reset();

  struct Entry {
    const char *name;
    pl::memory::FuncPtr detour;
    EglEntryIndex index;
  };

  const Entry entries[] = {
      {"eglSwapBuffers",
       reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_eglSwapBuffers),
       kEglSwapBuffers},
      {"eglSwapBuffersWithDamageKHR",
       reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_eglSwapBuffersWithDamageKHR),
       kEglSwapBuffersWithDamage},
      {"eglCreateContext",
       reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_eglCreateContext),
       kEglCreateContext},
      {"eglMakeCurrent",
       reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_eglMakeCurrent),
       kEglMakeCurrent},
      {"eglDestroyContext",
       reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_eglDestroyContext),
       kEglDestroyContext},
      {"eglSwapInterval",
       reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_eglSwapInterval),
       kEglSwapInterval},
  };

  for (const auto &entry : entries) {
    const std::uintptr_t address =
        pl::memory::resolveSignature(entry.name, moduleName);
    void *original = nullptr;
    if (address != 0 &&
        pl::memory::hook(reinterpret_cast<void *>(address), entry.detour,
                         &original) == 0) {
      // Without a trampoline the detour would call its own address, so a hook
      // that produced none is treated as failed rather than risking a hang.
      if (original == nullptr) {
        pl::memory::unhook(reinterpret_cast<void *>(address), entry.detour);
        mFailedHooks.emplace_back(entry.name);
        continue;
      }
      gEglOriginals[entry.index] = original;
      ++mInstalledCount;
    } else {
      mFailedHooks.emplace_back(entry.name);
    }
  }

  // Resolve the helpers directly rather than through the hook chain: the mod
  // only reads from them, and going through the driver's own copy avoids
  // depending on hook order.
  gCreateSync = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(
      eglGetProcAddress("eglCreateSyncKHR"));
  gClientWaitSync = reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(
      eglGetProcAddress("eglClientWaitSyncKHR"));
  gDestroySync = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(
      eglGetProcAddress("eglDestroySyncKHR"));
  gGetCurrentDisplay = reinterpret_cast<PFNEGLGETCURRENTDISPLAYPROC>(
      eglGetProcAddress("eglGetCurrentDisplay"));
  gGetCurrentContext = reinterpret_cast<PFNEGLGETCURRENTCONTEXTPROC>(
      eglGetProcAddress("eglGetCurrentContext"));

  mInstalled = mInstalledCount > 0;
  return mInstalled;
}

void EglSensor::beginFrame() { mFrameStartNs = nowNs(); }

void EglSensor::noteSwapInterval(int interval) { mSwapInterval = interval; }

void EglSensor::setFrameCallback(FrameCallback callback, void *user) {
  mFrameCallback = callback;
  mFrameCallbackUser = user;
}

void EglSensor::endFrame(std::uint64_t cpuNs, bool gpuMeasured,
                         std::uint64_t gpuNs, bool gpuOverran) {
  const std::uint64_t now = nowNs();
  const std::uint64_t frameNs =
      mLastSwapNs != 0 && now > mLastSwapNs ? now - mLastSwapNs : 0;
  mLastSwapNs = now;
  ++mSwapCount;
  mTotalCpuNs += cpuNs;

  if (frameNs != 0) {
    FrameSample sample;
    sample.frameNs = frameNs;
    sample.cpuNs = cpuNs;
    sample.gpuNs = gpuNs;
    sample.gpuMeasured = gpuMeasured;
    sample.gpuOverran = gpuOverran;
    mFrames.push(sample);

    if (mFrameCallback != nullptr) {
      mFrameCallback(mFrames.stats(), mFrames.cadenceMs(), mFrames.vsyncLocked(),
                     mFrameCallbackUser);
    }
  }
}

EglSensor &EglSensor::instance() {
  static EglSensor sensor;
  return sensor;
}

} // namespace fpsopto