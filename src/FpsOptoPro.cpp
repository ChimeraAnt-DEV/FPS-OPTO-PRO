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
#include "EglSensor.h"
#include "GlInterceptor.h"
#include "ThreadScheduler.h"

namespace {

using fpsopto::AdpfSession;
using fpsopto::EglSensor;
using fpsopto::FrameStats;
using fpsopto::FpsConfig;
using fpsopto::GlInterceptor;
using fpsopto::ThreadScheduler;
using fpsopto::Workload;

constexpr const char *kModuleId = "fps_opto_pro.core";
constexpr const char *kEnableGlFilterKey = "enableGlFilter";
constexpr const char *kEnableSchedulerKey = "enableScheduler";
constexpr const char *kWorkloadKey = "workload";
constexpr const char *kTargetFpsKey = "targetFps";
constexpr const char *kMaxBoostKey = "maxWorkerBoost";
constexpr const char *kLogStatsKey = "logStats";

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

    applyLogging(config);
    registerMenu(config);
    return true;
  }

  bool disable() {
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

    if (self.mLogging.load(std::memory_order_relaxed)) {
      self.maybeLog(stats);
    }
  }

  void maybeLog(const FrameStats &stats) {
    const std::uint64_t now = nowNs();
    if (now < mNextLogNs) {
      return;
    }

    const FpsConfig config = snapshot();
    mNextLogNs = now + static_cast<std::uint64_t>(config.logIntervalSeconds) *
                          1000000000ull;

    const auto &glStats = GlInterceptor::instance().stats();
    const auto &decision = ThreadScheduler::instance().lastDecision();

    getSelf().getLogger().info(
        "frame {:.2f} ms ({:.1f} fps) cpu {:.2f} gpu {:.2f} | gl suppressed "
        "{:.1f}% | bound {} boost {}",
        stats.frameMs, stats.fps, stats.cpuMs, stats.gpuMs,
        glStats.suppressionRatio() * 100.0, static_cast<int>(decision.bound),
        decision.boost);
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