#pragma once

// The auto mode: which levers to keep paying for, decided from live evidence.
//
// Two rules, both answering "is this lever earning its keep?":
//
//   1. If the frame is GPU-bound, the CPU-side levers cannot move the frame
//      time. Worse, they cost something: the filter still inspects every state
//      call, and a raised priority on a GPU-bound frame parks the render
//      thread on a core the driver's submission thread wants. So they are
//      switched off until the balance changes back.
//
//   2. If a hook's drop rate is only a few percent, it is not paying for its
//      own indirection. A detour is one extra call plus the shadow comparison
//      for every state call; at a couple of percent that costs more than the
//      calls it removes.
//
// Both rules are hysteresis-driven. Flipping a lever on and off around a
// threshold would make the frame time itself the thing that oscillates, which
// is the failure mode this is supposed to remove.

#include <cstdint>

namespace fpsopto {

// A hook whose drop rate falls below this is not worth its indirection.
inline constexpr double kLowYieldDropRate = 0.05;

// A hook must reach this drop rate to be switched back on. The gap between the
// two is the hysteresis band: a hook sitting near the threshold stays wherever
// it is rather than chattering.
inline constexpr double kReviveDropRate = 0.15;

// A hook must have this many calls before its drop rate is trusted at all. A
// hook called twice with one drop is not "50% effective", it is unmeasured.
inline constexpr std::uint64_t kMinCallsToJudge = 400;

// Frames a state must hold before the auto mode acts on it, so a single
// unrepresentative frame cannot flip anything.
inline constexpr int kBoundHoldFrames = 45;

enum class AutoAction : unsigned char {
  None,
  // The frame is GPU-bound: stop paying for the CPU-side levers.
  ShedCpuFeatures,
  // The frame is no longer GPU-bound: the levers can earn their keep again.
  RestoreCpuFeatures,
};

struct AutoInput {
  // From the frame accumulator. `boundKnown` is false until there is enough
  // telemetry to pick a winner, in which case nothing is changed.
  bool gpuBound = false;
  bool boundKnown = false;
  bool paced = false;
};

// Decides whether to drop or restore the CPU-side levers.
class AutoMode {
public:
  // Returns an action only when the new state has been the same for
  // kBoundHoldFrames frames.
  AutoAction update(const AutoInput &input) {
    if (!input.boundKnown) {
      return AutoAction::None;
    }

    // A paced frame is the display limiting the frame rate, not either
    // resource. It says nothing about the balance, so the hold is not advanced
    // and the current state is kept.
    if (input.paced) {
      return AutoAction::None;
    }

    if (input.gpuBound == mGpuBound) {
      mHoldFrames = 0;
      return AutoAction::None;
    }

    if (++mHoldFrames < kBoundHoldFrames) {
      return AutoAction::None;
    }

    mHoldFrames = 0;
    mGpuBound = input.gpuBound;
    return mGpuBound ? AutoAction::ShedCpuFeatures
                     : AutoAction::RestoreCpuFeatures;
  }

  [[nodiscard]] bool cpuFeaturesShed() const { return mGpuBound; }
  [[nodiscard]] bool gpuBound() const { return mGpuBound; }

  void reset() {
    mGpuBound = false;
    mHoldFrames = 0;
  }

private:
  bool mGpuBound = false;
  int mHoldFrames = 0;
};

// Decides whether a single hook should stay installed, using the hysteresis
// band above. Kept separate from AutoMode because a hook's usefulness and the
// frame's balance are independent questions.
class HookYieldJudge {
public:
  // `enabled` is the current state, which the caller owns. Returns the state
  // to use from now on.
  [[nodiscard]] static bool evaluate(std::uint64_t calls,
                                     std::uint64_t suppressed, bool enabled) {
    if (calls < kMinCallsToJudge) {
      return enabled;
    }
    const double rate =
        static_cast<double>(suppressed) / static_cast<double>(calls);
    if (enabled) {
      return rate >= kLowYieldDropRate;
    }
    return rate >= kReviveDropRate;
  }
};

} // namespace fpsopto