// Target identification shared by the binary loader and the architecture layer.
//
// This lives in the support layer, which both depend on, so that the
// architecture layer never has to include the binary loader just to name an
// instruction set.
#pragma once

#include <cstdint>
#include <string_view>

namespace xdec {

enum class Endian : uint8_t {
  Little,
  Big,
};

[[nodiscard]] std::string_view toString(Endian endian) noexcept;

enum class Arch : uint8_t {
  Unknown = 0,
  AArch64,
  Arm32,
  Thumb,
  X86,
  X86_64,
  RiscV32,
  RiscV64,
  Mips32,
  Mips64,
  PowerPc64,
  LoongArch64,
};

/// Canonical lowercase name, also the directory name under `spec/`.
[[nodiscard]] std::string_view toString(Arch arch) noexcept;
[[nodiscard]] bool parseArch(std::string_view name, Arch& out) noexcept;

/// Natural pointer width in bits; 0 when unknown.
[[nodiscard]] unsigned pointerBits(Arch arch) noexcept;

/// Fixed instruction length in bytes, or 0 for variable-length encodings.
/// Instruction fetching uses this to size lookahead windows.
[[nodiscard]] unsigned fixedInstructionSize(Arch arch) noexcept;

}  // namespace xdec
