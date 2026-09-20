#pragma once

// The crash-loop guard, wired to the filesystem.
//
// BootGuard.h holds the pure decision; this owns the marker file and the
// process-lifetime knowledge of whether the current launch is in safe mode.
// The marker lives in the mod's data directory, which the launcher gives the
// mod and which survives a crash (unlike anything held in memory).

#include <cstdint>
#include <filesystem>

#include "BootGuard.h"

namespace fpsopto {

class BootMarker {
public:
  static BootMarker &instance();

  // Reads the marker left by the previous launch and decides how this one
  // starts. Idempotent: a second call returns the first decision, because by
  // then this launch has written its own marker.
  BootDecision begin(const std::filesystem::path &dataDir);

  // Retires the marker once the frame loop is proven stable. Safe to call
  // every frame; only the transition does work.
  void noteFrame(std::uint64_t uptimeNs, std::uint64_t frames);

  [[nodiscard]] bool safeMode() const { return mSafeMode; }
  [[nodiscard]] int consecutiveFailures() const { return mConsecutiveFailures; }
  [[nodiscard]] bool began() const { return mBegan; }

  // Test seam: run the handshake again against a fresh directory.
  void resetForTest() {
    mBegan = false;
    mSafeMode = false;
    mRetired = false;
    mConsecutiveFailures = 0;
    mMarkerPath.clear();
  }

private:
  BootMarker() = default;

  void writeMarker(bool open, int consecutiveFailures);

  bool mBegan = false;
  bool mSafeMode = false;
  bool mRetired = false;
  int mConsecutiveFailures = 0;
  std::filesystem::path mMarkerPath;
};

} // namespace fpsopto