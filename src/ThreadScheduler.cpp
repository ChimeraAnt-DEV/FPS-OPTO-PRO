#include "ThreadScheduler.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <dirent.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

#include <android/log.h>

namespace fpsopto {
namespace {


// Nice values the game already assigned before this mod touched anything.
//
// Restoring a thread to a fixed default would clobber a priority the game set
// on purpose, and some of those (the audio mixer, the network thread) are
// deliberately raised. Recording the value first is what makes the back-out
// safe.
struct OriginalPriority {
  int nice = 0;
  bool captured = false;
};

std::unordered_map<pid_t, OriginalPriority> gOriginalPriority;

// Threads that sit on the frame-critical path. Matching on the name is the
// only handle a mod has; there is no public API to ask "which thread submits
// GL commands". Anything absent from this list is left completely alone,
// which is the conservative default given how much else shares the process.
constexpr const char *kRenderThreadNames[] = {
    "Render thread", "RenderThread", "RenderMain",
    "MTK Render",    "GLThread",     "GL thread",
    "GFX",           "gfx",
};

// Substring matches, for names that carry a per-worker suffix such as
// "Worker-3" or "ChunkBuilder_2".
constexpr const char *kRenderThreadPrefixes[] = {
    "Worker", "ChunkBuilder", "Chunk", "Mesh", "AsyncRunner",
};

bool isRenderThreadName(const char *name) {
  if (name == nullptr || name[0] == '\0') {
    return false;
  }
  for (const char *candidate : kRenderThreadNames) {
    if (std::strcmp(name, candidate) == 0) {
      return true;
    }
  }
  for (const char *prefix : kRenderThreadPrefixes) {
    if (std::strncmp(name, prefix, std::strlen(prefix)) == 0) {
      return true;
    }
  }
  return false;
}

size_t listThreadIds(std::vector<pid_t> &ids) {
  ids.clear();
  DIR *dir = opendir("/proc/self/task");
  if (dir == nullptr) {
    return 0;
  }
  while (dirent *entry = readdir(dir)) {
    if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
      continue;
    }
    ids.push_back(static_cast<pid_t>(std::strtol(entry->d_name, nullptr, 10)));
  }
  closedir(dir);
  return ids.size();
}

bool readThreadName(pid_t tid, char *out, size_t outSize) {
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
  FILE *file = std::fopen(path, "r");
  if (file == nullptr) {
    return false;
  }
  const bool ok = std::fgets(out, static_cast<int>(outSize), file) != nullptr;
  std::fclose(file);
  if (!ok) {
    return false;
  }
  const size_t len = std::strlen(out);
  if (len > 0 && out[len - 1] == '\n') {
    out[len - 1] = '\0';
  }
  return true;
}

bool readNice(pid_t tid, int &nice) {
  errno = 0;
  const int value = getpriority(PRIO_PROCESS, tid);
  if (errno != 0) {
    return false;
  }
  nice = value;
  return true;
}

// Only ever moves a thread toward more favourable scheduling, and records what
// it found first so the change can be handed back exactly.
void applyBoost(pid_t tid, int boost) {
  int current = 0;
  if (!readNice(tid, current)) {
    return;
  }

  auto &record = gOriginalPriority[tid];
  if (!record.captured) {
    record.nice = current;
    record.captured = true;
  }

  const int target = std::max(-20, current - boost);
  if (target != current) {
    setpriority(PRIO_PROCESS, tid, target);
  }
}

void restoreBoost(pid_t tid) {
  auto it = gOriginalPriority.find(tid);
  if (it == gOriginalPriority.end() || !it->second.captured) {
    return;
  }
  setpriority(PRIO_PROCESS, tid, it->second.nice);
  gOriginalPriority.erase(it);
}

template <typename Fn> void forEachRenderThread(Fn &&fn) {
  std::vector<pid_t> ids;
  if (listThreadIds(ids) == 0) {
    return;
  }
  for (pid_t tid : ids) {
    char name[32] = {};
    if (!readThreadName(tid, name, sizeof(name))) {
      continue;
    }
    if (isRenderThreadName(name)) {
      fn(tid);
    }
  }
}

} // namespace

void ThreadScheduler::onFrame(double frameMs, double cadenceMs, double cpuLoad,
                              double gpuLoad, bool gpuTimingAvailable,
                              bool gpuOverran, bool paced, bool workloadKnown) {
  if (!mRunning) {
    return;
  }

  GovernorInput input;
  input.frameMs = frameMs;
  input.cadenceMs = cadenceMs;
  input.cpuLoad = cpuLoad;
  input.gpuLoad = gpuLoad;
  input.gpuTimingAvailable = gpuTimingAvailable;
  input.gpuOverran = gpuOverran;
  input.paced = paced;
  input.workloadKnown = workloadKnown;
  input.maxBoost = mMaxBoost;

  // The policy is a handful of comparisons, so it runs every frame. Only the
  // part that walks the process's thread list is periodic: enumerating threads
  // and reading /proc is far too expensive to do per frame.
  const ScheduleDecision decision = decide(input);
  mLast = decision;

  if (++mFrameCounter % kSamplePeriodFrames != 0) {
    return;
  }

  if (decision.boost == mAppliedBoost) {
    return;
  }
  apply(decision);
}

void ThreadScheduler::apply(const ScheduleDecision &decision) {
  if (decision.boost > 0) {
    // Apply exactly what the policy chose. The governor distinguishes a small
    // safe lift (1) from a full lift under a confirmed CPU bound, and using
    // mMaxBoost here would silently promote every small lift to a full one.
    forEachRenderThread([&decision](pid_t tid) {
      applyBoost(tid, decision.boost);
    });
  } else if (mAppliedBoost > 0) {
    // A GPU-bound frame means the earlier lift is now counterproductive, so
    // every thread it touched is handed back its recorded priority.
    forEachRenderThread([](pid_t tid) { restoreBoost(tid); });
  }

  mAppliedBoost = decision.boost;
}

void ThreadScheduler::setMaxBoost(int maxBoost) {
  mMaxBoost = std::clamp(maxBoost, 0, 10);
}

void ThreadScheduler::start() {
  if (mRunning) {
    return;
  }
  mRunning = true;
  mFrameCounter = 0;
  mAppliedBoost = -1;
}

void ThreadScheduler::stop() {
  if (!mRunning) {
    return;
  }
  mRunning = false;
  if (mAppliedBoost > 0) {
    forEachRenderThread([](pid_t tid) { restoreBoost(tid); });
  }
  gOriginalPriority.clear();
  mAppliedBoost = -1;
}

ThreadScheduler &ThreadScheduler::instance() {
  static ThreadScheduler scheduler;
  return scheduler;
}

} // namespace fpsopto