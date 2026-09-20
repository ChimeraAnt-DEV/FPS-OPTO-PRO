#pragma once

// Pure frame-timing accumulator. Header-only and dependency-free so the
// classification logic can be exercised on the host without a GL context.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>

namespace fpsopto {

// One frame as observed at the buffer-swap boundary.
struct FrameSample {
  std::uint64_t frameNs = 0; // swap-to-swap interval
  std::uint64_t cpuNs = 0;   // time spent inside the swap call on this thread
  std::uint64_t gpuNs = 0;   // GPU completion time, 0 when unmeasurable
  bool gpuMeasured = false;
  // True when the previous frame's GPU work was still running at this swap,
  // which by itself is the strongest evidence that the GPU is the limit.
  bool gpuOverran = false;
};

struct FrameStats {
  double frameMs = 0.0;
  double fps = 0.0;
  double cpuMs = 0.0;
  double gpuMs = 0.0;
  double cpuLoad = 0.0;
  double gpuLoad = 0.0;
  double medianFrameMs = 0.0;
  double cadenceMs = 0.0;
  bool gpuTiming = false;
  std::uint64_t frames = 0;
};

// Rolling window kept as a small ring. A window of modest size is deliberate:
// frame time on mobile is non-stationary (thermal state and scene complexity
// both move it), so a long average describes a game state that no longer
// exists.
template <std::size_t Window = 120> class FrameAccumulator {
public:
  void push(const FrameSample &sample) {
    if (sample.frameNs == 0) {
      return;
    }

    const double frameMs = toMs(sample.frameNs);
    if (mCount == 0) {
      mEmaFrameMs = frameMs;
      mEmaCpuMs = toMs(sample.cpuNs);
      mEmaGpuMs = toMs(sample.gpuNs);
    } else {
      mEmaFrameMs += kEmaAlpha * (frameMs - mEmaFrameMs);
      mEmaCpuMs += kEmaAlpha * (toMs(sample.cpuNs) - mEmaCpuMs);
      if (sample.gpuMeasured) {
        mEmaGpuMs += kEmaAlpha * (toMs(sample.gpuNs) - mEmaGpuMs);
      }
    }

    mWindow[mNext] = frameMs;
    mNext = (mNext + 1) % Window;
    if (mCount < Window) {
      ++mCount;
    }

    if (sample.gpuMeasured) {
      mGpuTiming = true;
      mGpuVotes += sample.gpuOverran ? 2 : 0;
      if (mEmaGpuMs > mEmaFrameMs * kGpuBoundRatio) {
        ++mGpuVotes;
      }
    } else if (sample.gpuOverran) {
      // An overrun is a fence-poll result, so it is a valid signal even on a
      // driver that cannot report how long the GPU took.
      mGpuTiming = true;
      mGpuVotes += 2;
    }
    mGpuVotes = std::min<std::uint32_t>(mGpuVotes, kMaxVotes);

    // The fastest frame the device has managed is the best available estimate
    // of the display cadence, because a frame cannot be presented faster than
    // the swap chain will accept it. It is allowed to drift upward so that a
    // device which got faster (or throttled) is not judged against a cadence
    // it can no longer reach.
    if (frameMs < mBestFrameMs || mBestFrameMs <= 0.0) {
      mBestFrameMs = frameMs;
      mFramesAtBest = 0;
    } else if (++mFramesAtBest > kBestDecayFrames) {
      mBestFrameMs *= kBestDecayFactor;
      mFramesAtBest = 0;
    }

    ++mFrames;
  }

  void reset() { *this = FrameAccumulator{}; }

  [[nodiscard]] FrameStats stats() const {
    FrameStats out;
    out.frames = mFrames;
    out.frameMs = mEmaFrameMs;
    out.fps = mEmaFrameMs > 0.0 ? 1000.0 / mEmaFrameMs : 0.0;
    out.cpuMs = mEmaCpuMs;
    out.gpuMs = mEmaGpuMs;
    out.gpuTiming = mGpuTiming;
    if (mEmaFrameMs > 0.0) {
      out.cpuLoad = mEmaCpuMs / mEmaFrameMs;
      out.gpuLoad = mGpuTiming ? mEmaGpuMs / mEmaFrameMs : 0.0;
    }
    out.medianFrameMs = median();
    out.cadenceMs = mBestFrameMs;
    return out;
  }

  // The fastest swap-to-swap interval seen recently. It is the only cadence
  // estimate available to a mod that never sees a display mode, and unlike a
  // nominal refresh rate it cannot come from a panel the GPU is not driving.
  [[nodiscard]] double cadenceMs() const { return mBestFrameMs; }

  [[nodiscard]] std::size_t sampleCount() const { return mCount; }

  // A frame time that sits within a fraction of a millisecond of the cadence,
  // with a tight spread, is the swap chain pacing the game rather than the
  // game being fast. The governor must not read a locked frame time as "the
  // CPU has headroom".
  [[nodiscard]] bool vsyncLocked(double toleranceMs = 0.6) const {
    if (mCount < Window / 4 || mBestFrameMs <= 0.0) {
      return false;
    }
    if (std::abs(mEmaFrameMs - mBestFrameMs) > toleranceMs) {
      return false;
    }
    return spreadMs() <= toleranceMs;
  }

  [[nodiscard]] bool gpuBound() const {
    if (mGpuVotes >= kBoundVotes) {
      return true;
    }
    return mGpuTiming && mEmaFrameMs > 0.0 &&
           mEmaGpuMs > mEmaFrameMs * kGpuBoundRatio;
  }

  [[nodiscard]] bool cpuBound() const {
    return !gpuBound() && mEmaFrameMs > 0.0 &&
           mEmaCpuMs > mEmaFrameMs * kCpuBoundRatio;
  }

  [[nodiscard]] std::uint32_t gpuVotes() const { return mGpuVotes; }

private:
  static constexpr double kEmaAlpha = 0.1;
  static constexpr double kGpuBoundRatio = 0.85;
  static constexpr double kCpuBoundRatio = 0.7;
  static constexpr std::uint32_t kBoundVotes = 6;
  static constexpr std::uint32_t kMaxVotes = 1000;
  static constexpr std::size_t kBestDecayFrames = 3600;
  static constexpr double kBestDecayFactor = 1.05;

  static double toMs(std::uint64_t ns) {
    return static_cast<double>(ns) / 1.0e6;
  }

  [[nodiscard]] double median() const {
    if (mCount == 0) {
      return 0.0;
    }
    std::array<double, Window> sorted{};
    std::copy(mWindow.begin(), mWindow.begin() + mCount, sorted.begin());
    std::sort(sorted.begin(), sorted.begin() + mCount);
    return sorted[mCount / 2];
  }

  [[nodiscard]] double spreadMs() const {
    if (mCount < 2) {
      return 0.0;
    }
    double lo = mWindow[0];
    double hi = mWindow[0];
    for (std::size_t i = 1; i < mCount; ++i) {
      lo = std::min(lo, mWindow[i]);
      hi = std::max(hi, mWindow[i]);
    }
    return hi - lo;
  }

  std::array<double, Window> mWindow{};
  std::size_t mNext = 0;
  std::size_t mCount = 0;
  double mEmaFrameMs = 0.0;
  double mEmaCpuMs = 0.0;
  double mEmaGpuMs = 0.0;
  bool mGpuTiming = false;
  std::uint32_t mGpuVotes = 0;
  std::uint64_t mFrames = 0;
  double mBestFrameMs = 0.0;
  std::size_t mFramesAtBest = 0;
};

} // namespace fpsopto