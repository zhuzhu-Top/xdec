#include "xdec/obs/metrics.h"

#include <algorithm>
#include <mutex>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <unistd.h>

#include <cstdio>
#endif

namespace xdec::obs {
namespace {

struct Registry {
  std::mutex mutex;
  std::vector<Counter*> counters;
};

Registry& registry() {
  static auto* instance = new Registry;
  return *instance;
}

}  // namespace

Counter::Counter(std::string_view name) noexcept : name_(name) {
  Registry& reg = registry();
  const std::lock_guard lock{reg.mutex};
  reg.counters.push_back(this);
}

std::vector<std::pair<std::string_view, uint64_t>> counterSnapshot() {
  Registry& reg = registry();
  std::vector<std::pair<std::string_view, uint64_t>> out;
  {
    const std::lock_guard lock{reg.mutex};
    out.reserve(reg.counters.size());
    for (const Counter* counter : reg.counters) {
      out.emplace_back(counter->name(), counter->value());
    }
  }
  std::ranges::sort(out);
  return out;
}

void resetCounters() {
  Registry& reg = registry();
  const std::lock_guard lock{reg.mutex};
  for (Counter* counter : reg.counters) {
    counter->reset();
  }
}

MemoryUsage processMemory() noexcept {
  MemoryUsage usage;
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS counters{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
    usage.currentBytes = counters.WorkingSetSize;
    usage.peakBytes = counters.PeakWorkingSetSize;
  }
#else
  rusage self{};
  if (getrusage(RUSAGE_SELF, &self) == 0) {
#if defined(__APPLE__)
    usage.peakBytes = static_cast<uint64_t>(self.ru_maxrss);
#else
    usage.peakBytes = static_cast<uint64_t>(self.ru_maxrss) * 1024;
#endif
  }
#if defined(__linux__)
  if (std::FILE* statm = std::fopen("/proc/self/statm", "r"); statm != nullptr) {
    unsigned long size = 0;
    unsigned long resident = 0;
    if (std::fscanf(statm, "%lu %lu", &size, &resident) == 2) {
      usage.currentBytes = static_cast<uint64_t>(resident) *
                           static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    }
    std::fclose(statm);
  }
#endif
  if (usage.currentBytes == 0) {
    usage.currentBytes = usage.peakBytes;
  }
  if (usage.peakBytes < usage.currentBytes) {
    usage.peakBytes = usage.currentBytes;
  }
#endif
  return usage;
}

}  // namespace xdec::obs
