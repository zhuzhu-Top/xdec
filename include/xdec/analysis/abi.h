// Target/Virtual ABI specification: pure data description of calling
// conventions and special registers so SSA construction, variable recovery,
// and arity trimming never hardcode AArch64 register names.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xdec/il/op.h"
#include "xdec/il/register_file.h"
#include "xdec/support/target.h"

namespace xdec::il {
class Function;
}

namespace xdec::analysis {

struct AbiSpec {
  std::string name;
  il::RegId stackPointer{};
  il::RegId framePointer{};
  il::RegId linkRegister{};
  std::vector<il::RegId> intArgRegs;
  std::vector<il::RegId> intRetRegs;
  std::vector<il::RegId> calleeSavedRegs;
  std::vector<il::RegId> callerSavedRegs;
  int64_t stackGrowthDir = -1;
  unsigned pointerBits = 64;

  /// Returns 0-based index of `root` in `intArgRegs`, or -1 if absent.
  [[nodiscard]] int argIndex(il::RegId root) const noexcept;

  /// True when `root` is explicitly listed in `calleeSavedRegs`, or equals
  /// `stackPointer`.
  [[nodiscard]] bool isCalleeSaved(il::RegId root) const noexcept;

  /// True when `root` is clobbered across calls (listed in `callerSavedRegs`,
  /// or absent from `calleeSavedRegs` and not `stackPointer`).
  [[nodiscard]] bool isCallerSaved(il::RegId root) const noexcept;

  /// Constructs the standard AAPCS64 ABI against `registers`.
  [[nodiscard]] static AbiSpec aarch64(const il::RegisterFile& registers);

  /// Default ABI for `function` when no explicit `AbiSpec` is supplied.
  [[nodiscard]] static AbiSpec defaultFor(const il::Function& function);
};

}  // namespace xdec::analysis
