#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>

// Not every NDK EGL header set spells these typedefs out, so they are declared
// against the core signatures rather than assumed to exist.
#ifndef FPSOPTO_EGL_CURRENT_PFN_DECLARED
#define FPSOPTO_EGL_CURRENT_PFN_DECLARED
typedef EGLDisplay(EGLAPIENTRY *PFNEGLGETCURRENTDISPLAYPROC)(void);
typedef EGLContext(EGLAPIENTRY *PFNEGLGETCURRENTCONTEXTPROC)(void);
#endif

#ifndef FPSOPTO_EGL_CORE_PFN_DECLARED
#define FPSOPTO_EGL_CORE_PFN_DECLARED
typedef EGLBoolean(EGLAPIENTRY *PFNEGLSWAPBUFFERSPROC)(EGLDisplay, EGLSurface);
typedef EGLContext(EGLAPIENTRY *PFNEGLCREATECONTEXTPROC)(EGLDisplay, EGLConfig,
                                                         EGLContext,
                                                         const EGLint *);
typedef EGLBoolean(EGLAPIENTRY *PFNEGLMAKECURRENTPROC)(EGLDisplay, EGLSurface,
                                                       EGLSurface, EGLContext);
typedef EGLBoolean(EGLAPIENTRY *PFNEGLDESTROYCONTEXTPROC)(EGLDisplay,
                                                          EGLContext);
typedef EGLBoolean(EGLAPIENTRY *PFNEGLSWAPINTERVALPROC)(EGLDisplay, EGLint);
#endif

#include <cstdint>
#include <string>
#include <vector>

#include "FrameTelemetry.h"

namespace fpsopto {

struct FrameStats;

// Invoked once per presented frame. `paced` reports whether the swap chain is
// currently the thing limiting the frame rate.
using FrameCallback = void (*)(const FrameStats &stats, double cadenceMs,
                               bool pacedMs, void *user);

// Observes the buffer-swap boundary. Strictly read-only: it never changes what
// is drawn, or when it is presented.
class EglSensor {
public:
  static EglSensor &instance();

  // `moduleName` is the shared object exporting the EGL entry points, normally
  // "libEGL.so".
  bool install(const std::string &moduleName);

  // The callback must stay valid until it is replaced or cleared, and must not
  // throw or block: it runs on the game's render thread inside the swap call.
  void setFrameCallback(FrameCallback callback, void *user);

  [[nodiscard]] bool installed() const { return mInstalledCount > 0; }
  [[nodiscard]] int installedCount() const { return mInstalledCount; }
  [[nodiscard]] const std::vector<std::string> &failedHooks() const {
    return mFailedHooks;
  }

  [[nodiscard]] const FrameAccumulator<> &accumulator() const {
    return mFrames;
  }

  // Called by the swap detours. `cpuNs` is the time the swap call itself took,
  // which is where a driver blocks when the GPU is behind.
  void beginFrame();
  void endFrame(std::uint64_t cpuNs, bool gpuMeasured, std::uint64_t gpuNs,
                bool gpuOverran);

  // Number of swaps since the last context change, used by the governor to
  // know whether the window is actually presenting.
  [[nodiscard]] std::uint64_t swapCount() const { return mSwapCount; }

  // Swap interval as reported by eglSwapInterval; 1 when never observed.
  [[nodiscard]] int swapInterval() const { return mSwapInterval; }

  void noteSwapInterval(int interval);

  [[nodiscard]] bool hasPresented() const { return mSwapCount > 0; }

private:
  EglSensor() = default;

  bool mInstalled = false;
  int mInstalledCount = 0;
  std::vector<std::string> mFailedHooks;

  FrameAccumulator<> mFrames{};
  std::uint64_t mFrameStartNs = 0;
  std::uint64_t mLastSwapNs = 0;
  std::uint64_t mTotalCpuNs = 0;
  std::uint64_t mSwapCount = 0;
  int mSwapInterval = 1;
  FrameCallback mFrameCallback = nullptr;
  void *mFrameCallbackUser = nullptr;
};

} // namespace fpsopto