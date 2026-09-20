#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "GlCounters.h"
#include "GlStateModel.h"

namespace fpsopto {

// Per-thread cache entry for the current context's shadow lookup. Kept at
// namespace scope so it is complete before it is used as a thread-local member.
struct ShadowCache {
  std::uint64_t generation = 0;
  std::uintptr_t handle = 0;
  GlStateModel *state = nullptr;
  TextureUnits *textures = nullptr;
};

// Installs detours on the GL ES state entry points and keeps a shadow of the
// context so redundant calls can be dropped.
//
// Installs every hook it can and reports which ones failed, so a device whose
// driver exports only a subset still gets that subset.
//
// The shadow is per rendering context. It is learned lazily: the shadow for a
// context is only trusted once this mod has observed that context being used,
// which is exactly the state `freshContextState()` returns.
// The live shadow is the one for the context the calling thread has current.
// Because a thread's current context can be created by another thread, the
// shadow lookup goes through the EGL current-context query rather than relying
// on which thread happened to install or reset it.
//
// Individual hooks can be switched off at runtime, which is what the auto mode
// uses when a hook's drop rate does not justify its indirection. A disabled
// hook is still installed -- removing it mid-frame would race the render
// thread -- but its detour forwards immediately.
class GlInterceptor {
public:
  static GlInterceptor &instance();

  // `moduleName` is the shared object exporting the GL entry points, normally
  // "libGLESv2.so".
  bool install(const std::string &moduleName);
  void uninstall();

  [[nodiscard]] bool installed() const { return mInstalledCount > 0; }
  [[nodiscard]] int installedCount() const { return mInstalledCount; }
  [[nodiscard]] int failedCount() const { return mFailedCount; }
  [[nodiscard]] const std::vector<std::string> &failedHooks() const {
    return mFailedHooks;
  }

  // Snapshot of the traffic counters; safe to copy from any thread.
  [[nodiscard]] GlTrafficSnapshot traffic() const {
    GlTrafficSnapshot out = GlCounters::instance().snapshot();
    for (std::size_t i = 0; i < kGlHookCount; ++i) {
      if (!mHookEnabled[i].load(std::memory_order_relaxed)) {
        ++out.hooksDisabled;
      }
    }
    return out;
  }

  // Records one call that passed through the filter. Called by the detours, so
  // it is a plain increment on thread-local storage rather than an atomic.
  void noteCall(GlHook hook, bool suppressed) {
    GlCounters::instance().note(hook, suppressed);
  }

  // Whether a hook is currently allowed to drop a call it recognises. A
  // disabled hook still updates the shadow -- other hooks read the fields it
  // maintains -- but forwards every call, so it stops costing the drop path.
  [[nodiscard]] bool hookEnabled(GlHook hook) const {
    return mHookEnabled[static_cast<std::size_t>(hook)].load(
        std::memory_order_relaxed);
  }

  void setHookEnabled(GlHook hook, bool enabled) {
    mHookEnabled[static_cast<std::size_t>(hook)].store(
        enabled, std::memory_order_relaxed);
  }

  // Turns every hook's filtering off (or back on). Used by safe mode, which
  // wants the hooks installed but inert rather than absent.
  void setAllHooksEnabled(bool enabled) {
    for (std::size_t i = 0; i < kGlHookCount; ++i) {
      mHookEnabled[i].store(enabled, std::memory_order_relaxed);
    }
  }

  // The context the calling thread has current, or 0 when none is. Set once at
  // install time from the EGL entry point and called on every filtered state
  // call, so the entry point it wraps must not itself be hooked.
  using CurrentContextFn = std::uintptr_t (*)();
  void setCurrentContextQuery(CurrentContextFn query) {
    mCurrentContext.store(query, std::memory_order_release);
  }

  // Registers a context whose shadow should start at the spec defaults. Called
  // with the handle returned by eglCreateContext, before it can become current.
  void noteContextCreated(std::uintptr_t context);

  // Drops the recorded shadow for a context that is being destroyed.
  void noteContextDestroyed(std::uintptr_t context);

  // The shadow of the context the calling thread has current. `state` is null
  // when that context is one this mod must not filter -- one it never saw
  // created -- in which case the caller forwards the call unfiltered.
  struct LiveShadow {
    GlStateModel *state = nullptr;
    TextureUnits *textures = nullptr;
    explicit operator bool() const { return state != nullptr; }
  };
  LiveShadow shadow();

  // Test-only: read the raw shadow struct for the current context, so the
  // drift check and the tests can compare it against real state.
  [[nodiscard]] const GlStateModel *shadowStateForTest();

  // Test-only: route the current-context query to a caller-supplied handle. A
  // handle of 0 restores the single no-context shadow used before any context
  // has been registered.
  void setCurrentContextForTest(std::uintptr_t context);

private:
  static constexpr std::size_t kMaxContexts = 64;

  struct ContextSlot {
    std::uintptr_t handle = 0;
    bool used = false;
    GlStateModel state = freshContextState();
    TextureUnits textures{};
  };

  GlInterceptor() {
    // Filtering is on from construction. Whether the hooks are actually in
    // place is a separate question (`install`), and the policy that switches a
    // hook off runs after install, so this is the neutral starting point.
    for (std::size_t i = 0; i < kGlHookCount; ++i) {
      mHookEnabled[i].store(true, std::memory_order_relaxed);
    }
  }

  std::uintptr_t currentContext() const {
    if (const CurrentContextFn query =
            mCurrentContext.load(std::memory_order_acquire)) {
      return query();
    }
    return mTestContext;
  }

  std::atomic<CurrentContextFn> mCurrentContext{nullptr};
  // Host-build only: the handle `setCurrentContextForTest` makes current on the
  // calling thread. On device the EGL query is installed at enable time and this
  // stays 0.
  static inline thread_local std::uintptr_t mTestContext = 0;
  // Remembers the last lookup on this thread so the common case -- the same
  // context call after call -- does not take the table lock. `generation` is
  // compared against `mContextGeneration`, which is bumped on every create or
  // destroy, so a cached pointer can never outlive the slot it points into.
  static inline thread_local ShadowCache mCache;
  ContextSlot mContexts[kMaxContexts]{};
  GlStateModel mDefaultState = freshContextState();
  TextureUnits mDefaultTextures{};

  // Guards the slot table, which a thread creating or destroying a context can
  // touch while another thread filters calls. The shadow fields themselves are
  // read without it: a filter decision only touches the current context's own
  // state, which one thread at a time drives because GL is current there.
  std::mutex mContextMutex;
  std::atomic<std::uint64_t> mContextGeneration{0};

  std::atomic<bool> mHookEnabled[kGlHookCount] = {};

  bool mInstalled = false;
  int mInstalledCount = 0;
  int mFailedCount = 0;
  std::vector<std::string> mFailedHooks;
};

} // namespace fpsopto
