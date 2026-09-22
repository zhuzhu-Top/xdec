// Lightweight wall-time accumulators for the interpreter's hot loop.
//
// The exec session runs tens of millions of instructions per trace, so any
// instrumentation here has to be a single pointer check plus a
// steady_clock::now() pair, not a string-keyed registry. Callers own the
// PerfCounter fields (see xdec::exec::ExecSessionPerfStats) and pass a
// nullable pointer through; ScopedTimer skips both clock reads when the
// pointer is null, so unmeasured callers pay nothing beyond the check.
#pragma once

#include <chrono>
#include <cstdint>

namespace xdec {

struct PerfCounter {
  uint64_t totalNs = 0;
  uint64_t count = 0;

  void add(uint64_t ns) noexcept {
    totalNs += ns;
    ++count;
  }
};

class ScopedTimer {
 public:
  explicit ScopedTimer(PerfCounter* counter) noexcept
      : counter_(counter), start_(counter == nullptr ? Clock::time_point{} : Clock::now()) {}

  ~ScopedTimer() {
    if (counter_ != nullptr) {
      counter_->add(static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_)
              .count()));
    }
  }

  ScopedTimer(const ScopedTimer&) = delete;
  ScopedTimer& operator=(const ScopedTimer&) = delete;

 private:
  using Clock = std::chrono::steady_clock;
  PerfCounter* counter_;
  Clock::time_point start_;
};

}  // namespace xdec
