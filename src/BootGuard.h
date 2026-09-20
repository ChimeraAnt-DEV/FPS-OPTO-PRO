#pragma once

// Crash-loop guard.
//
// The instant-close failure mode is the worst one this mod has: the game dies
// during startup, so the user cannot reach the Mod Menu to turn the mod off,
// and the next launch dies the same way. Nothing about the failure is
// recoverable from inside the process that is crashing.
//
// The guard is a two-file handshake across launches:
//
//   * While running, a "booting" marker is held open and the mod is considered
//     unproven.
//   * Once the frame loop has been stable for a few seconds, the marker is
//     retired and a "clean" marker is written.
//   * On the next load, a marker that is still open means the previous launch
//     never got that far -- it crashed, was killed, or hung during startup.
//
// Being killed by the system for memory pressure looks the same as a crash
// from here. That is deliberate: a launch that never reached steady state is
// not evidence that the mod is safe to enable, regardless of why.
//
// The state machine is pure so the decision can be tested exhaustively; the
// file handling around it lives in BootGuard.cpp.

#include <cstdint>

namespace fpsopto {

enum class BootOutcome : unsigned char {
  // Fresh state, or a launch that is proven stable: run normally.
  Normal,
  // The previous launch never reached steady state. Run with every hook off.
  SafeMode,
};

struct BootDecision {
  BootOutcome outcome = BootOutcome::Normal;
  // Number of consecutive unproven launches, including this one. Exposed for
  // the log line, which is the only place a user can see why safe mode started.
  int consecutiveFailures = 0;
};

// How long the frame loop must be stable before the launch counts as proven.
// Long enough that a crash triggered by the first few frames -- the common
// case for a bad hook -- is caught, short enough that it is not a meaningful
// delay to the user.
inline constexpr std::uint64_t kStableUptimeNs = 8ull * 1000ull * 1000ull * 1000ull;

// At least this many frames must have been presented as well, so a launch that
// hung after presenting nothing is not mistaken for one that ran fine.
inline constexpr std::uint64_t kStableFrames = 90;

struct BootState {
  bool markerOpen = false;
  int consecutiveFailures = 0;
};

// The decision made at load, from the marker found on disk.
constexpr BootDecision decideAtLoad(const BootState &state) {
  BootDecision decision;
  if (!state.markerOpen) {
    decision.outcome = BootOutcome::Normal;
    decision.consecutiveFailures = 0;
    return decision;
  }
  decision.outcome = BootOutcome::SafeMode;
  decision.consecutiveFailures = state.consecutiveFailures + 1;
  return decision;
}

// The marker state to persist at load. A launch that starts in safe mode opens
// its own marker again: if safe mode itself does not survive startup, that is
// the strongest possible evidence the problem is not one of the opt-in hooks,
// and the next launch should know it happened twice.
constexpr BootState stateAfterLoad(const BootDecision &decision) {
  BootState next;
  next.markerOpen = true;
  next.consecutiveFailures = decision.consecutiveFailures;
  return next;
}

// Whether the running launch has earned the right to retire its marker. Both
// conditions must hold: time alone does not prove the loop ran, and frames
// alone would be satisfied by a burst during a long stall.
constexpr bool isProvenStable(std::uint64_t uptimeNs, std::uint64_t frames) {
  return uptimeNs >= kStableUptimeNs && frames >= kStableFrames;
}

// The marker state to persist once the launch is proven stable.
constexpr BootState stateAfterStable() {
  BootState next;
  next.markerOpen = false;
  next.consecutiveFailures = 0;
  return next;
}

} // namespace fpsopto