// splitmix64: the deterministic fill behind `memfill` directives, corpus
// cases, and the differential driver's random memory. It lives in support
// because three sides must agree on the byte stream: the xdec CLI, the C++
// tests, and tools/diff_unicorn.py (which reimplements it — keep them in
// sync!).
#pragma once

#include <cstdint>

namespace xdec {

[[nodiscard]] inline uint64_t splitmix64Next(uint64_t& state) {
  state += 0x9E3779B97F4A7C15ull;
  uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

}  // namespace xdec
