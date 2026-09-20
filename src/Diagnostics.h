#pragma once

// The live diagnostic snapshot.
//
// Everything the mod measures, in one value the overlay, the log, and the
// tests all format from. A single source means the menu cannot show a number
// that disagrees with the log, which is what makes the readout trustworthy
// enough to decide whether the mod is helping.

#include <cstdint>
#include <string>

namespace fpsopto {

inline constexpr std::size_t kDiagnosticsTextMax = 256;

struct Diagnostics {
  // Frame timing, from the accumulator.
  double frameMs = 0.0;
  double fps = 0.0;
  double cpuMs = 0.0;
  double gpuMs = 0.0;
  double cpuLoad = 0.0;
  double gpuLoad = 0.0;
  double cadenceMs = 0.0;
  bool gpuTiming = false;
  bool paced = false;

  // Filter effectiveness.
  double glDropRate = 0.0;
  std::uint64_t glCalls = 0;
  std::uint64_t glSuppressed = 0;
  std::uint64_t hooksDisabled = 0;

  // Which resource the frame time is dominated by, as the governor sees it.
  bool gpuBound = false;
  bool cpuBound = false;

  // Levers currently active, so the readout says what is being paid for.
  bool filterActive = true;
  bool schedulerActive = true;
  // Set by the crash-loop guard when this launch is running with the hooks
  // installed but inert.
  bool safeMode = false;

  // Free-form status line. Rendered as a single row so the overlay stays a
  // few lines tall.
  std::string notes;

  // Writes a two-line summary into `out`, which must hold at least
  // kDiagnosticsTextMax bytes. A null pointer is ignored.
  void writeSummary(char *out) const;
  std::string summary() const;
};

} // namespace fpsopto