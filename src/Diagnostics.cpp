#include "Diagnostics.h"

#include <cstdio>

namespace fpsopto {

void Diagnostics::writeSummary(char *out) const {
  if (out == nullptr) {
    return;
  }

  const char *bound = gpuBound ? "gpu" : (cpuBound ? "cpu" : "?");
  const char *pacing = paced ? " vsync" : "";

  // Two lines on purpose: the overlay renders each line as its own row, and
  // the first line is the one a user reads at a glance. std::snprintf always
  // terminates, so a clipped line is readable rather than a buffer overrun.
  std::snprintf(out, kDiagnosticsTextMax,
                "%.1f ms  %.0f fps | cpu %.1f ms  gpu %.1f ms%s | %s\n"
                "GL dropped %.0f%% of %llu | %s%s%s",
                frameMs, fps, cpuMs, gpuMs, pacing, bound,
                glDropRate * 100.0,
                static_cast<unsigned long long>(glCalls),
                filterActive ? "" : "filter off  ",
                schedulerActive ? "" : "sched off  ",
                safeMode ? "SAFE MODE" : "");
  out[kDiagnosticsTextMax - 1] = '\0';
}

std::string Diagnostics::summary() const {
  char buffer[kDiagnosticsTextMax] = {};
  writeSummary(buffer);
  return std::string(buffer);
}

} // namespace fpsopto