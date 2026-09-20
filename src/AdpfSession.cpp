#include "AdpfSession.h"

#include <dlfcn.h>
#include <unistd.h>

namespace fpsopto {
namespace {

// APerformanceHint is a C API that only exists from Android 12 (API 31), and
// the version shipped in the NDK is gated behind newer API levels than the
// launcher's minimum. It is therefore loaded dynamically, which also means a
// device without the library degrades to "no session" instead of failing to
// load the mod.

struct APerformanceHintManager;
struct APerformanceHintSession;

using GetManagerFn = APerformanceHintManager *(*)();
using CreateSessionFn = APerformanceHintSession *(*)(APerformanceHintManager *,
                                                     const int32_t *, size_t,
                                                     int64_t);
using UpdateTargetFn = int (*)(APerformanceHintSession *, int64_t);
using ReportActualFn = int (*)(APerformanceHintSession *, int64_t);
using CloseSessionFn = void (*)(APerformanceHintSession *);
using SetPreferEfficiencyFn = int (*)(APerformanceHintSession *, bool);

void *gLibrary = nullptr;
GetManagerFn gGetManager = nullptr;
CreateSessionFn gCreateSession = nullptr;
UpdateTargetFn gUpdateTarget = nullptr;
ReportActualFn gReportActual = nullptr;
CloseSessionFn gCloseSession = nullptr;
SetPreferEfficiencyFn gSetPreferEfficiency = nullptr;

bool loadLibrary() {
  if (gLibrary != nullptr) {
    return true;
  }
  // NDK ABI is unchanged across releases, so the versionless soname is what
  // the platform ships.
  gLibrary = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
  if (gLibrary == nullptr) {
    return false;
  }
  gGetManager = reinterpret_cast<GetManagerFn>(
      dlsym(gLibrary, "APerformanceHint_getManager"));
  gCreateSession = reinterpret_cast<CreateSessionFn>(
      dlsym(gLibrary, "APerformanceHint_createSession"));
  gUpdateTarget = reinterpret_cast<UpdateTargetFn>(
      dlsym(gLibrary, "APerformanceHint_updateTargetWorkDuration"));
  gReportActual = reinterpret_cast<ReportActualFn>(
      dlsym(gLibrary, "APerformanceHint_reportActualWorkDuration"));
  gCloseSession = reinterpret_cast<CloseSessionFn>(
      dlsym(gLibrary, "APerformanceHint_closeSession"));
  gSetPreferEfficiency = reinterpret_cast<SetPreferEfficiencyFn>(
      dlsym(gLibrary, "APerformanceHint_setPreferPowerEfficiency"));

  return gGetManager != nullptr && gCreateSession != nullptr &&
         gUpdateTarget != nullptr && gReportActual != nullptr &&
         gCloseSession != nullptr;
}

APerformanceHintManager *gManager = nullptr;
APerformanceHintSession *gSession = nullptr;

} // namespace

bool AdpfSession::start(int targetFps) {
  if (mActive) {
    return true;
  }
  if (!loadLibrary()) {
    return false;
  }

  gManager = gGetManager();
  if (gManager == nullptr) {
    return false;
  }

  if (targetFps <= 0) {
    return false;
  }

  // The session is created over the calling thread. The scheduler widens it to
  // the render group once those threads exist, so creating it here is only the
  // first step and is safe even before the game has forked its workers.
  const int32_t threadIds[] = {static_cast<int32_t>(gettid())};
  mTargetNs = 1000000000ull / static_cast<std::uint64_t>(targetFps);

  gSession = gCreateSession(gManager, threadIds, 1,
                            static_cast<int64_t>(mTargetNs));
  if (gSession == nullptr) {
    return false;
  }

  // Ask for the efficiency-leaning operating points. On a thermally limited
  // device the sustained frame rate is decided by how long the peak clocks
  // survive, so favouring efficiency is what keeps the late-session frames
  // close to the early ones. Advisory: a platform that ignores it is fine.
  if (gSetPreferEfficiency != nullptr) {
    gSetPreferEfficiency(gSession, true);
  }
  mPreferEfficiency = true;

  mActive = true;
  return true;
}

void AdpfSession::stop() {
  if (!mActive) {
    return;
  }
  if (gCloseSession != nullptr && gSession != nullptr) {
    gCloseSession(gSession);
  }
  gSession = nullptr;
  mActive = false;
  mPreferEfficiency = false;
}

void AdpfSession::reportFrame(std::uint64_t actualDurationNs) {
  if (!mActive || gReportActual == nullptr || gSession == nullptr) {
    return;
  }
  gReportActual(gSession, static_cast<int64_t>(actualDurationNs));
}

void AdpfSession::updateTarget(std::uint64_t targetDurationNs) {
  if (!mActive || gUpdateTarget == nullptr || gSession == nullptr) {
    return;
  }
  if (targetDurationNs == 0 || targetDurationNs == mTargetNs) {
    return;
  }
  mTargetNs = targetDurationNs;
  gUpdateTarget(gSession, static_cast<int64_t>(targetDurationNs));
}

AdpfSession &AdpfSession::instance() {
  static AdpfSession session;
  return session;
}

} // namespace fpsopto