#include "xdec/support/target.h"

namespace xdec {

std::string_view toString(Endian endian) noexcept {
  return endian == Endian::Little ? "little" : "big";
}

std::string_view toString(Arch arch) noexcept {
  switch (arch) {
    case Arch::Unknown:
      return "unknown";
    case Arch::AArch64:
      return "arm64";
    case Arch::Arm32:
      return "arm32";
    case Arch::Thumb:
      return "thumb";
    case Arch::X86:
      return "x86";
    case Arch::X86_64:
      return "x86_64";
    case Arch::RiscV32:
      return "riscv32";
    case Arch::RiscV64:
      return "riscv64";
    case Arch::Mips32:
      return "mips32";
    case Arch::Mips64:
      return "mips64";
    case Arch::PowerPc64:
      return "ppc64";
    case Arch::LoongArch64:
      return "loongarch64";
  }
  return "unknown";
}

bool parseArch(std::string_view name, Arch& out) noexcept {
  // Accepts the canonical name plus the common aliases people type.
  if (name == "arm64" || name == "aarch64") {
    out = Arch::AArch64;
  } else if (name == "arm32" || name == "arm") {
    out = Arch::Arm32;
  } else if (name == "thumb") {
    out = Arch::Thumb;
  } else if (name == "x86" || name == "i386") {
    out = Arch::X86;
  } else if (name == "x86_64" || name == "amd64" || name == "x64") {
    out = Arch::X86_64;
  } else if (name == "riscv32") {
    out = Arch::RiscV32;
  } else if (name == "riscv64") {
    out = Arch::RiscV64;
  } else if (name == "mips32" || name == "mips") {
    out = Arch::Mips32;
  } else if (name == "mips64") {
    out = Arch::Mips64;
  } else if (name == "ppc64" || name == "powerpc64") {
    out = Arch::PowerPc64;
  } else if (name == "loongarch64") {
    out = Arch::LoongArch64;
  } else {
    return false;
  }
  return true;
}

unsigned pointerBits(Arch arch) noexcept {
  switch (arch) {
    case Arch::AArch64:
    case Arch::X86_64:
    case Arch::RiscV64:
    case Arch::Mips64:
    case Arch::PowerPc64:
    case Arch::LoongArch64:
      return 64;
    case Arch::Arm32:
    case Arch::Thumb:
    case Arch::X86:
    case Arch::RiscV32:
    case Arch::Mips32:
      return 32;
    case Arch::Unknown:
      return 0;
  }
  return 0;
}

unsigned fixedInstructionSize(Arch arch) noexcept {
  switch (arch) {
    case Arch::AArch64:
    case Arch::Arm32:
    case Arch::Mips32:
    case Arch::Mips64:
    case Arch::PowerPc64:
    case Arch::LoongArch64:
      return 4;
    // Thumb mixes 16- and 32-bit encodings, RISC-V mixes 16 and 32 with the C
    // extension, and x86 is fully variable: all report 0.
    case Arch::Thumb:
    case Arch::RiscV32:
    case Arch::RiscV64:
    case Arch::X86:
    case Arch::X86_64:
    case Arch::Unknown:
      return 0;
  }
  return 0;
}

}  // namespace xdec
