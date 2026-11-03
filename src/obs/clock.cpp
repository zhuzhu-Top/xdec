#include "xdec/obs/clock.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <string>

namespace xdec::obs {
namespace {

std::chrono::steady_clock::time_point origin() noexcept {
  static const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  return start;
}

// Touch the origin during static initialisation so "since start" means since
// process start rather than since the first log line.
[[maybe_unused]] const auto kOriginInit = origin();

}  // namespace

uint64_t toMonotonicNs(std::chrono::steady_clock::time_point point) noexcept {
  const auto delta = point - origin();
  return delta.count() <= 0
             ? 0
             : static_cast<uint64_t>(
                   std::chrono::duration_cast<std::chrono::nanoseconds>(delta).count());
}

uint64_t monotonicNs() noexcept { return toMonotonicNs(std::chrono::steady_clock::now()); }

uint32_t threadIndex() noexcept {
  static std::atomic<uint32_t> next{0};
  thread_local const uint32_t index = next.fetch_add(1, std::memory_order_relaxed);
  return index;
}

bool parseDuration(std::string_view text, std::chrono::nanoseconds& out) noexcept {
  if (text.empty()) {
    return false;
  }
  const std::string input{text};
  char* parsedEnd = nullptr;
  errno = 0;
  const double value = std::strtod(input.c_str(), &parsedEnd);
  if (errno == ERANGE || parsedEnd == input.c_str() || value < 0) {
    return false;
  }
  const std::string_view unit{
      parsedEnd, static_cast<std::size_t>(input.c_str() + input.size() - parsedEnd)};
  double seconds = 0;
  if (unit.empty() || unit == "s") {
    seconds = value;
  } else if (unit == "ms") {
    seconds = value / 1000.0;
  } else if (unit == "m" || unit == "min") {
    seconds = value * 60.0;
  } else if (unit == "h") {
    seconds = value * 3600.0;
  } else {
    return false;
  }
  out = std::chrono::nanoseconds{static_cast<int64_t>(seconds * 1e9)};
  return true;
}

}  // namespace xdec::obs
