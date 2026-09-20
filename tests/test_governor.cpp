// Policy tests for the scheduling governor. No mocks: this drives the same
// `decide()` the Android library calls on every frame.

#include "FrameGovernor.h"
#include "test_support.h"

using namespace fpsopto;

namespace {

GovernorInput base() {
  GovernorInput in;
  in.maxBoost = 2;
  in.minTargetFps = 30;
  in.maxTargetFps = 60;
  return in;
}

// A frame that the swap chain is pacing must never be treated as headroom.
void pacedFrameAsksForNothing() {
  GovernorInput in = base();
  in.paced = true;
  in.frameMs = 16.6;
  in.cadenceMs = 16.6;
  in.cpuLoad = 0.2;
  in.gpuLoad = 0.2;
  const auto decision = decide(in);
  EXPECT_TRUE(decision.paced);
  EXPECT_EQ(decision.boost, 0);
}

// A GPU-overrun frame is the clearest evidence the GPU is the limit, and the
// response is to *reduce* pressure rather than raise priority.
void gpuOverrunPullsTheBoostBack() {
  GovernorInput in = base();
  in.frameMs = 33.0;
  in.cadenceMs = 16.6;
  in.gpuOverran = true;
  in.gpuTimingAvailable = true;
  in.gpuLoad = 1.4;
  const auto decision = decide(in);
  EXPECT_EQ(static_cast<int>(decision.bound), static_cast<int>(Bound::Gpu));
  EXPECT_EQ(decision.boost, 0);
}

void heavyGpuLoadIsTreatedAsGpuBound() {
  GovernorInput in = base();
  in.frameMs = 33.0;
  in.cadenceMs = 16.6;
  in.gpuTimingAvailable = true;
  in.gpuLoad = 0.95;
  in.cpuLoad = 0.9;
  const auto decision = decide(in);
  EXPECT_EQ(static_cast<int>(decision.bound), static_cast<int>(Bound::Gpu));
  EXPECT_EQ(decision.boost, 0);
}

void saturatedCpuRaisesTheBoost() {
  GovernorInput in = base();
  in.frameMs = 33.0;
  in.cadenceMs = 16.6;
  in.cpuLoad = 0.95;
  in.gpuLoad = 0.4;
  in.gpuTimingAvailable = true;
  const auto decision = decide(in);
  EXPECT_EQ(static_cast<int>(decision.bound), static_cast<int>(Bound::Cpu));
  EXPECT_EQ(decision.boost, 2);
}

// On a device whose driver reports no GPU timing the CPU number is the only
// signal available, so a mid-range load there still counts as CPU bound.
void cpuLoadWithoutGpuTimingIsCpuBound() {
  GovernorInput in = base();
  in.frameMs = 33.0;
  in.cadenceMs = 16.6;
  in.cpuLoad = 0.6;
  in.gpuTimingAvailable = false;
  const auto decision = decide(in);
  EXPECT_EQ(static_cast<int>(decision.bound), static_cast<int>(Bound::Cpu));
  EXPECT_EQ(decision.boost, 2);
}

// With GPU timing available and the GPU idle, the same CPU load is not by
// itself proof of a CPU bottleneck, but it is still worth a small lift.
void moderateCpuLoadWithIdleGpuGetsASmallBoost() {
  GovernorInput in = base();
  in.frameMs = 33.0;
  in.cadenceMs = 16.6;
  in.cpuLoad = 0.6;
  in.gpuLoad = 0.3;
  in.gpuTimingAvailable = true;
  in.workloadKnown = true;
  const auto decision = decide(in);
  EXPECT_EQ(decision.boost, 2);
}

void noMeasurementYetStillGetsAOneStepLift() {
  GovernorInput in = base();
  in.workloadKnown = false;
  const auto decision = decide(in);
  EXPECT_EQ(decision.boost, 1);
}

void zeroMaxBoostDisablesAllLifts() {
  GovernorInput in = base();
  in.maxBoost = 0;
  in.cpuLoad = 0.99;
  in.gpuTimingAvailable = true;
  in.gpuLoad = 0.2;
  const auto decision = decide(in);
  EXPECT_EQ(decision.boost, 0);
}

// The target frame rate follows the measured cadence rather than a hardcoded
// refresh rate, so a 120 Hz panel and a 60 Hz panel get different answers.
void targetFpsFollowsTheMeasuredCadence() {
  GovernorInput in = base();
  in.maxTargetFps = 120;
  in.cadenceMs = 1000.0 / 90.0;
  EXPECT_EQ(decideTargetFps(in), 90);
}

void explicitTargetFpsWinsOverTheCadence() {
  GovernorInput in = base();
  in.targetFps = 45;
  in.cadenceMs = 16.6;
  EXPECT_EQ(decideTargetFps(in), 45);
}

// A cadence outside the permitted window falls back to the configured ceiling
// rather than being adopted blindly.
void implausibleCadenceClampsToTheCeiling() {
  GovernorInput in = base();
  in.cadenceMs = 1000.0 / 240.0; // faster than maxTargetFps allows
  EXPECT_EQ(decideTargetFps(in), 60);
}

void unknownCadenceFallsBackToTheCeiling() {
  GovernorInput in = base();
  in.cadenceMs = 0.0;
  EXPECT_EQ(decideTargetFps(in), 60);
}

} // namespace

void runGovernorTests() {
  pacedFrameAsksForNothing();
  gpuOverrunPullsTheBoostBack();
  heavyGpuLoadIsTreatedAsGpuBound();
  saturatedCpuRaisesTheBoost();
  cpuLoadWithoutGpuTimingIsCpuBound();
  moderateCpuLoadWithIdleGpuGetsASmallBoost();
  noMeasurementYetStillGetsAOneStepLift();
  zeroMaxBoostDisablesAllLifts();
  targetFpsFollowsTheMeasuredCadence();
  explicitTargetFpsWinsOverTheCadence();
  implausibleCadenceClampsToTheCeiling();
  unknownCadenceFallsBackToTheCeiling();
}