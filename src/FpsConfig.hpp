#pragma once

#include <optional>
#include <string_view>

#include <pl/Config.hpp>

namespace fpsopto {

// Declared workload shape. `Auto` reads the GPU/CPU balance out of the live
// frame telemetry; the explicit values exist so a device with a known-bad
// governor profile can be pinned without changing anything that is rendered.
enum class Workload {
  Auto,
  GpuBound,
  CpuBound,
};

struct FpsConfig {
  int version = 1;

  bool enableGlFilter = true;
  bool enableEglSensor = true;
  bool enableScheduler = true;

  bool logStats = false;
  int logIntervalSeconds = 30;

  Workload workload = Workload::Auto;

  // 0 means "adopt whatever the display prefers".
  int targetFps = 0;
  int minTargetFps = 30;
  int maxTargetFps = 60;

  // Upper bound on how far a render thread's nice value may be raised. Boost is
  // advisory: the scheduler may ignore it entirely.
  int maxWorkerBoost = 2;

  // Stronger, more invasive levers. Off by default so the honest baseline is
  // "the mod does nothing to your scheduler unless you ask".
  bool allowThreadAffinity = false;
  bool allowAdpfSession = false;
};

inline constexpr int kMinTargetFps = 0;
inline constexpr int kMaxTargetFps = 240;
inline constexpr int kMinLogInterval = 5;
inline constexpr int kMaxLogInterval = 600;
inline constexpr int kMinWorkerBoost = 0;
inline constexpr int kMaxWorkerBoost = 10;
inline constexpr std::string_view kWorkloadMenuOptions = "Auto,GpuBound,CpuBound";

} // namespace fpsopto

namespace pl::config {

template <> struct Schema<fpsopto::FpsConfig> {
  static constexpr std::string_view title = "FPS-OPTO-PRO";
  static constexpr std::string_view description =
      "Render-path optimisation for Minecraft Bedrock. Every option changes "
      "timing, never pixels.";

  static constexpr FieldSchema field(std::string_view name) {
    if (name == "version") {
      return {"Version", "Config schema version managed by the mod.",
              std::nullopt, std::nullopt, true};
    }
    if (name == "enableGlFilter") {
      return {"Skip redundant GL calls",
              "Drop GL ES state calls the driver would ignore anyway. Draw "
              "output is unchanged; state traffic per frame drops.",
              std::nullopt, std::nullopt, false};
    }
    if (name == "enableEglSensor") {
      return {"Live frame sensor",
              "Measure frame interval and GPU workloads at the swap boundary "
              "to drive the scheduler. Read-only.",
              std::nullopt, std::nullopt, false};
    }
    if (name == "enableScheduler") {
      return {"Frame-aware scheduling",
              "Raise render-thread priority and ask the kernel's scheduler "
              "for a frame-paced CPU/GPU allocation.",
              std::nullopt, std::nullopt, false};
    }
    if (name == "logStats") {
      return {"Log frame stats",
              "Write measured frame timings to logcat under the mod tag.",
              std::nullopt, std::nullopt, false};
    }
    if (name == "logIntervalSeconds") {
      return {"Log interval (s)", "How often stats are written.",
              fpsopto::kMinLogInterval, fpsopto::kMaxLogInterval, false};
    }
    if (name == "workload") {
      return {"Workload shape",
              "Which resource the frame time is dominated by. Auto measures "
              "it from GPU completion timing.",
              std::nullopt, std::nullopt, false};
    }
    if (name == "targetFps") {
      return {"Target FPS (0 = auto)",
              "Frame rate the scheduler aims to sustain. Zero adopts the "
              "display's preferred rate.",
              fpsopto::kMinTargetFps, fpsopto::kMaxTargetFps, false};
    }
    if (name == "minTargetFps") {
      return {"Minimum target FPS", "Lower clamp used when auto-detecting.",
              fpsopto::kMinTargetFps, fpsopto::kMaxTargetFps, false};
    }
    if (name == "maxTargetFps") {
      return {"Maximum target FPS", "Upper clamp used when auto-detecting.",
              fpsopto::kMinTargetFps, fpsopto::kMaxTargetFps, false};
    }
    if (name == "maxWorkerBoost") {
      return {"Max priority boost",
              "Ceiling on render-thread nice reduction. Advisory.",
              fpsopto::kMinWorkerBoost, fpsopto::kMaxWorkerBoost, false};
    }
    if (name == "allowThreadAffinity") {
      return {"Pin render threads (advanced)",
              "Restrict the render thread group to the fast cores. Helps on "
              "big.LITTLE parts, hurts when thermal throttling moves the "
              "bottleneck.",
              std::nullopt, std::nullopt, false};
    }
    if (name == "allowAdpfSession") {
      return {"ADPF hint session (advanced)",
              "Cooperative CPU/GPU deadline hints on Android 12+. Can hurt on "
              "drivers whose GPU timing is unreliable, which is why it is off "
              "by default.",
              std::nullopt, std::nullopt, false};
    }
    return {};
  }
};

} // namespace pl::config