// NOTE: this translation unit must also be compiled into the host test binary,
// where PL_INTERPOSER_HOST_TEST keeps the Android/GL/preloader dependencies out
// and the `gl*` call-through names become the test's probe functions.

#include "GlInterceptor.h"

#include <array>
#include <cstring>
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
  const std::uint32_t bit = capabilityBit(capability);
  if (bit != 0 && (self.mState.enabled & bit) != 0) {
    record(true);
    return;
  }
  record(false);
  if (bit != 0) {
    self.mState.enabled |= bit;
  }
  FPSOPTO_ORIGINAL(kEntryEnable, glEnable)(capability);
}

void fpsopto_glDisable(unsigned int capability) {
  auto &self = GlInterceptor::instance();
  const std::uint32_t bit = capabilityBit(capability);
  if (bit != 0 && (self.mState.enabled & bit) == 0) {
    record(true);
    return;
  }
  record(false);
  if (bit != 0) {
    self.mState.enabled &= ~bit;
  }
  FPSOPTO_ORIGINAL(kEntryDisable, glDisable)(capability);
}

void fpsopto_glBlendFunc(unsigned int src, unsigned int dst) {
  auto &self = GlInterceptor::instance();
  if (sameBlend(self.mState, src, dst, src, dst)) {
    record(true);
    return;
  }
  record(false);
  self.mState.blendSrcRgb = src;
  self.mState.blendDstRgb = dst;
  self.mState.blendSrcAlpha = src;
  self.mState.blendDstAlpha = dst;
  FPSOPTO_ORIGINAL(kEntryBlendFunc, glBlendFunc)(src, dst);
}

void fpsopto_glBlendFuncSeparate(unsigned int srcRgb, unsigned int dstRgb,
                                 unsigned int srcAlpha, unsigned int dstAlpha) {
  auto &self = GlInterceptor::instance();
  if (sameBlend(self.mState, srcRgb, dstRgb, srcAlpha, dstAlpha)) {
    record(true);
    return;
  }
  record(false);
  self.mState.blendSrcRgb = srcRgb;
  self.mState.blendDstRgb = dstRgb;
  self.mState.blendSrcAlpha = srcAlpha;
  self.mState.blendDstAlpha = dstAlpha;
  FPSOPTO_ORIGINAL(kEntryBlendFuncSeparate, glBlendFuncSeparate)(srcRgb, dstRgb, srcAlpha, dstAlpha);
}

void fpsopto_glBlendEquation(unsigned int mode) {
  auto &self = GlInterceptor::instance();
  if (self.mState.blendEquationRgb == mode &&
      self.mState.blendEquationAlpha == mode) {
    record(true);
    return;
  }
  record(false);
  self.mState.blendEquationRgb = mode;
  self.mState.blendEquationAlpha = mode;
  FPSOPTO_ORIGINAL(kEntryBlendEquation, glBlendEquation)(mode);
}

void fpsopto_glBlendEquationSeparate(unsigned int rgb, unsigned int alpha) {
  auto &self = GlInterceptor::instance();
  if (self.mState.blendEquationRgb == rgb &&
      self.mState.blendEquationAlpha == alpha) {
    record(true);
    return;
  }
  record(false);
  self.mState.blendEquationRgb = rgb;
  self.mState.blendEquationAlpha = alpha;
  FPSOPTO_ORIGINAL(kEntryBlendEquationSeparate, glBlendEquationSeparate)(rgb, alpha);
}

void fpsopto_glBlendColor(float r, float g, float b, float a) {
  auto &self = GlInterceptor::instance();
  if (sameBlendColor(self.mState, r, g, b, a)) {
    record(true);
    return;
  }
  record(false);
  self.mState.blendColorR = r;
  self.mState.blendColorG = g;
  self.mState.blendColorB = b;
  self.mState.blendColorA = a;
  FPSOPTO_ORIGINAL(kEntryBlendColor, glBlendColor)(r, g, b, a);
}

void fpsopto_glDepthFunc(unsigned int func) {
  auto &self = GlInterceptor::instance();
  if (self.mState.depthFunc == func) {
    record(true);
    return;
  }
  record(false);
  self.mState.depthFunc = func;
  FPSOPTO_ORIGINAL(kEntryDepthFunc, glDepthFunc)(func);
}

void fpsopto_glDepthMask(unsigned char flag) {
  auto &self = GlInterceptor::instance();
  const std::uint8_t normalized = flag ? 1 : 0;
  if (self.mState.depthMask == normalized) {
    record(true);
    return;
  }
  record(false);
  self.mState.depthMask = normalized;
  FPSOPTO_ORIGINAL(kEntryDepthMask, glDepthMask)(flag);
}

void fpsopto_glColorMask(unsigned char r, unsigned char g, unsigned char b,
                         unsigned char a) {
  auto &self = GlInterceptor::instance();
  const std::uint32_t nr = r ? 1u : 0u;
  const std::uint32_t ng = g ? 1u : 0u;
  const std::uint32_t nb = b ? 1u : 0u;
  const std::uint32_t na = a ? 1u : 0u;
  if (sameColorMask(self.mState, nr, ng, nb, na)) {
    record(true);
    return;
  }
  record(false);
  self.mState.colorMaskR = nr;
  self.mState.colorMaskG = ng;
  self.mState.colorMaskB = nb;
  self.mState.colorMaskA = na;
  FPSOPTO_ORIGINAL(kEntryColorMask, glColorMask)(r, g, b, a);
}

void fpsopto_glCullFace(unsigned int mode) {
  auto &self = GlInterceptor::instance();
  if (self.mState.cullFaceMode == mode) {
    record(true);
    return;
  }
  record(false);
  self.mState.cullFaceMode = mode;
  FPSOPTO_ORIGINAL(kEntryCullFace, glCullFace)(mode);
}

void fpsopto_glFrontFace(unsigned int mode) {
  auto &self = GlInterceptor::instance();
  if (self.mState.frontFace == mode) {
    record(true);
    return;
  }
  record(false);
  self.mState.frontFace = mode;
  FPSOPTO_ORIGINAL(kEntryFrontFace, glFrontFace)(mode);
}

void fpsopto_glDepthRangef(float near, float far) {
  auto &self = GlInterceptor::instance();
  if (sameDepthRange(self.mState, near, far)) {
    record(true);
    return;
  }
  record(false);
  self.mState.depthRangeNear = near;
  self.mState.depthRangeFar = far;
  FPSOPTO_ORIGINAL(kEntryDepthRangef, glDepthRangef)(near, far);
}

void fpsopto_glSampleCoverage(float value, unsigned char invert) {
  auto &self = GlInterceptor::instance();
  const std::uint8_t normalized = invert ? 1 : 0;
  if (sameSampleCoverage(self.mState, value, normalized)) {
    record(true);
    return;
  }
  record(false);
  self.mState.sampleCoverageValue = value;
  self.mState.sampleCoverageInvert = normalized;
  FPSOPTO_ORIGINAL(kEntrySampleCoverage, glSampleCoverage)(value, invert);
}

void fpsopto_glViewport(int x, int y, int w, int h) {
  auto &self = GlInterceptor::instance();
  if (sameViewport(self.mState, x, y, w, h)) {
    record(true);
    return;
  }
  record(false);
  self.mState.viewportX = x;
  self.mState.viewportY = y;
  self.mState.viewportW = w;
  self.mState.viewportH = h;
  FPSOPTO_ORIGINAL(kEntryViewport, glViewport)(x, y, w, h);
}

void fpsopto_glUseProgram(unsigned int program) {
  auto &self = GlInterceptor::instance();
  if (sameBinding(self.mState.currentProgram, program)) {
    record(true);
    return;
  }
  record(false);
  self.mState.currentProgram = program;
  FPSOPTO_ORIGINAL(kEntryUseProgram, glUseProgram)(program);
}

void fpsopto_glActiveTexture(unsigned int texture) {
  auto &self = GlInterceptor::instance();
  if (self.mState.activeTexture == texture) {
    record(true);
    return;
  }
  record(false);
  self.mState.activeTexture = texture;
  FPSOPTO_ORIGINAL(kEntryActiveTexture, glActiveTexture)(texture);
}

void fpsopto_glBindBuffer(unsigned int target, unsigned int buffer) {
  auto &self = GlInterceptor::instance();
  std::uint32_t *shadow = nullptr;
  if (target == kGlArrayBuffer) {
    shadow = &self.mState.arrayBuffer;
  } else if (target == kGlElementArrayBuffer) {
    shadow = &self.mState.elementArrayBuffer;
  } else if (target == kGlPixelUnpackBuffer) {
    shadow = &self.mState.pixelUnpackBuffer;
  } else if (target == kGlPixelPackBuffer) {
    shadow = &self.mState.pixelPackBuffer;
  }

  if (shadow != nullptr && sameBinding(*shadow, buffer)) {
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
  const int unitIndex = TextureUnits::unitOf(self.mState.activeTexture);
  const int slot = textureTargetSlot(target);
  const bool tracked = unitIndex >= 0 && slot != 0;

  // Binding is only replaceable for a target whose slot can be addressed on the
  // active unit. A stale entry would resample the wrong image, so an untracked
  // target is always forwarded.
  if (tracked) {
    std::uint32_t &shadow = self.mTextures.textures[unitIndex][slot];
    if (sameBinding(shadow, texture)) {
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
  const bool draw = target == kGlFramebuffer || target == kGlDrawFramebuffer;
  if (draw && self.mState.framebuffer == framebuffer) {
    record(true);
    return;
  }
  record(false);
  if (draw) {
    self.mState.framebuffer = framebuffer;
  }
  FPSOPTO_ORIGINAL(kEntryBindFramebuffer, glBindFramebuffer)(target, framebuffer);
}

void fpsopto_glBindVertexArray(unsigned int array) {
  auto &self = GlInterceptor::instance();
  if (self.mState.vertexArray == array) {
    record(true);
    return;
  }
  record(false);
  self.mState.vertexArray = array;
  FPSOPTO_ORIGINAL(kEntryBindVertexArray, glBindVertexArray)(array);
}

void fpsopto_glPixelStorei(unsigned int pname, int param) {
  auto &self = GlInterceptor::instance();
  std::uint32_t *shadow = nullptr;
  if (pname == kGlUnpackAlignment) {
    shadow = &self.mState.unpackAlignment;
  } else if (pname == kGlPackAlignment) {
    shadow = &self.mState.packAlignment;
  } else if (pname == kGlUnpackRowLength) {
    shadow = &self.mState.unpackRowLength;
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

} // namespace
#endif

void GlInterceptor::onContextChanged() {
  mState = freshContextState();
  std::memset(mTextures.textures, 0, sizeof(mTextures.textures));
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

  onContextChanged();

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
  onContextChanged();
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