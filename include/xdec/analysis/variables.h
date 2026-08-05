// Variable and type recovery: the bridge from SSA values to named C locals.
//
// Three variable kinds are recovered:
//   * Arguments — the EntryReg leaves of the calling-convention registers
//     (x0..x7 on AArch64), already distinct per register in the IL.
//   * Locals — stack slots, enumerated by classifying every memory access
//     through analysis::StackFrame; one variable per slot delta.
//   * Temps — phi values, which outlive their block and therefore cannot be
//     inlined by the emitter; one variable per phi, in block order.
//
// Types stay deliberately minimal: machine-width integers plus one level of
// pointer, with signedness inferred from signed operations and defaulting by
// width. There is no struct recovery, no float tracking, no inter-procedural
// propagation — the honest small lattice is documented at each step.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xdec/analysis/stack_frame.h"
#include "xdec/il/function.h"

namespace xdec::analysis {

enum class VarKind : uint8_t { Argument, Local, Temp };

/// Minimal C type: an integer of `width` bits, `pointerDepth` levels of
/// pointer above it. `pointeeWidth` records the widest observed access
/// through the pointer (0: pointee unknown, print as void). Signedness
/// starts as a width default and is promoted by observed signed operations.
struct CType {
  uint32_t width = 64;
  uint32_t pointerDepth = 0;
  uint32_t pointeeWidth = 0;
  bool isSigned = false;

  [[nodiscard]] std::string format() const;
};

struct Variable {
  VarKind kind = VarKind::Temp;
  std::string name;
  CType type;
  /// Argument: the argument-register root this leaf belongs to.
  il::RegId argRoot{};
  /// Local: displacement from the entry stack pointer.
  int64_t stackDelta = 0;
  /// Temp: the phi value backing this variable.
  il::ValueId value{};
};

class VariableTable {
 public:
  /// Recovers the three variable kinds from a function at Ssa maturity or
  /// beyond (EntryReg leaves and stack-slot shapes must already exist).
  [[nodiscard]] static VariableTable recover(const il::Function& function,
                                             const StackFrame& frame);

  [[nodiscard]] const Variable* argumentFor(il::RegId root) const;
  [[nodiscard]] const Variable* localAt(int64_t delta) const;
  [[nodiscard]] const Variable* tempFor(il::ValueId value) const;

  [[nodiscard]] std::span<const Variable> arguments() const noexcept { return args_; }
  [[nodiscard]] std::span<const Variable> locals() const noexcept { return locals_; }
  [[nodiscard]] std::span<const Variable> temps() const noexcept { return temps_; }

  /// The type of the value the function returns, or unset when no `ret` carries
  /// one — which is the only evidence there is that a function returns `void`.
  [[nodiscard]] const std::optional<CType>& returnType() const noexcept {
    return returnType_;
  }

 private:
  std::vector<Variable> args_;
  std::vector<Variable> locals_;
  std::vector<Variable> temps_;
  std::optional<CType> returnType_;
  std::map<uint32_t, uint32_t> argByRoot_;
  std::map<int64_t, uint32_t> localByDelta_;
  std::map<uint32_t, uint32_t> tempByValue_;
};

}  // namespace xdec::analysis
