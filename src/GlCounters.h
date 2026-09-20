#pragma once

// Filter bookkeeping, kept off the hot path.
//
// Every filtered GL call used to touch two process-wide atomics. That is a
// contended store in the middle of the render loop, and it grows worse the
// more worker threads draw. Here each thread accumulates into plain (non
// atomic) storage it owns, and only when its local batch fills does it fold
// the batch into the shared totals with one relaxed atomic add per counter.
// The shared side is read rarely (the overlay, the log line, the auto policy),
// so it only ever needs to be approximately current.
//
// The counters are also kept per entry point. A hook that drops almost nothing
// is pure overhead, and the auto policy needs the per-hook number to notice.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fpsopto {

// The state entry points this mod can drop a call on. The order is arbitrary;
// the names are what the log and the overlay report.
enum class GlHook : std::uint8_t {
  Enable,
  Disable,
  BlendFunc,
  BlendFuncSeparate,
  BlendEquation,
  BlendEquationSeparate,
  BlendColor,
  DepthFunc,
  DepthMask,
  ColorMask,
  CullFace,
  FrontFace,
  DepthRangef,
  SampleCoverage,
  Viewport,
  UseProgram,
  ActiveTexture,
  BindBuffer,
  BindTexture,
  BindFramebuffer,
  BindVertexArray,
  PixelStorei,
  Count,
};

inline constexpr std::size_t kGlHookCount = static_cast<std::size_t>(GlHook::Count);

// Human-readable name, for the overlay and the log. Kept as a lookup rather
// than magic_enum so this header stays dependency-free for the host build.
constexpr const char *glHookName(GlHook hook) {
  switch (hook) {
  case GlHook::Enable: return "glEnable";
  case GlHook::Disable: return "glDisable";
  case GlHook::BlendFunc: return "glBlendFunc";
  case GlHook::BlendFuncSeparate: return "glBlendFuncSeparate";
  case GlHook::BlendEquation: return "glBlendEquation";
  case GlHook::BlendEquationSeparate: return "glBlendEquationSeparate";
  case GlHook::BlendColor: return "glBlendColor";
  case GlHook::DepthFunc: return "glDepthFunc";
  case GlHook::DepthMask: return "glDepthMask";
  case GlHook::ColorMask: return "glColorMask";
  case GlHook::CullFace: return "glCullFace";
  case GlHook::FrontFace: return "glFrontFace";
  case GlHook::DepthRangef: return "glDepthRangef";
  case GlHook::SampleCoverage: return "glSampleCoverage";
  case GlHook::Viewport: return "glViewport";
  case GlHook::UseProgram: return "glUseProgram";
  case GlHook::ActiveTexture: return "glActiveTexture";
  case GlHook::BindBuffer: return "glBindBuffer";
  case GlHook::BindTexture: return "glBindTexture";
  case GlHook::BindFramebuffer: return "glBindFramebuffer";
  case GlHook::BindVertexArray: return "glBindVertexArray";
  case GlHook::PixelStorei: return "glPixelStorei";
  case GlHook::Count: break;
  }
  return "?";
}

struct HookTraffic {
  std::uint64_t calls = 0;
  std::uint64_t suppressed = 0;

  [[nodiscard]] double dropRate() const {
    if (calls == 0) {
      return 0.0;
    }
    return static_cast<double>(suppressed) / static_cast<double>(calls);
  }
};

// Sum of every hook's traffic plus the per-hook breakdown.
struct GlTrafficSnapshot {
  HookTraffic total{};
  HookTraffic perHook[kGlHookCount]{};
  std::uint64_t hooksDisabled = 0;

  [[nodiscard]] double dropRate() const { return total.dropRate(); }
};

class GlCounters {
public:
  static GlCounters &instance() {
    static GlCounters counters;
    return counters;
  }

  // Called on the calling thread for every filtered call. Deliberately
  // non-atomic on the fast path: it touches only this thread's batch.
  void note(GlHook hook, bool suppressed) noexcept {
    auto &batch = local();
    const auto index = static_cast<std::size_t>(hook);
    ++batch.calls[index];
    if (suppressed) {
      ++batch.suppressed[index];
    }
    if (++batch.pending >= kFlushEvery) {
      flush(batch);
    }
  }

  // Folds this thread's outstanding batch into the shared totals. Called when
  // a batch fills and once per frame by the swap path, so a thread that goes
  // quiet does not strand the last few counts.
  void publish() noexcept { flush(local()); }

  [[nodiscard]] GlTrafficSnapshot snapshot() const {
    GlTrafficSnapshot out;
    for (std::size_t i = 0; i < kGlHookCount; ++i) {
      out.perHook[i].calls = mCalls[i].load(std::memory_order_relaxed);
      out.perHook[i].suppressed =
          mSuppressed[i].load(std::memory_order_relaxed);
      out.total.calls += out.perHook[i].calls;
      out.total.suppressed += out.perHook[i].suppressed;
    }
    return out;
  }

  // Only meaningful on the host, where there is one logical render thread. On
  // device the totals are monotonic, which is what the overlay wants anyway.
  void reset() noexcept {
    for (std::size_t i = 0; i < kGlHookCount; ++i) {
      mCalls[i].store(0, std::memory_order_relaxed);
      mSuppressed[i].store(0, std::memory_order_relaxed);
    }
    local() = LocalBatch{};
  }

private:
  // How many calls a thread absorbs before paying for the atomic fold. Large
  // enough to matter, small enough that the overlay is not showing stale
  // numbers after a hitch.
  static constexpr std::uint32_t kFlushEvery = 64;

  struct LocalBatch {
    std::uint64_t calls[kGlHookCount]{};
    std::uint64_t suppressed[kGlHookCount]{};
    std::uint32_t pending = 0;
  };

  static LocalBatch &local() {
    static thread_local LocalBatch batch;
    return batch;
  }

  static void flush(LocalBatch &batch) noexcept {
    auto &self = instance();
    if (batch.pending == 0) {
      return;
    }
    for (std::size_t i = 0; i < kGlHookCount; ++i) {
      if (batch.calls[i] != 0) {
        self.mCalls[i].fetch_add(batch.calls[i], std::memory_order_relaxed);
        batch.calls[i] = 0;
      }
      if (batch.suppressed[i] != 0) {
        self.mSuppressed[i].fetch_add(batch.suppressed[i],
                                      std::memory_order_relaxed);
        batch.suppressed[i] = 0;
      }
    }
    batch.pending = 0;
  }

  std::atomic<std::uint64_t> mCalls[kGlHookCount]{};
  std::atomic<std::uint64_t> mSuppressed[kGlHookCount]{};
};

} // namespace fpsopto