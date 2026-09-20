// Host tests for the decision code behind the upgrades: the diagnostic
// snapshot, the auto mode, and the crash-loop guard. Each is a pure function
// or a small state machine, so they can be exercised exhaustively here and
// trusted on device.

#include "test_support.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "AutoPolicy.h"
#include "BootGuard.h"
#include "BootMarker.h"
#include "Diagnostics.h"

namespace {

// ---------------------------------------------------------------------------
// Auto mode
// ---------------------------------------------------------------------------

void gpuBoundShedsCpuFeaturesAfterAConfirmationPeriod() {
  fpsopto::AutoMode mode;
  fpsopto::AutoInput input;
  input.boundKnown = true;
  input.gpuBound = true;

  // The first frames only start the hold; nothing flips yet.
  EXPECT_EQ(static_cast<int>(mode.update(input)),
            static_cast<int>(fpsopto::AutoAction::None));
  for (int i = 0; i < fpsopto::kBoundHoldFrames - 2; ++i) {
    EXPECT_EQ(static_cast<int>(mode.update(input)),
              static_cast<int>(fpsopto::AutoAction::None));
  }
  EXPECT_EQ(static_cast<int>(mode.update(input)),
            static_cast<int>(fpsopto::AutoAction::ShedCpuFeatures));
  EXPECT_TRUE(mode.cpuFeaturesShed());
  // It does not fire again once the state is settled.
  EXPECT_EQ(static_cast<int>(mode.update(input)),
            static_cast<int>(fpsopto::AutoAction::None));
}

void losingGpuBoundRestoresCpuFeatures() {
  fpsopto::AutoMode mode;
  fpsopto::AutoInput input;
  input.boundKnown = true;
  input.gpuBound = true;
  for (int i = 0; i < fpsopto::kBoundHoldFrames; ++i) {
    mode.update(input);
  }
  EXPECT_TRUE(mode.cpuFeaturesShed());

  input.gpuBound = false;
  fpsopto::AutoAction last = fpsopto::AutoAction::None;
  for (int i = 0; i < fpsopto::kBoundHoldFrames; ++i) {
    last = mode.update(input);
  }
  EXPECT_EQ(static_cast<int>(last),
            static_cast<int>(fpsopto::AutoAction::RestoreCpuFeatures));
  EXPECT_FALSE(mode.cpuFeaturesShed());
}

// A paced frame says nothing about which resource is the limit, so the mode
// must not act on it.
void pacedFramesDoNotFlipTheMode() {
  fpsopto::AutoMode mode;
  fpsopto::AutoInput input;
  input.boundKnown = true;
  input.gpuBound = true;
  input.paced = true;
  for (int i = 0; i < fpsopto::kBoundHoldFrames * 2; ++i) {
    EXPECT_EQ(static_cast<int>(mode.update(input)),
              static_cast<int>(fpsopto::AutoAction::None));
  }
}

void unknownBalanceChangesNothing() {
  fpsopto::AutoMode mode;
  fpsopto::AutoInput input;
  input.boundKnown = false;
  for (int i = 0; i < fpsopto::kBoundHoldFrames * 2; ++i) {
    EXPECT_EQ(static_cast<int>(mode.update(input)),
              static_cast<int>(fpsopto::AutoAction::None));
  }
}

void lowYieldHooksAreDisabledAndOnlyRevivedOnRealEvidence() {
  using fpsopto::HookYieldJudge;
  // Too few calls to judge: the state is kept whatever the ratio.
  EXPECT_TRUE(HookYieldJudge::evaluate(10, 0, true));
  EXPECT_FALSE(HookYieldJudge::evaluate(10, 10, false));

  // Measured and low-yield: disabled.
  EXPECT_FALSE(HookYieldJudge::evaluate(1000, 10, true));
  // Measured and useful: kept.
  EXPECT_TRUE(HookYieldJudge::evaluate(1000, 300, true));

  // A disabled hook needs more evidence to come back, which is the hysteresis.
  EXPECT_FALSE(HookYieldJudge::evaluate(1000, 80, false)); // 8% < 15%
  EXPECT_TRUE(HookYieldJudge::evaluate(1000, 200, false)); // 20% >= 15%
}

// ---------------------------------------------------------------------------
// Crash-loop guard
// ---------------------------------------------------------------------------

void aFirstLaunchIsNormal() {
  const auto decision = fpsopto::decideAtLoad({});
  EXPECT_EQ(static_cast<int>(decision.outcome),
            static_cast<int>(fpsopto::BootOutcome::Normal));
  EXPECT_EQ(decision.consecutiveFailures, 0);
}

void anOpenMarkerMeansSafeMode() {
  fpsopto::BootState state;
  state.markerOpen = true;
  state.consecutiveFailures = 0;
  const auto decision = fpsopto::decideAtLoad(state);
  EXPECT_EQ(static_cast<int>(decision.outcome),
            static_cast<int>(fpsopto::BootOutcome::SafeMode));
  EXPECT_EQ(decision.consecutiveFailures, 1);
}

void consecutiveFailuresAccumulate() {
  fpsopto::BootState state;
  state.markerOpen = true;
  state.consecutiveFailures = 2;
  const auto decision = fpsopto::decideAtLoad(state);
  EXPECT_EQ(decision.consecutiveFailures, 3);
}

// Time alone is not proof the loop ran, and frames alone could be a burst
// during a stall, so both must be satisfied.
void stabilityNeedsBothTimeAndFrames() {
  EXPECT_FALSE(fpsopto::isProvenStable(fpsopto::kStableUptimeNs, 1));
  EXPECT_FALSE(fpsopto::isProvenStable(0, fpsopto::kStableFrames));
  EXPECT_TRUE(fpsopto::isProvenStable(fpsopto::kStableUptimeNs,
                                      fpsopto::kStableFrames));
}

void safeModeKeepsItsOwnMarkerOpen() {
  fpsopto::BootDecision decision;
  decision.outcome = fpsopto::BootOutcome::SafeMode;
  decision.consecutiveFailures = 1;
  const auto opened = fpsopto::stateAfterLoad(decision);
  EXPECT_TRUE(opened.markerOpen);
  EXPECT_EQ(opened.consecutiveFailures, 1);

  const auto clean = fpsopto::stateAfterStable();
  EXPECT_FALSE(clean.markerOpen);
  EXPECT_EQ(clean.consecutiveFailures, 0);
}

// The file handshake, against a scratch directory.
void markerRoundTripsThroughDisk() {
  const auto root = std::filesystem::temp_directory_path() / "fpsopto-boot-test";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);

  auto &marker = fpsopto::BootMarker::instance();
  marker.resetForTest();

  // First launch: no marker, so normal, and a marker is written.
  auto decision = marker.begin(root);
  EXPECT_EQ(static_cast<int>(decision.outcome),
            static_cast<int>(fpsopto::BootOutcome::Normal));
  EXPECT_TRUE(std::filesystem::exists(root / "boot.marker"));

  // A launch that crashes leaves the marker open, so the next one is safe mode.
  marker.resetForTest();
  decision = marker.begin(root);
  EXPECT_EQ(static_cast<int>(decision.outcome),
            static_cast<int>(fpsopto::BootOutcome::SafeMode));
  EXPECT_TRUE(marker.safeMode());
  EXPECT_EQ(marker.consecutiveFailures(), 1);

  // Proving stability retires the marker, and then the launch after that is
  // normal again.
  marker.noteFrame(fpsopto::kStableUptimeNs, fpsopto::kStableFrames);
  marker.resetForTest();
  decision = marker.begin(root);
  EXPECT_EQ(static_cast<int>(decision.outcome),
            static_cast<int>(fpsopto::BootOutcome::Normal));
  EXPECT_FALSE(marker.safeMode());

  std::filesystem::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// Diagnostics and drift
// ---------------------------------------------------------------------------

void theDiagnosticSummaryCarriesTheKeyNumbers() {
  fpsopto::Diagnostics diagnostics;
  diagnostics.frameMs = 16.6;
  diagnostics.fps = 60.0;
  diagnostics.cpuMs = 4.0;
  diagnostics.gpuMs = 12.0;
  diagnostics.glDropRate = 0.25;
  diagnostics.glCalls = 1000;
  diagnostics.gpuBound = true;

  char buffer[fpsopto::kDiagnosticsTextMax] = {};
  diagnostics.writeSummary(buffer);
  const std::string text(buffer);
  EXPECT_TRUE(text.find("16.6 ms") != std::string::npos);
  EXPECT_TRUE(text.find("60 fps") != std::string::npos);
  EXPECT_TRUE(text.find("gpu") != std::string::npos);
  EXPECT_TRUE(text.find("25%") != std::string::npos);

  // The summary must fit the buffer the overlay sizes for, and a null pointer
  // must not be written through.
  EXPECT_TRUE(diagnostics.summary().size() < fpsopto::kDiagnosticsTextMax);
  diagnostics.writeSummary(nullptr);
}

} // namespace

void runPolicyTests() {
  gpuBoundShedsCpuFeaturesAfterAConfirmationPeriod();
  losingGpuBoundRestoresCpuFeatures();
  pacedFramesDoNotFlipTheMode();
  unknownBalanceChangesNothing();
  lowYieldHooksAreDisabledAndOnlyRevivedOnRealEvidence();

  aFirstLaunchIsNormal();
  anOpenMarkerMeansSafeMode();
  consecutiveFailuresAccumulate();
  stabilityNeedsBothTimeAndFrames();
  safeModeKeepsItsOwnMarkerOpen();
  markerRoundTripsThroughDisk();

  theDiagnosticSummaryCarriesTheKeyNumbers();
}
