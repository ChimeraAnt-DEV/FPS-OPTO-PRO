// Behaviour of the frame accumulator: what the governor is told about a run
// of frames.

#include "FrameTelemetry.h"
#include "test_support.h"

using namespace fpsopto;

namespace {

constexpr double kMs = 1.0e6;

FrameSample frame(double frameMs, double cpuMs = 0.0, double gpuMs = 0.0,
                  bool gpuMeasured = false, bool overran = false) {
  FrameSample sample;
  sample.frameNs = static_cast<std::uint64_t>(frameMs * kMs);
  sample.cpuNs = static_cast<std::uint64_t>(cpuMs * kMs);
  sample.gpuNs = static_cast<std::uint64_t>(gpuMs * kMs);
  sample.gpuMeasured = gpuMeasured;
  sample.gpuOverran = overran;
  return sample;
}

void zeroLengthFrameIsIgnored() {
  FrameAccumulator<> acc;
  FrameSample empty;
  acc.push(empty);
  EXPECT_EQ(acc.stats().frames, std::uint64_t{0});
}

void frameRateMatchesTheFrameTime() {
  FrameAccumulator<> acc;
  for (int i = 0; i < 40; ++i) {
    acc.push(frame(16.0));
  }
  // The exponential average has converged from the first sample, so a steady
  // input must land on the input value exactly.
  EXPECT_NEAR(acc.stats().frameMs, 16.0, 0.01);
  EXPECT_NEAR(acc.stats().fps, 62.5, 0.5);
}

void cadenceIsTheFastestObservedFrame() {
  FrameAccumulator<> acc;
  for (int i = 0; i < 30; ++i) {
    acc.push(frame(33.0));
  }
  acc.push(frame(16.6));
  for (int i = 0; i < 30; ++i) {
    acc.push(frame(33.0));
  }
  EXPECT_NEAR(acc.cadenceMs(), 16.6, 0.01);
}

void steadyCadenceIsReportedAsPaced() {
  FrameAccumulator<> acc;
  for (int i = 0; i < 60; ++i) {
    acc.push(frame(16.6));
  }
  EXPECT_TRUE(acc.vsyncLocked());
}

// A game that is comfortably faster than the display still gets its frames
// released at the display's rate, so the swap interval is the cadence and the
// frame time sits on it. That must read as paced, not as headroom.
void pacedEvenWhenCpuIsIdle() {
  FrameAccumulator<> acc;
  for (int i = 0; i < 60; ++i) {
    acc.push(frame(16.6, 2.0));
  }
  EXPECT_TRUE(acc.vsyncLocked());
  EXPECT_FALSE(acc.cpuBound());
}

void irregularFramesAreNotPaced() {
  FrameAccumulator<> acc;
  for (int i = 0; i < 60; ++i) {
    acc.push(frame(i % 2 == 0 ? 16.6 : 40.0));
  }
  EXPECT_FALSE(acc.vsyncLocked());
}

void gpuOverrunVotesMakeItGpuBound() {
  FrameAccumulator<> acc;
  for (int i = 0; i < 20; ++i) {
    acc.push(frame(40.0, 10.0, 0.0, false, true));
  }
  EXPECT_TRUE(acc.gpuBound());
  EXPECT_FALSE(acc.cpuBound());
}

void idleGpuWithBusyCpuIsCpuBound() {
  FrameAccumulator<> acc;
  for (int i = 0; i < 40; ++i) {
    acc.push(frame(40.0, 36.0, 8.0, true));
  }
  EXPECT_FALSE(acc.gpuBound());
  EXPECT_TRUE(acc.cpuBound());
}

void gpuTimingFlagReflectsMeasurement() {
  FrameAccumulator<> acc;
  for (int i = 0; i < 10; ++i) {
    acc.push(frame(16.6));
  }
  EXPECT_FALSE(acc.stats().gpuTiming);

  for (int i = 0; i < 10; ++i) {
    acc.push(frame(16.6, 1.0, 14.0, true));
  }
  EXPECT_TRUE(acc.stats().gpuTiming);
}

void resetClearsEverything() {
  FrameAccumulator<> acc;
  for (int i = 0; i < 40; ++i) {
    acc.push(frame(16.0));
  }
  acc.reset();
  EXPECT_EQ(acc.stats().frames, std::uint64_t{0});
  EXPECT_EQ(acc.sampleCount(), std::size_t{0});
  EXPECT_NEAR(acc.stats().frameMs, 0.0, 0.001);
}

void medianIgnoresASingleHitch() {
  FrameAccumulator<> acc;
  for (int i = 0; i < 99; ++i) {
    acc.push(frame(16.0));
  }
  acc.push(frame(200.0));
  // The exponential average is dragged up by the hitch, the median is not.
  EXPECT_NEAR(acc.stats().medianFrameMs, 16.0, 0.01);
  EXPECT_TRUE(acc.stats().frameMs > 16.0);
}

} // namespace

void runTelemetryTests() {
  zeroLengthFrameIsIgnored();
  frameRateMatchesTheFrameTime();
  cadenceIsTheFastestObservedFrame();
  steadyCadenceIsReportedAsPaced();
  pacedEvenWhenCpuIsIdle();
  irregularFramesAreNotPaced();
  gpuOverrunVotesMakeItGpuBound();
  idleGpuWithBusyCpuIsCpuBound();
  gpuTimingFlagReflectsMeasurement();
  resetClearsEverything();
  medianIgnoresASingleHitch();
}