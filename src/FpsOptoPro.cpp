#include "FpsConfig.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>

#include "AdpfSession.h"
#include "AutoPolicy.h"
#include "BootGuard.h"
#include "BootMarker.h"
#include "Diagnostics.h"
#include "EglSensor.h"
#include "GlCounters.h"
#include "GlInterceptor.h"
#include "ThreadScheduler.h"

namespace {

using fpsopto::AdpfSession;
using fpsopto::AutoAction;
using fpsopto::AutoInput;
using fpsopto::AutoMode;
using fpsopto::BootMarker;
using fpsopto::Diagnostics;
using fpsopto::EglSensor;
using fpsopto::FrameStats;
using fpsopto::FpsConfig;
using fpsopto::GlCounters;
using fpsopto::GlHook;
using fpsopto::GlInterceptor;
using fpsopto::kGlHookCount;
using fpsopto::ThreadScheduler;
using fpsopto::Workload;

constexpr const char *kModuleId = "fps_opto_pro.core";
constexpr const char *kEnableGlFilterKey = "enableGlFilter";
constexpr const char *kEnableSchedulerKey = "enableScheduler";
constexpr const char *kWorkloadKey = "workload";
constexpr const char *kTargetFpsKey = "targetFps";
constexpr const char *kMaxBoostKey = "maxWorkerBoost";
constexpr const char *kLogStatsKey = "logStats";
constexpr const char *kShowOverlayKey = "showOverlay";
constexpr const char *kAutoModeKey = "autoMode";

// How often the auto mode re-reads the per-hook drop rates. Frequent enough to
// react to a scene change, rare enough that the review never lands inside the
// frame budget it is trying to protect.
constexpr std::uint64_t kHookReviewIntervalNs = 3ull * 1000ull * 1000ull * 1000ull;

// The overlay is diagnostic text, so a handful of updates a second is plenty
// and does not itself become a frame-time cost.
constexpr int kOverlayFramesPerUpdate = 6;

// Shared objects carrying the graphics entry points on every Android build.
constexpr const char *kGlesModule = "libGLESv2.so";
constexpr const char *kEglModule = "libEGL.so";

std::uint64_t nowNs() {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(
      duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
          .count());
}

bool parseBool(std::string_view value, bool fallback) {
  if (value == "true" || value == "1" || value == "on" || value == "enabled") {
    return true;
  }
  if (value == "false" || value == "0" || value == "off" ||
      value == "disabled") {
    return false;
  }
  return fallback;
}

int parseInt(std::string_view value, int fallback) {
  std::string text(value);
  char *end = nullptr;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0') {
    return fallback;
  }
  return static_cast<int>(parsed);
}

std::string boolToMenuValue(bool value) { return value ? "true" : "false"; }

// The Mod Menu radio control reports the selected option's index, so both the
// index and the option name are accepted; anything else keeps the current
// value rather than silently snapping to the first option.
Workload parseWorkload(std::string_view value, Workload fallback) {
  if (value == "Auto" || value == "0") {
    return Workload::Auto;
  }
  if (value == "GpuBound" || value == "1") {
    return Workload::GpuBound;
  }
  if (value == "CpuBound" || value == "2") {
    return Workload::CpuBound;
  }
  return fallback;
}

int workloadToIndex(Workload workload) {
  switch (workload) {
  case Workload::Auto:
    return 0;
  case Workload::GpuBound:
    return 1;
  case Workload::CpuBound:
    return 2;
  }
  return 0;
}

std::string joinNames(const std::vector<std::string> &names) {
  std::string out;
  for (const auto &name : names) {
    if (!out.empty()) {
      out += ", ";
    }
    out += name;
  }
  return out.empty() ? "none" : out;
}

class FpsOptoPro {
public:
  static FpsOptoPro &instance() {
    static FpsOptoPro mod;
    return mod;
  }

  FpsOptoPro() : mSelf(*ll::mod::NativeMod::current()) {}

  [[nodiscard]] ll::mod::NativeMod &getSelf() const { return mSelf; }

  bool load() {
    auto &self = getSelf();
    std::lock_guard lock(mConfigMutex);
    mConfigFile.emplace();
    if (!mConfigFile->load()) {
      self.getLogger().error("Failed to load config");
      mConfigFile.reset();
      return false;
    }

    normalize(mConfigFile->value());
    if (!mConfigFile->save()) {
      self.getLogger().warn("Could not persist normalised config");
    }

    self.getLogger().info("Loaded FPS-OPTO-PRO {}", self.getVersion());
    return true;
  }

  bool enable() {
    const FpsConfig config = snapshot();

    // The crash-loop guard runs before anything else is installed. If the
    // previous launch never reached steady state, this one keeps every hook
    // installed but inert, which is the only state the user can recover from.
    beginBootGuard();

    // Ordering matters. The game has not started its renderer yet at this
    // point, so the GL filter is in place before the first context exists and
    // the EGL sensor is watching before the first buffer is swapped.
    if (config.enableGlFilter) {
      installGlFilter();
    }
    if (config.enableEglSensor) {
      installEglSensor();
    }

    auto &scheduler = ThreadScheduler::instance();
    scheduler.setMaxBoost(config.maxWorkerBoost);
    if (config.enableScheduler) {
      scheduler.start();
    }

    if (config.allowAdpfSession) {
      const int target = config.targetFps > 0 ? config.targetFps : 60;
      if (!AdpfSession::instance().start(target)) {
        getSelf().getLogger().info(
            "No ADPF performance hint manager on this device; running without "
            "a hint session");
      }
    }

    {
      std::lock_guard lock(mConfigMutex);
      mAutoModeEnabled = config.autoMode;
      mOverlayEnabled = config.showOverlay;
    }
    applyAutoMode();
    applyLogging(config);
    registerMenu(config);
    return true;
  }

  bool disable() {
    mOverlayEnabled.store(false, std::memory_order_relaxed);
    ThreadScheduler::instance().stop();
    AdpfSession::instance().stop();
    EglSensor::instance().setFrameCallback(nullptr, nullptr);
    GlInterceptor::instance().uninstall();
    pl::modmenu::unregisterModule(kModuleId);
    {
      std::lock_guard lock(mConfigMutex);
      mLogging = false;
    }
    getSelf().getLogger().info("Disabled");
    return true;
  }

  bool unload() {
    disable();
    std::lock_guard lock(mConfigMutex);
    mConfigFile.reset();
    return true;
  }

private:
  ll::mod::NativeMod &mSelf;
  std::mutex mConfigMutex;
  std::optional<pl::config::ConfigFile<FpsConfig>> mConfigFile;
  std::atomic_bool mLogging{false};
  std::uint64_t mNextLogNs = 0;

  // Auto mode state. `mAutoModeEnabled` mirrors the config so the render
  // thread can read it without the config mutex; the rest is only touched from
  // the frame callback, which runs on the render thread inside the swap.
  std::atomic_bool mAutoModeEnabled{true};
  AutoMode mAutoMode;
  std::uint64_t mNextHookReviewNs = 0;
  bool mHookEnabledState[kGlHookCount] = {};

  // Overlay state. The flag is read on the render thread and written by the
  // menu; the snapshot itself is guarded because taking the overlay down from
  // the menu races the per-frame update.
  std::atomic_bool mOverlayEnabled{false};
  std::mutex mOverlayMutex;
  int mOverlayFrameCounter = 0;
  Diagnostics mDiagnostics;

  // Start of this launch, for the crash-loop guard's stability test.
  const std::uint64_t mStartNs = nowNs();

  static void normalize(FpsConfig &config) {
    const FpsConfig defaults;
    config.version = defaults.version;
    config.minTargetFps = std::clamp(config.minTargetFps,
                                     fpsopto::kMinTargetFps,
                                     fpsopto::kMaxTargetFps);
    config.maxTargetFps = std::clamp(config.maxTargetFps,
                                     fpsopto::kMinTargetFps,
                                     fpsopto::kMaxTargetFps);
    if (config.minTargetFps > config.maxTargetFps) {
      std::swap(config.minTargetFps, config.maxTargetFps);
    }
    config.targetFps = std::clamp(config.targetFps, 0, fpsopto::kMaxTargetFps);
    config.maxWorkerBoost = std::clamp(config.maxWorkerBoost,
                                       fpsopto::kMinWorkerBoost,
                                       fpsopto::kMaxWorkerBoost);
    config.logIntervalSeconds =
        std::clamp(config.logIntervalSeconds, fpsopto::kMinLogInterval,
                   fpsopto::kMaxLogInterval);
  }

  FpsConfig snapshot() {
    std::lock_guard lock(mConfigMutex);
    if (!mConfigFile) {
      return FpsConfig{};
    }
    auto copy = mConfigFile->value();
    normalize(copy);
    return copy;
  }

  void installGlFilter() {
    auto &gl = GlInterceptor::instance();
    if (!gl.install(kGlesModule)) {
      getSelf().getLogger().warn(
          "GL filter could not attach to {}; state calls are unchanged",
          kGlesModule);
      return;
    }
    getSelf().getLogger().info("GL filter active: {} hooks installed, {} unavailable ({})",
                               gl.installedCount(), gl.failedCount(),
                               joinNames(gl.failedHooks()));
  }

  void installEglSensor() {
    auto &egl = EglSensor::instance();
    if (!egl.install(kEglModule)) {
      getSelf().getLogger().warn(
          "Frame sensor could not attach to {}; scheduling stays in its "
          "baseline state",
          kEglModule);
      return;
    }

    // The hook invokes this on the render thread inside the swap call.
    egl.setFrameCallback(&FpsOptoPro::onFrame, nullptr);
    getSelf().getLogger().info("Frame sensor active: {} hooks",
                               egl.installedCount());
  }

  [[nodiscard]] std::uint64_t uptimeNs() const {
    const std::uint64_t now = nowNs();
    return now > mStartNs ? now - mStartNs : 0;
  }

  static void onFrame(const FrameStats &stats, double cadenceMs, bool paced,
                      void *) {
    auto &self = instance();

    auto &scheduler = ThreadScheduler::instance();
    if (scheduler.running() && stats.frames > 0) {
      // The accumulator has already folded overrun events into its GPU
      // votes, so the per-frame overrun flag is not needed here.
      scheduler.onFrame(stats.frameMs, cadenceMs, stats.cpuLoad, stats.gpuLoad,
                        stats.gpuTiming, false, paced, stats.frames > 240);
    }

    // Feed the platform the frame time it actually got. Without this the hint
    // session has no idea whether its target is being met, and never moves the
    // operating point. The hint is exactly the advertised target while the
    // display paces the frame, otherwise the measured frame time.
    auto &adpf = AdpfSession::instance();
    if (adpf.active()) {
      const std::uint64_t measuredNs = static_cast<std::uint64_t>(
          stats.frameMs * 1000000.0);
      const std::uint64_t reportedNs =
          paced && cadenceMs > 0.0
              ? static_cast<std::uint64_t>(cadenceMs * 1000000.0)
              : measuredNs;
      if (reportedNs != 0) {
        adpf.reportFrame(reportedNs);
      }
    }

    // A launch that has presented enough frames over enough time has proven
    // it does not crash on start-up, which is what retires the boot marker.
    if (!BootMarker::instance().began() ||
        !BootMarker::instance().safeMode()) {
      BootMarker::instance().noteFrame(self.uptimeNs(), stats.frames);
    }

    // Every thread's counter batch is folded into the shared totals once per
    // frame, so a worker that goes quiet does not leave its last few calls
    // unaccounted for in the readout.
    GlCounters::instance().publish();

    self.updateAutoMode(stats, paced);
    self.updateOverlay(stats, paced);

    if (self.mLogging.load(std::memory_order_relaxed)) {
      self.maybeLog(stats);
    }
  }

  // Installs the crash-loop guard and, when the previous launch did not reach
  // steady state, leaves every hook inert. Called before any hook is installed
  // so a bad hook cannot take out the launch that reports it.
  void beginBootGuard() {
    std::filesystem::path dataDir;
    {
      std::lock_guard lock(mConfigMutex);
      if (mConfigFile) {
        dataDir = mConfigFile->configPath().parent_path();
      }
    }

    auto &marker = BootMarker::instance();
    if (dataDir.empty()) {
      // No config path means no durable place for the marker, so there is no
      // handshake to run. Normal launch, nothing to recover from.
      return;
    }

    const auto decision = marker.begin(dataDir);
    if (decision.outcome == fpsopto::BootOutcome::SafeMode) {
      GlInterceptor::instance().setAllHooksEnabled(false);
      ThreadScheduler::instance().stop();
      AdpfSession::instance().stop();
      getSelf().getLogger().warn(
          "Previous launch did not reach steady state ({} in a row); starting "
          "in safe mode with the hooks installed but inert",
          decision.consecutiveFailures);
    }
  }

  // Re-states the auto-mode decision to the filter. Called on enable and
  // whenever the config changes, so switching the mode off restores every hook
  // rather than leaving it wherever the policy last put it.
  void applyAutoMode() {
    mAutoMode.reset();
    if (mAutoModeEnabled.load(std::memory_order_relaxed)) {
      GlInterceptor::instance().setAllHooksEnabled(true);
      for (auto &state : mHookEnabledState) {
        state = true;
      }
    }
  }

  // Reads the GPU/CPU balance and the per-hook drop rates and sheds what is
  // not paying for itself. Runs on the render thread inside the swap call.
  void updateAutoMode(const FrameStats &stats, bool paced) {
    if (!mAutoModeEnabled.load(std::memory_order_relaxed) ||
        BootMarker::instance().safeMode()) {
      return;
    }

    // The accumulator is the only place that knows which resource won the
    // frame; the per-frame stats it hands over are the smoothed values.
    const auto &accumulator = EglSensor::instance().accumulator();
    AutoInput input;
    input.boundKnown = stats.frames > 240;
    input.gpuBound = accumulator.gpuBound();
    input.paced = paced;

    const AutoAction action = mAutoMode.update(input);
    if (action == AutoAction::ShedCpuFeatures) {
      // A GPU-bound frame cannot be helped by CPU-side work, and comparing
      // every state call against the shadow is exactly that. The scheduler
      // needs no handling here: its own governor already drops the boost to
      // zero on a GPU-bound frame.
      GlInterceptor::instance().setAllHooksEnabled(false);
      for (auto &state : mHookEnabledState) {
        state = false;
      }
      getSelf().getLogger().info(
          "GPU-bound: state-call filtering shed until the balance changes");
    } else if (action == AutoAction::RestoreCpuFeatures) {
      GlInterceptor::instance().setAllHooksEnabled(true);
      for (auto &state : mHookEnabledState) {
        state = true;
      }
      getSelf().getLogger().info("No longer GPU-bound: levers restored");
    }

    // The per-hook review only applies outside the GPU-bound state: while shed
    // there is nothing to review, because every hook is deliberately off.
    if (!mAutoMode.cpuFeaturesShed()) {
      reviewHooks();
    }
  }

  // Switches off any hook whose drop rate is too low to pay for its
  // indirection, and back on any that has since earned its place. Reads a
  // shared snapshot, so it runs on a slow cadence rather than per frame.
  void reviewHooks() {
    const std::uint64_t now = nowNs();
    if (now < mNextHookReviewNs) {
      return;
    }
    mNextHookReviewNs = now + kHookReviewIntervalNs;

    const auto traffic = GlCounters::instance().snapshot();
    for (std::size_t i = 0; i < kGlHookCount; ++i) {
      const auto hook = static_cast<GlHook>(i);
      const bool next = fpsopto::HookYieldJudge::evaluate(
          traffic.perHook[i].calls, traffic.perHook[i].suppressed,
          mHookEnabledState[i]);
      if (next != mHookEnabledState[i]) {
        GlInterceptor::instance().setHookEnabled(hook, next);
        getSelf().getLogger().info(
            "{} {} (drop rate {:.1f}% over {} calls)",
            fpsopto::glHookName(hook), next ? "re-enabled" : "disabled",
            traffic.perHook[i].dropRate() * 100.0, traffic.perHook[i].calls);
        mHookEnabledState[i] = next;
      }
    }
  }

  // Builds and submits the overlay. Cell values are re-read here and now, so
  // the readout never shows a number older than its own update interval.
  void updateOverlay(const FrameStats &stats, bool paced) {
    if (!mOverlayEnabled.load(std::memory_order_relaxed)) {
      return;
    }

    std::lock_guard lock(mOverlayMutex);
    if (++mOverlayFrameCounter < kOverlayFramesPerUpdate) {
      return;
    }
    mOverlayFrameCounter = 0;
    updateOverlayLocked(stats, paced);
  }

  // Fills the snapshot from live values. Caller holds `mOverlayMutex`.
  void updateOverlayLocked(const FrameStats &stats, bool paced) {
    const auto traffic = GlCounters::instance().snapshot();
    mDiagnostics.frameMs = stats.frameMs;
    mDiagnostics.fps = stats.fps;
    mDiagnostics.cpuMs = stats.cpuMs;
    mDiagnostics.gpuMs = stats.gpuMs;
    mDiagnostics.cpuLoad = stats.cpuLoad;
    mDiagnostics.gpuLoad = stats.gpuLoad;
    mDiagnostics.cadenceMs = stats.cadenceMs;
    mDiagnostics.gpuTiming = stats.gpuTiming;
    mDiagnostics.paced = paced;
    mDiagnostics.glDropRate = traffic.dropRate();
    mDiagnostics.glCalls = traffic.total.calls;
    mDiagnostics.glSuppressed = traffic.total.suppressed;
    mDiagnostics.hooksDisabled = traffic.hooksDisabled;
    mDiagnostics.gpuBound = mAutoMode.gpuBound();
    mDiagnostics.filterActive = GlInterceptor::instance().installed() &&
                                traffic.hooksDisabled < kGlHookCount;
    mDiagnostics.schedulerActive = ThreadScheduler::instance().running();
    mDiagnostics.safeMode = BootMarker::instance().safeMode();

    publishOverlay();
  }

  // Submits the current snapshot, or an empty command list when the overlay is
  // switched off, which is how the menu takes it down again.
  void publishOverlay() {
    std::lock_guard lock(mOverlayMutex);
    if (mOverlayEnabled.load(std::memory_order_relaxed)) {
      publishOverlayLocked();
      return;
    }
    pl::modmenu::submitDrawCommands(kModuleId,
                                    std::vector<pl::modmenu::DrawCommand>{});
  }

  void publishOverlayLocked() {
    char text[fpsopto::kDiagnosticsTextMax] = {};
    mDiagnostics.writeSummary(text);

    // One text command per line, stacked, so the HUD editor can place each row
    // independently and the two lines do not draw on top of each other.
    std::vector<pl::modmenu::DrawCommand> commands;
    std::string block(text);
    std::size_t start = 0;
    int row = 0;
    while (start <= block.size()) {
      const std::size_t end = block.find('\n', start);
      std::string line = block.substr(
          start, end == std::string::npos ? std::string::npos : end - start);
      if (!line.empty()) {
        pl::modmenu::DrawCommand command;
        command.type = pl::modmenu::DrawCommandType::Text;
        command.text = std::move(line);
        command.x = 0.0f;
        command.y = static_cast<float>(row) * 18.0f;
        command.size = 14.0f;
        commands.push_back(std::move(command));
        ++row;
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }

    pl::modmenu::submitDrawCommands(kModuleId, commands);
  }

  void maybeLog(const FrameStats &stats) {
    const std::uint64_t now = nowNs();
    if (now < mNextLogNs) {
      return;
    }

    const FpsConfig config = snapshot();
    mNextLogNs = now + static_cast<std::uint64_t>(config.logIntervalSeconds) *
                          1000000000ull;

    const auto glStats = GlCounters::instance().snapshot();
    const auto &decision = ThreadScheduler::instance().lastDecision();

    getSelf().getLogger().info(
        "frame {:.2f} ms ({:.1f} fps) cpu {:.2f} gpu {:.2f} | gl dropped "
        "{:.1f}% of {} | bound {} boost {}",
        stats.frameMs, stats.fps, stats.cpuMs, stats.gpuMs,
        glStats.dropRate() * 100.0, glStats.total.calls,
        static_cast<int>(decision.bound), decision.boost);
  }

  void applyLogging(const FpsConfig &config) {
    std::lock_guard lock(mConfigMutex);
    mLogging = config.logStats;
    if (config.logStats) {
      mNextLogNs = nowNs() +
                   static_cast<std::uint64_t>(config.logIntervalSeconds) *
                       1000000000ull;
    }
  }

  void registerMenu(const FpsConfig &config) {
    const bool registered =
        pl::modmenu::ModuleBuilder(kModuleId, "FPS Optimizer")
            .modId(getSelf().getId())
            .description(
                "Removes render-path overhead without changing a single pixel.")
            .defaultEnabled(true)
            .onConfigChanged(&FpsOptoPro::onConfigChanged)
            .config(kEnableGlFilterKey, "Skip redundant GL calls",
                    pl::modmenu::ConfigType::Toggle,
                    boolToMenuValue(config.enableGlFilter))
            .config(kEnableSchedulerKey, "Frame-aware scheduling",
                    pl::modmenu::ConfigType::Toggle,
                    boolToMenuValue(config.enableScheduler))
            .config(kWorkloadKey, "Workload shape",
                    pl::modmenu::ConfigType::Radio,
                    std::to_string(workloadToIndex(config.workload)),
                    std::string(fpsopto::kWorkloadMenuOptions))
            .config(kTargetFpsKey, "Target FPS (0 = auto)",
                    pl::modmenu::ConfigType::SliderInt,
                    std::to_string(config.targetFps), "0",
                    std::to_string(fpsopto::kMaxTargetFps))
            .config(kMaxBoostKey, "Max priority boost",
                    pl::modmenu::ConfigType::SliderInt,
                    std::to_string(config.maxWorkerBoost),
                    std::to_string(fpsopto::kMinWorkerBoost),
                    std::to_string(fpsopto::kMaxWorkerBoost))
            .config(kLogStatsKey, "Log frame stats",
                    pl::modmenu::ConfigType::Toggle,
                    boolToMenuValue(config.logStats))
            .config(kShowOverlayKey, "Show diagnostics overlay",
                    pl::modmenu::ConfigType::Toggle,
                    boolToMenuValue(config.showOverlay))
            .config(kAutoModeKey, "Automatic mode",
                    pl::modmenu::ConfigType::Toggle,
                    boolToMenuValue(config.autoMode))
            .registerModule();

    if (registered) {
      getSelf().getLogger().info("Mod Menu module {} registered", kModuleId);
    } else {
      getSelf().getLogger().warn("Mod Menu module {} could not be registered",
                                 kModuleId);
    }
  }

  static void onConfigChanged(std::string_view moduleId, std::string_view key,
                              std::string_view value) {
    instance().handleConfigChanged(moduleId, key, value);
  }

  void handleConfigChanged(std::string_view moduleId, std::string_view key,
                           std::string_view value) {
    if (moduleId != kModuleId) {
      return;
    }

    FpsConfig updated;
    {
      std::lock_guard lock(mConfigMutex);
      if (!mConfigFile) {
        return;
      }
      auto &config = mConfigFile->value();
      if (key == kEnableGlFilterKey) {
        config.enableGlFilter = parseBool(value, config.enableGlFilter);
      } else if (key == kEnableSchedulerKey) {
        config.enableScheduler = parseBool(value, config.enableScheduler);
      } else if (key == kWorkloadKey) {
        config.workload = parseWorkload(value, config.workload);
      } else if (key == kTargetFpsKey) {
        config.targetFps = parseInt(value, config.targetFps);
      } else if (key == kMaxBoostKey) {
        config.maxWorkerBoost = parseInt(value, config.maxWorkerBoost);
      } else if (key == kLogStatsKey) {
        config.logStats = parseBool(value, config.logStats);
        mLogging = config.logStats;
      } else if (key == kShowOverlayKey) {
        config.showOverlay = parseBool(value, config.showOverlay);
        mOverlayEnabled = config.showOverlay;
      } else if (key == kAutoModeKey) {
        config.autoMode = parseBool(value, config.autoMode);
        mAutoModeEnabled = config.autoMode;
      } else {
        return;
      }
      normalize(config);
      mConfigFile->save();
      updated = config;
    }

    if (key == kMaxBoostKey) {
      ThreadScheduler::instance().setMaxBoost(updated.maxWorkerBoost);
    }
    if (key == kAutoModeKey) {
      // Re-state the hooks from the new mode, and take the overlay down if it
      // was switched off so a stale readout does not stay on screen.
      applyAutoMode();
    }
    if (key == kShowOverlayKey) {
      publishOverlay();
    }
    if (key == kLogStatsKey && updated.logStats) {
      mNextLogNs = nowNs() +
                   static_cast<std::uint64_t>(updated.logIntervalSeconds) *
                       1000000000ull;
    }

    getSelf().getLogger().info("Config {} updated", key);
  }
};

} // namespace

PL_REGISTER_MOD(FpsOptoPro, FpsOptoPro::instance())