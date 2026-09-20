#pragma once

#include "FrameGovernor.h"

namespace fpsopto {

// Maps frame statistics onto real render threads.
//
// Every action here is advisory. The scheduler is free to ignore any of it,
// and none of it changes what the game draws.
class ThreadScheduler {
public:
  static ThreadScheduler &instance();

  void start();
  void stop();

  [[nodiscard]] bool running() const { return mRunning; }
  [[nodiscard]] const ScheduleDecision &lastDecision() const {
    return mLast;
  }
  [[nodiscard]] int currentBoost() const { return mAppliedBoost; }

  void setMaxBoost(int maxBoost);

  // Called once per frame from the swap detour. Sampling into the render
  // threads is throttled internally: enumerating a process's threads is far
  // too expensive to do per frame.
  void onFrame(double frameMs, double cadenceMs, double cpuLoad, double gpuLoad,
               bool gpuTimingAvailable, bool gpuOverran, bool paced,
               bool workloadKnown);

  // Applies a decision to the threads of the calling process. Exposed for
  // tests, which drive it directly rather than through a frame.
  void apply(const ScheduleDecision &decision);

private:
  ThreadScheduler() = default;

  static constexpr int kSamplePeriodFrames = 90;

  bool mRunning = false;
  int mFrameCounter = 0;
  int mAppliedBoost = -1;
  int mMaxBoost = 2;
  ScheduleDecision mLast{};
};

} // namespace fpsopto