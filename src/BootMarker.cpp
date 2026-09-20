#include "BootMarker.h"

#include <fstream>
#include <string>

namespace fpsopto {
namespace {

// The marker is a tiny text file so its contents are readable in a bug report.
// "open" means a launch is in progress and has not yet proven itself.
constexpr const char *kMarkerFileName = "boot.marker";

// Reads the marker back into its two fields. Returns false when no marker is
// present at all, which is the first launch after install.
bool readMarker(const std::filesystem::path &path, BootState &state) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return false;
  }
  std::string first;
  if (!std::getline(stream, first)) {
    return false;
  }
  state.markerOpen = first == "open";
  // A marker with no readable count is still a marker: the launch it describes
  // is evidence regardless of whether the count survived with it.
  std::string second;
  if (std::getline(stream, second)) {
    try {
      state.consecutiveFailures = std::stoi(second);
    } catch (...) {
      state.consecutiveFailures = 0;
    }
  }
  return true;
}

} // namespace

BootMarker &BootMarker::instance() {
  static BootMarker marker;
  return marker;
}

void BootMarker::writeMarker(bool open, int consecutiveFailures) {
  if (mMarkerPath.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(mMarkerPath.parent_path(), ec);

  std::ofstream stream(mMarkerPath, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return;
  }
  // Written as two lines so a human reading the file sees the state and the
  // failure count without needing the format explained.
  stream << (open ? "open" : "clean") << '\n' << consecutiveFailures << '\n';
}

BootDecision BootMarker::begin(const std::filesystem::path &dataDir) {
  if (mBegan) {
    BootDecision decision;
    decision.outcome = mSafeMode ? BootOutcome::SafeMode : BootOutcome::Normal;
    decision.consecutiveFailures = mConsecutiveFailures;
    return decision;
  }
  mBegan = true;

  mMarkerPath = dataDir / kMarkerFileName;

  BootState previous;
  readMarker(mMarkerPath, previous);

  const BootDecision decision = decideAtLoad(previous);
  mSafeMode = decision.outcome == BootOutcome::SafeMode;
  mConsecutiveFailures = decision.consecutiveFailures;
  mRetired = false;

  // A clean launch opens its marker; a safe-mode launch opens it too, so that
  // a safe mode which itself fails to start is recorded as a second failure.
  writeMarker(true, mConsecutiveFailures);
  return decision;
}

void BootMarker::noteFrame(std::uint64_t uptimeNs, std::uint64_t frames) {
  if (!mBegan || mRetired) {
    return;
  }
  if (!isProvenStable(uptimeNs, frames)) {
    return;
  }
  mRetired = true;
  const BootState next = stateAfterStable();
  mConsecutiveFailures = next.consecutiveFailures;
  writeMarker(next.markerOpen, next.consecutiveFailures);
}

} // namespace fpsopto