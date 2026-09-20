#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "GlStateModel.h"

namespace fpsopto {

struct GlHookStats {
  std::uint64_t stateCalls = 0;
  std::uint64_t suppressed = 0;

  // Share of state calls that never reached the driver. A suppressed call is
  // cheaper than an issued one, but only the driver can say by how much, so
  // this is a traffic counter rather than a timing claim.
  double suppressionRatio() const {
    if (stateCalls == 0) {
      return 0.0;
    }
    return static_cast<double>(suppressed) / static_cast<double>(stateCalls);
  }
};

// Installs detours on the GL ES state entry points and keeps a shadow of the
// context so redundant calls can be dropped.
//
// Installs every hook it can and reports which ones failed, so a device whose
// driver exports only a subset still gets that subset.
class GlInterceptor {
public:
  static GlInterceptor &instance();

  // `moduleName` is the shared object exporting the GL entry points, normally
  // "libGLESv2.so".
  bool install(const std::string &moduleName);
  void uninstall();

  [[nodiscard]] bool installed() const { return mInstalledCount > 0; }
  [[nodiscard]] int installedCount() const { return mInstalledCount; }
  [[nodiscard]] int failedCount() const { return mFailedCount; }
  [[nodiscard]] const std::vector<std::string> &failedHooks() const {
    return mFailedHooks;
  }
  [[nodiscard]] const GlHookStats &stats() const { return mStats; }

  // Records one call that passed through the filter. Called by the detours.
  void noteCall(bool suppressed) {
    ++mStats.stateCalls;
    if (suppressed) {
      ++mStats.suppressed;
    }
  }

  // Resets the shadow for a context that has just become current. Called for a
  // fresh context or when a different context replaces the previous one; the
  // state a context is required to start at is what `freshContextState`
  // returns.
  void onContextChanged();

  GlStateModel mState = freshContextState();
  TextureUnits mTextures{};

private:
  GlInterceptor() = default;

  bool mInstalled = false;
  int mInstalledCount = 0;
  int mFailedCount = 0;
  std::vector<std::string> mFailedHooks;
  GlHookStats mStats{};
};

} // namespace fpsopto