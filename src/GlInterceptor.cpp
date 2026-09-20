// NOTE: this translation unit must also be compiled into the host test binary,
// where PL_INTERPOSER_HOST_TEST keeps the Android/GL/preloader dependencies out
// and the `gl*` call-through names become the test's probe functions.

#include "GlInterceptor.h"

#include <array>
#include <cstring>
#include <dlfcn.h>
#include <type_traits>
#include <mutex>
#include <string>
#include <unordered_map>

#ifdef PL_INTERPOSER_HOST_TEST
extern "C" {
void glEnable(unsigned int);
void glDisable(unsigned int);
void glBlendFunc(unsigned int, unsigned int);
void glBlendFuncSeparate(unsigned int, unsigned int, unsigned int, unsigned int);
void glBlendEquation(unsigned int);
void glBlendEquationSeparate(unsigned int, unsigned int);
void glBlendColor(float, float, float, float);
void glDepthFunc(unsigned int);
void glDepthMask(unsigned char);
void glColorMask(unsigned char, unsigned char, unsigned char, unsigned char);
void glCullFace(unsigned int);
void glFrontFace(unsigned int);
void glDepthRangef(float, float);
void glSampleCoverage(float, unsigned char);
void glViewport(int, int, int, int);
void glUseProgram(unsigned int);
void glActiveTexture(unsigned int);
void glBindBuffer(unsigned int, unsigned int);
void glBindTexture(unsigned int, unsigned int);
void glBindFramebuffer(unsigned int, unsigned int);
void glBindVertexArray(unsigned int);
void glPixelStorei(unsigned int, int);
static inline void fpsoptoLog(const char *) {}
}
#define FPSOPTO_LOG(msg) fpsoptoLog(msg)
#define FPSOPTO_HAVE_PRELOADER 0
#else
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>

#include <EGL/egl.h>

#include <android/log.h>

#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>
#define FPSOPTO_LOG(msg) \
  __android_log_print(ANDROID_LOG_INFO, "FPS-OPTO-PRO", "%s", (msg))
#define FPSOPTO_HAVE_PRELOADER 1
#endif

namespace fpsopto {
namespace {

void record(bool suppressed) {
  GlInterceptor::instance().noteCall(suppressed);
}

// Convenience wrapper. `self.shadow()` is null for a context this mod must not
// filter, and every detour then forwards the call untouched.
GlStateModel *stateOf(GlInterceptor &self) { return self.shadow().state; }

// The hook replaces the exported symbol itself, so a detour that called the
// symbol by name would re-enter its own detour forever. The trampoline the
// hook API hands back is the only safe way through, and it is stored per
// symbol here.
using GlEntryPoint = void (*)();

enum GlEntryIndex : std::size_t {
  kEntryEnable,
  kEntryDisable,
  kEntryBlendFunc,
  kEntryBlendFuncSeparate,
  kEntryBlendEquation,
  kEntryBlendEquationSeparate,
  kEntryBlendColor,
  kEntryDepthFunc,
  kEntryDepthMask,
  kEntryColorMask,
  kEntryCullFace,
  kEntryFrontFace,
  kEntryDepthRangef,
  kEntrySampleCoverage,
  kEntryViewport,
  kEntryUseProgram,
  kEntryActiveTexture,
  kEntryBindBuffer,
  kEntryBindTexture,
  kEntryBindFramebuffer,
  kEntryBindVertexArray,
  kEntryPixelStorei,
  kEntryCount,
};

std::array<GlEntryPoint, kEntryCount> gOriginals{};

// Falls back to the direct symbol when the hook API did not supply a
// trampoline. That is correct in the host test build, where the `gl*` names are
// the test's own probes and are never hooked, and on a device it can only be
// reached for a symbol whose hook failed -- in which case no detour of ours is
// installed on it either, so there is nothing to recurse into.
GlEntryPoint entryOr(GlEntryIndex index, GlEntryPoint fallback) {
  const GlEntryPoint original = gOriginals[index];
  return original != nullptr ? original : fallback;
}

#define FPSOPTO_ORIGINAL(index, fallback)                                  \
  (reinterpret_cast<std::decay_t<decltype(fallback)>>(                      \
      entryOr((index), reinterpret_cast<GlEntryPoint>(fallback))))

} // namespace

// ---------------------------------------------------------------------------
// Detours
// ---------------------------------------------------------------------------

extern "C" {

void fpsopto_glEnable(unsigned int capability) {
  auto &self = GlInterceptor::instance();
  GlStateModel *state = stateOf(self);
  const bool tracked = state != nullptr;
  const std::uint32_t bit = capabilityBit(capability);
  if (tracked && bit != 0 && (state->enabled & bit) != 0) {
    record(true);
    return;
  }
  record(false);
  if (tracked && bit != 0) {
    state->enabled |= bit;
  }
  FPSOPTO_ORIGINAL(kEntryEnable, glEnable)(capability);
}

void fpsopto_glDisable(unsigned int capability) {
  auto &self = GlInterceptor::instance();
  GlStateModel *state = stateOf(self);
  const bool tracked = state != nullptr;
  const std::uint32_t bit = capabilityBit(capability);
  if (tracked && bit != 0 && (state->enabled & bit) == 0) {
    record(true);
    return;
  }
  record(false);
  if (tracked && bit != 0) {
    state->enabled &= ~bit;
  }
  FPSOPTO_ORIGINAL(kEntryDisable, glDisable)(capability);
}

void fpsopto_glBlendFunc(unsigned int src, unsigned int dst) {
  auto &self = GlInterceptor::instance();
  GlStateModel *state = stateOf(self);
  const bool tracked = state != nullptr;
  if (tracked && sameBlend(*state, src, dst, src, dst)) {
    record(true);
    return;
  }
  record(false);
  if (tracked) {
    state->blendSrcRgb = src;
    state->blendDstRgb = dst;
    state->blendSrcAlpha = src;
    state->blendDstAlpha = dst;
  }
  FPSOPTO_ORIGINAL(kEntryBlendFunc, glBlendFunc)(src, dst);
}

void fpsopto_glBlendFuncSeparate(unsigned int srcRgb, unsigned int dstRgb,
                                 unsigned int srcAlpha, unsigned int dstAlpha) {
  auto &self = GlInterceptor::instance();
  GlStateModel *state = stateOf(self);
  const bool tracked = state != nullptr;
  if (tracked && sameBlend(*state, srcRgb, dstRgb, srcAlpha, dstAlpha)) {
    record(true);
    return;
  }
  record(false);
  if (tracked) {
    state->blendSrcRgb = srcRgb;
    state->blendDstRgb = dstRgb;
    state->blendSrcAlpha = srcAlpha;
    state->blendDstAlpha = dstAlpha;
  }
  FPSOPTO_ORIGINAL(kEntryBlendFuncSeparate, glBlendFuncSeparate)(srcRgb, dstRgb, srcAlpha, dstAlpha);
}

void fpsopto_glBlendEquation(unsigned int mode) {
  auto &self = GlInterceptor::instance();
  GlStateModel *state = stateOf(self);
  const bool tracked = state != nullptr;
  if (tracked && state->blendEquationRgb == mode &&
      state->blendEquationAlpha == mode) {
    record(true);
    return;
  }
  record(false);
  if (tracked) {
    state->blendEquationRgb = mode;
    state->blendEquationAlpha = mode;
  }
  FPSOPTO_ORIGINAL(kEntryBlendEquation, glBlendEquation)(mode);
}

void fpsopto_glBlendEquationSeparate(unsigned int rgb, unsigned int alpha) {
  auto &self = GlInterceptor::instance();
  GlStateModel *state = stateOf(self);
  const bool tracked = state != nullptr;
  if (tracked && state->blendEquationRgb == rgb &&
      state->blendEquationAlpha == alpha) {
    record(true);
    return;
  }
  record(false);
  if (tracked) {
    state->blendEquationRgb = rgb;
    state->blendEquationAlpha = alpha;
  }
  FPSOPTO_ORIGINAL(kEntryBlendEquationSeparate, glBlendEquationSeparate)(rgb, alpha);
}

void fpsopto_glBlendColor(float r, float g, float b, float a) {
  auto &self = GlInterceptor::instance();
  GlStateModel *state = stateOf(self);
  const bool tracked = state != nullptr;
  if (tracked && sameBlendColor(*state, r, g, b, a)) {
    record(true);
    return;
  }
  record(false);
  if (tracked) {
    state->blendColorR = r;
    state->blendColorG = g;
    state->blendColorB = b;
    state->blendColorA = a;
  }
  FPSOPTO_ORIGINAL(kEntryBlendColor, glBlendColor)(r, g, b, a);
}

void fpsopto_glDepthFunc(unsigned int func) {
  auto &self = GlInterceptor::instance();
  GlStateModel *state = stateOf(self);
  const bool tracked = state != nullptr;
  if (tracked && state->depthFunc == func) {
    record(true);
    return;
  }
  record(false);
  if (tracked) {
    state->depthFunc = func;
  }
  FPSOPTO_ORIGINAL(kEntryDepthFunc, glDepthFunc)(func);
}

void fpsopto_glDepthMask(unsigned char flag) {
  auto &self = GlInterceptor::instance();
  GlStateModel *state = stateOf(self);
  const bool tracked = state != nullptr;
  const std::uint8_t normalized = flag ? 1 : 0;
  if (tracked && state->depthMask == normalized) {
    record(true);
    return;
  }
  record(false);
  if (tracked) {
    state->depthMask = normalized;
  }
  FPSOPTO_ORIGINAL(kEntryDepthMask, glDepthMask)(flag);
}

void fpsopto_glColorMask(unsigned char r, unsigned char g, unsigned char b,
                         unsigned char a) {
  auto &self = GlInterceptor::instance();
  GlStateModel *state = stateOf(self);
  const bool tracked = state != nullptr;
  const std::uint32_t nr = r ? 1u : 0u;
  const std::uint32_t ng = g ? 1u : 0u;
  const std::uint32_t nb = b ? 1u : 0u;
  const std::uint32_t na = a ? 1u : 0u;
  if (tracked && sameColorMask(*state, nr, ng, nb, na)) {
    record(true);
    return;
  }
  record(false);
  if (tracked) {
    state->colorMaskR = nr;
    state->colorMaskG = ng;
    state->colorMaskB = nb;
    state->colorMaskA = na;
  }
  FPSOPTO_ORIGINAL(kEntryColorMask, glColorMask)(r, g, b, a);
}

void fpsopto_glCullFace(unsigned int mode) {
  auto &self = GlInterceptor::instance();
  GlStateModel *state = stateOf(self);
  const bool tracked = state != nullptr;
  if (tracked && state->cullFaceMode == mode) {
    record(true);
    return;
  }
  record(false);
  if (tracked) {
    state->cullFaceMode = mode;
  }
  FPSOPTO_ORIGINAL(kEntryCullFace, glCullFace)(mode);
}

void fpsopto_glFrontFace(unsigned int mode) {
  auto &self = GlInterceptor::instance();
  GlStateModel *state = stateOf(self);
  const bool tracked = state != nullptr;
  if (tracked && state->frontFace == mode) {
    record(true);
    return;
  }
  record(false);
  if (tracked) {
    state->frontFace = mode;
  }
  FPSOPTO_ORIGINAL(kEntryFrontFace, glFrontFace)(mode);
}

void fpsopto_glDepthRangef(float near, float far) {
  auto &self = GlInterceptor::instance();
  GlStateModel *state = stateOf(self);
  const bool tracked = state != nullptr;
  if (tracked && sameDepthRange(*state, near, far)) {
    record(true);
    return;
  }
  record(false);
  if (tracked) {
    state->depthRangeNear = near;
    state->depthRangeFar = far;
  }
  FPSOPTO_ORIGINAL(kEntryDepthRangef, glDepthRangef)(near, far);
}

void fpsopto_glSampleCoverage(float value, unsigned char invert) {
  auto &self = GlInterceptor::instance();
  GlStateModel *state = stateOf(self);
  const bool tracked = state != nullptr;
  const std::uint8_t normalized = invert ? 1 : 0;
  if (tracked && sameSampleCoverage(*state, value, normalized)) {
    record(true);
    return;
  }
  record(false);
  if (tracked) {
    state->sampleCoverageValue = value;
    state->sampleCoverageInvert = normalized;
  }
  FPSOPTO_ORIGINAL(kEntrySampleCoverage, glSampleCoverage)(value, invert);
}

void fpsopto_glViewport(int x, int y, int w, int h) {
  auto &self = GlInterceptor::instance();
  GlStateModel *state = stateOf(self);
  const bool tracked = state != nullptr;
  if (tracked && sameViewport(*state, x, y, w, h)) {
    record(true);
    return;
  }
  record(false);
  if (tracked) {
    state->viewportX = x;
    state->viewportY = y;
    state->viewportW = w;
    state->viewportH = h;
  }
  FPSOPTO_ORIGINAL(kEntryViewport, glViewport)(x, y, w, h);
}

void fpsopto_glUseProgram(unsigned int program) {
  auto &self = GlInterceptor::instance();
  GlStateModel *state = stateOf(self);
  const bool tracked = state != nullptr;
  if (tracked && canDropBinding(state->currentProgram, program)) {
    record(true);
    return;
  }
  record(false);
  if (tracked) {
    state->currentProgram = program;
  }
  FPSOPTO_ORIGINAL(kEntryUseProgram, glUseProgram)(program);
}

void fpsopto_glActiveTexture(unsigned int texture) {
  auto &self = GlInterceptor::instance();
  GlStateModel *state = stateOf(self);
  const bool tracked = state != nullptr;
  if (tracked && state->activeTexture == texture) {
    record(true);
    return;
  }
  record(false);
  if (tracked) {
    state->activeTexture = texture;
  }
  FPSOPTO_ORIGINAL(kEntryActiveTexture, glActiveTexture)(texture);
}

void fpsopto_glBindBuffer(unsigned int target, unsigned int buffer) {
  auto &self = GlInterceptor::instance();
  GlStateModel *state = stateOf(self);
  std::uint32_t *shadow = nullptr;
  if (state != nullptr) {
    if (target == kGlArrayBuffer) {
      shadow = &state->arrayBuffer;
    } else if (target == kGlPixelUnpackBuffer) {
      shadow = &state->pixelUnpackBuffer;
    } else if (target == kGlPixelPackBuffer) {
      shadow = &state->pixelPackBuffer;
    }
  }
  // GL_ELEMENT_ARRAY_BUFFER is not shadowed here at all: the binding belongs to
  // the current vertex-array object, so it is always forwarded. A context this
  // mod does not filter forwards everything, so an untracked target only ever
  // reaches the driver.

  if (shadow != nullptr && canDropBinding(*shadow, buffer)) {
    record(true);
    return;
  }
  record(false);
  if (shadow != nullptr) {
    *shadow = buffer;
  }
  FPSOPTO_ORIGINAL(kEntryBindBuffer, glBindBuffer)(target, buffer);
}

void fpsopto_glBindTexture(unsigned int target, unsigned int texture) {
  auto &self = GlInterceptor::instance();
  const GlInterceptor::LiveShadow live = self.shadow();
  const int unitIndex =
      live ? TextureUnits::unitOf(live.state->activeTexture) : -1;
  TextureUnits *textures = live.textures;
  const int slot = textureTargetSlot(target);
  const bool tracked = textures != nullptr && unitIndex >= 0 && slot != 0;

  // Binding is only replaceable for a target whose slot can be addressed on the
  // active unit. A stale entry would resample the wrong image, so an untracked
  // target is always forwarded.
  if (tracked) {
    std::uint32_t &shadow = textures->textures[unitIndex][slot];
    if (canDropBinding(shadow, texture)) {
      record(true);
      return;
    }
    record(false);
    shadow = texture;
    FPSOPTO_ORIGINAL(kEntryBindTexture, glBindTexture)(target, texture);
    return;
  }

  record(false);
  FPSOPTO_ORIGINAL(kEntryBindTexture, glBindTexture)(target, texture);
}

void fpsopto_glBindFramebuffer(unsigned int target, unsigned int framebuffer) {
  auto &self = GlInterceptor::instance();
  GlStateModel *state = stateOf(self);
  const bool draw = target == kGlFramebuffer || target == kGlDrawFramebuffer;
  if (state != nullptr && draw && canDropBinding(state->framebuffer, framebuffer)) {
    record(true);
    return;
  }
  record(false);
  if (draw) {
    if (state != nullptr) {
      state->framebuffer = framebuffer;
    }
  } else if (target == kGlReadFramebuffer && state != nullptr) {
    // GL_READ_FRAMEBUFFER does not alias the draw binding, but a read bind can
    // be paired with a blit into the (still shadowed) draw target, so the draw
    // shadow is dropped rather than trusted across it. The read binding itself
    // is not shadowed: its only consumer is a blit, which re-binds to be safe,
    // so forwarding a repeated read bind is always correct.
    state->framebuffer = kUnknownUint;
  }
  FPSOPTO_ORIGINAL(kEntryBindFramebuffer, glBindFramebuffer)(target, framebuffer);
}

void fpsopto_glBindVertexArray(unsigned int array) {
  auto &self = GlInterceptor::instance();
  GlStateModel *state = stateOf(self);
  const bool tracked = state != nullptr;
  if (tracked && canDropBinding(state->vertexArray, array)) {
    record(true);
    return;
  }
  record(false);
  if (tracked) {
    state->vertexArray = array;
  }
  FPSOPTO_ORIGINAL(kEntryBindVertexArray, glBindVertexArray)(array);
}

void fpsopto_glPixelStorei(unsigned int pname, int param) {
  auto &self = GlInterceptor::instance();
  GlStateModel *state = stateOf(self);
  std::uint32_t *shadow = nullptr;
  if (state != nullptr) {
    if (pname == kGlUnpackAlignment) {
      shadow = &state->unpackAlignment;
    } else if (pname == kGlPackAlignment) {
      shadow = &state->packAlignment;
    } else if (pname == kGlUnpackRowLength) {
      shadow = &state->unpackRowLength;
    }
  }

  if (shadow != nullptr && samePixelStore(*shadow, param)) {
    record(true);
    return;
  }
  record(false);
  if (shadow != nullptr) {
    *shadow = static_cast<std::uint32_t>(param);
  }
  FPSOPTO_ORIGINAL(kEntryPixelStorei, glPixelStorei)(pname, param);
}

} // extern "C"

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------

#if FPSOPTO_HAVE_PRELOADER
namespace {

struct HookSpec {
  const char *name;
  pl::memory::FuncPtr detour;
  GlEntryIndex index;
};

// The EGL query used to find the calling thread's current context. It is
// resolved directly from libEGL rather than through the hook chain: the
// interceptor calls it on every filtered state call, so it must not itself be
// a detour. It is self-contained here so the GL filter still knows its context
// when the EGL sensor is disabled.
using EglGetCurrentContextFn = EGLContext (*)();

std::uintptr_t queryCurrentContext() {
  static const auto getCurrentContext = []() -> EglGetCurrentContextFn {
    void *handle = dlopen("libEGL.so", RTLD_NOLOAD | RTLD_NOW);
    if (handle == nullptr) {
      handle = dlopen("libEGL.so", RTLD_NOW);
    }
    if (handle == nullptr) {
      return nullptr;
    }
    return reinterpret_cast<EglGetCurrentContextFn>(
        dlsym(handle, "eglGetCurrentContext"));
  }();
  if (getCurrentContext == nullptr) {
    return 0;
  }
  return reinterpret_cast<std::uintptr_t>(getCurrentContext());
}

} // namespace
#endif

void GlInterceptor::noteCall(bool suppressed) {
  mStateCalls.fetch_add(1, std::memory_order_relaxed);
  if (suppressed) {
    mSuppressed.fetch_add(1, std::memory_order_relaxed);
  }
}

GlInterceptor::LiveShadow GlInterceptor::shadow() {
  const std::uintptr_t handle = currentContext();
  if (handle == 0) {
    // No context is known, which is the host test build and any call made
    // outside a current context on device. The single default shadow is used so
    // the filter still runs; on device a state call with no current context is
    // a no-op in the driver regardless.
    return LiveShadow{&mDefaultState, &mDefaultTextures};
  }

  const std::uint64_t generation =
      mContextGeneration.load(std::memory_order_acquire);
  if (mCache.generation == generation && mCache.handle == handle) {
    return LiveShadow{mCache.state, mCache.textures};
  }

  std::lock_guard lock(mContextMutex);
  for (auto &slot : mContexts) {
    if (slot.used && slot.handle == handle) {
      mCache = ShadowCache{generation, handle, &slot.state, &slot.textures};
      return LiveShadow{&slot.state, &slot.textures};
    }
  }

  // A context this mod never saw created, so its real state is unknown. Forcing
  // a reset here would drop every call the game makes against it for one frame,
  // which is how a foreign or loader context would stall the render thread.
  // Leaving the state unshadowed forwards everything instead. The cache keeps
  // the null result so the scan is not repeated on every call either.
  mCache = ShadowCache{generation, handle, nullptr, nullptr};
  return LiveShadow{};
}

void GlInterceptor::noteContextCreated(std::uintptr_t context) {
  if (context == 0) {
    return;
  }
  std::lock_guard lock(mContextMutex);
  ContextSlot *freeSlot = nullptr;
  for (auto &slot : mContexts) {
    if (slot.used && slot.handle == context) {
      return;
    }
    if (!slot.used && freeSlot == nullptr) {
      freeSlot = &slot;
    }
  }
  if (freeSlot == nullptr) {
    // All slots in use. Without evicting a live context the safest degradation
    // is to leave the new context untracked, so its calls are forwarded.
    return;
  }
  freeSlot->used = true;
  freeSlot->handle = context;
  freeSlot->state = freshContextState();
  std::memset(freeSlot->textures.textures, 0, sizeof(freeSlot->textures.textures));
  mContextGeneration.fetch_add(1, std::memory_order_release);
}

void GlInterceptor::noteContextDestroyed(std::uintptr_t context) {
  if (context == 0) {
    return;
  }
  std::lock_guard lock(mContextMutex);
  for (auto &slot : mContexts) {
    if (slot.used && slot.handle == context) {
      slot.used = false;
      slot.handle = 0;
      mContextGeneration.fetch_add(1, std::memory_order_release);
      return;
    }
  }
}

void GlInterceptor::setCurrentContextForTest(std::uintptr_t context) {
  // The test build has no EGL query, so the handle is held per thread. This is
  // what lets a test put two threads on two contexts at once.
  mTestContext = context;
}

#if FPSOPTO_HAVE_PRELOADER

bool GlInterceptor::install(const std::string &moduleName) {
  if (mInstalledCount > 0) {
    return true;
  }

  const HookSpec specs[] = {
      {"glEnable", reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_glEnable), kEntryEnable},
      {"glDisable", reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_glDisable), kEntryDisable},
      {"glBlendFunc", reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_glBlendFunc), kEntryBlendFunc},
      {"glBlendFuncSeparate",
       reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_glBlendFuncSeparate), kEntryBlendFuncSeparate},
      {"glBlendEquation",
       reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_glBlendEquation), kEntryBlendEquation},
      {"glBlendEquationSeparate",
       reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_glBlendEquationSeparate), kEntryBlendEquationSeparate},
      {"glBlendColor", reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_glBlendColor), kEntryBlendColor},
      {"glDepthFunc", reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_glDepthFunc), kEntryDepthFunc},
      {"glDepthMask", reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_glDepthMask), kEntryDepthMask},
      {"glColorMask", reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_glColorMask), kEntryColorMask},
      {"glCullFace", reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_glCullFace), kEntryCullFace},
      {"glFrontFace", reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_glFrontFace), kEntryFrontFace},
      {"glDepthRangef", reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_glDepthRangef), kEntryDepthRangef},
      {"glSampleCoverage",
       reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_glSampleCoverage), kEntrySampleCoverage},
      {"glViewport", reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_glViewport), kEntryViewport},
      {"glUseProgram", reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_glUseProgram), kEntryUseProgram},
      {"glActiveTexture",
       reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_glActiveTexture), kEntryActiveTexture},
      {"glBindBuffer", reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_glBindBuffer), kEntryBindBuffer},
      {"glBindTexture", reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_glBindTexture), kEntryBindTexture},
      {"glBindFramebuffer",
       reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_glBindFramebuffer), kEntryBindFramebuffer},
      {"glBindVertexArray",
       reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_glBindVertexArray), kEntryBindVertexArray},
      {"glPixelStorei", reinterpret_cast<pl::memory::FuncPtr>(&fpsopto_glPixelStorei), kEntryPixelStorei},
  };

  // No shadow is reset here. Every context this mod is asked to track is
  // registered by `noteContextCreated` when eglCreateContext returns, and any
  // context the mod never saw (one that existed before the hooks) is forwarded
  // unfiltered until then.

  setCurrentContextQuery(&queryCurrentContext);

  for (const auto &spec : specs) {
    const std::uintptr_t address =
        pl::memory::resolveSignature(spec.name, moduleName);
    void *original = nullptr;
    if (address != 0 &&
        pl::memory::hook(reinterpret_cast<void *>(address), spec.detour,
                         &original) == 0) {
      // Without the trampoline the detour would call its own address. A hook
      // that did not produce one is treated as failed rather than risking a
      // hang deep inside the driver.
      if (original == nullptr) {
        pl::memory::unhook(reinterpret_cast<void *>(address), spec.detour);
        ++mFailedCount;
        mFailedHooks.emplace_back(spec.name);
        continue;
      }
      gOriginals[spec.index] =
          reinterpret_cast<GlEntryPoint>(original);
      ++mInstalledCount;
    } else {
      ++mFailedCount;
      mFailedHooks.emplace_back(spec.name);
    }
  }

  mInstalled = mInstalledCount > 0;
  return mInstalled;
}

void GlInterceptor::uninstall() {
  // The preloader owns the hook chain and removes it with the library; this
  // only clears the mod's own bookkeeping so no further call is filtered.
  mInstalled = false;
  mInstalledCount = 0;
}

#else

bool GlInterceptor::install(const std::string &) {
  mInstalled = true;
  return true;
}

void GlInterceptor::uninstall() { mInstalled = false; }

#endif

GlInterceptor &GlInterceptor::instance() {
  // A function-local static gives a single process-wide shadow with no
  // initialization-order problem: the mod's first hook may run on any thread.
  static GlInterceptor interceptor;
  return interceptor;
}

} // namespace fpsopto