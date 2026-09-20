#pragma once

#include <cstdint>

namespace fpsopto {

// Optional Android Dynamic Performance Framework hint session.
//
// ADPF lets a process tell the platform "these threads must finish N
// nanoseconds of work every M nanoseconds", and the platform responds by
// choosing operating points for them. This is a cooperative interface: it is
// advisory, the platform may ignore it, and the failure modes are real.
//
// The specific hazard is that the session is created with a *duration*, and a
// duration that is too aggressive pushes the platform's DVFS governor up
// before the workload justifies it, which on a thermally limited device costs
// more than it gains. That is why it is opt-in and why the target duration is
// only ever moved toward the measured frame time, never below it.
class AdpfSession {
public:
  static AdpfSession &instance();

  // Returns false when the platform has no performance hint manager (anything
  // before Android 12) or when the session could not be created. Either is a
  // normal outcome, not an error.
  bool start(int targetFps);

  void stop();

  [[nodiscard]] bool active() const { return mActive; }

  // Records how long the frame actually took so the platform can compare it
  // with the target it was given.
  void reportFrame(std::uint64_t actualDurationNs);

  // Narrows or widens the target workload duration.
  void updateTarget(std::uint64_t targetDurationNs);

private:
  AdpfSession() = default;

  bool mActive = false;
  std::uint64_t mTargetNs = 0;
  bool mPreferEfficiency = false;
};

} // namespace fpsopto