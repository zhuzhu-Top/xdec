// A small AArch64-shaped register file for the IL tests.
//
// Deliberately not the full architectural file: the IL tests are about the IR,
// not about ARM. The real one is declared by the instruction semantics DSL.
#pragma once

#include <string>

#include "xdec/il/register_file.h"

namespace xdec::test {

/// x0..x7 with their w views, plus sp, xzr and nzcv.
inline const il::RegisterFile& arm64Registers() {
  static const il::RegisterFile file = [] {
    il::RegisterFile registers;
    for (unsigned index = 0; index < 8; ++index) {
      const il::RegId full =
          registers.add("x" + std::to_string(index), 64, il::RegClass::General);
      // A w-register write zeroes the upper half of its x register. Modelling
      // that here rather than in a pass is what keeps a 32-bit write followed by
      // a 64-bit read from producing a stale value.
      registers.addSubRegister("w" + std::to_string(index), full, 0, 32, true);
    }
    registers.add("sp", 64, il::RegClass::StackPointer);
    registers.add("xzr", 64, il::RegClass::Zero);
    registers.add("nzcv", 0, il::RegClass::Flags);
    // One vector register, with both view shapes: `s0` is the low lane, whose
    // write zeroes the rest, and `q0_hi` is the `ins v0.d[1]` destination,
    // whose write must preserve the low half. Register SSA does not track the
    // vector class, so these exercise the paths where reads and writes stay
    // ops rather than becoming versions.
    const il::RegId vector = registers.add("q0", 128, il::RegClass::Vector);
    registers.addSubRegister("s0", vector, 0, 32, true);
    registers.addSubRegister("q0_hi", vector, 64, 64, false);
    return registers;
  }();
  return file;
}

}  // namespace xdec::test
