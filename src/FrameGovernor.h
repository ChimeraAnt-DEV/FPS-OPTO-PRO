#pragma once

#include <cstdint>

namespace fpsopto {

// Which resource the frame time is dominated by.
enum class Bound : std::uint8_t { Unknown, Cpu, Gpu };

// What the scheduler should do with the render threads this interval.
struct ScheduleDecision {
  Bound bound = Bound::Unknown;
  // Additional nice-value reduction granted to render threads, 0..maxBoost.
  int boost = 0;
  // Target frame rate the deadline hint should use; 0 means "leave it alone".
  int targetFps = 0;
  // True when the frame time is being paced by the swap chain, so nothing
  // should be changed at all.
  bool paced = false;
};

// Pure policy: frame statistics in, scheduling intent out.
//
// The rules are deliberately asymmetric.
//
//   * Under a CPU bound, raising priority genuinely helps: the render thread
//     competes for the same cores as the audio mixer and the UI thread, and
//     the frame is late the moment it loses a scheduling race.
//
//   * Under a GPU bound, priority cannot make the GPU finish sooner. Raising
//     it anyway steers the render thread onto the same cores the driver's
//     submission thread needs, which can lengthen the frame. So the boost is
//     pulled back toward zero, not pushed up.
//
//   * When the swap chain is pacing the game, the frame rate is already the
//     display's, and any intervention can only add jitter.
struct GovernorInput {
  double frameMs = 0.0;
  double cadenceMs = 0.0;
  double cpuLoad = 0.0;
  double gpuLoad = 0.0;
  bool gpuTimingAvailable = false;
  bool gpuOverran = false;
  bool paced = false;
  bool workloadKnown = false; // CPU/GPU balance measured well enough to trust
  int maxBoost = 2;
  int targetFps = 0;
  int minTargetFps = 30;
  int maxTargetFps = 60;
};

constexpr double kCpuHeadroomLoad = 0.55;
constexpr double kCpuSaturatedLoad = 0.80;
constexpr double kGpuSaturatedLoad = 0.90;

inline int decideTargetFps(const GovernorInput &in) {
  if (in.targetFps > 0) {
    return in.targetFps;
  }
  if (in.cadenceMs > 0.0) {
    const int detected =
        static_cast<int>(1000.0 / in.cadenceMs + 0.5);
    if (detected >= in.minTargetFps && detected <= in.maxTargetFps) {
      return detected;
    }
  }
  return in.maxTargetFps;
}

inline ScheduleDecision decide(const GovernorInput &in) {
  ScheduleDecision out;
  out.targetFps = decideTargetFps(in);

  if (in.paced) {
    out.paced = true;
    out.bound = Bound::Unknown;
    out.boost = 0;
    // A paced frame is already at the display's rate. Keeping the deadline
    // hint at that rate is safe; changing it is not.
    return out;
  }

  // A frame the governor has not measured well enough to trust either side is
  // still worth a small, safe lift: unlike the CPU and GPU rules below it does
  // not try to pick a winner, so it cannot be wrong about which resource is
  // saturated.
  if (!in.workloadKnown && !in.gpuTimingAvailable && in.gpuLoad <= 0.0 &&
      in.cpuLoad < kCpuHeadroomLoad) {
    out.bound = Bound::Unknown;
    out.boost = in.maxBoost > 0 ? 1 : 0;
    return out;
  }

  const bool gpuLimited =
      in.gpuOverran || (in.gpuTimingAvailable && in.gpuLoad >= kGpuSaturatedLoad);

  if (gpuLimited) {
    out.bound = Bound::Gpu;
    // Back off. A render thread parked on the GPU is holding a core it is not
    // using productively, and on a big.LITTLE part that core is often a fast
    // one the driver's own submission thread wants.
    out.boost = 0;
    return out;
  }

  const bool cpuLimited =
      in.cpuLoad >= kCpuSaturatedLoad ||
      (in.cpuLoad >= kCpuHeadroomLoad && !in.gpuTimingAvailable) ||
      (in.cpuLoad >= kCpuHeadroomLoad && in.gpuLoad < kGpuSaturatedLoad);

  if (cpuLimited) {
    out.bound = Bound::Cpu;
    out.boost = in.maxBoost;
    return out;
  }

  out.bound = Bound::Unknown;
  out.boost = in.maxBoost > 0 ? 1 : 0;
  return out;
}

} // namespace fpsopto