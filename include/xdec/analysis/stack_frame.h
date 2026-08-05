// Stack-frame address classification and the basic alias oracle.
//
// The enabler is the EntryReg leaf: after SSA construction the stack
// pointer's versions are constant displacements from `entry(sp)` — through
// the prologue's subtracts, through `mov x29, sp` (the frame pointer's
// version is the same hash-consed expression), through everything except a
// phi. So an address either bottoms out at the entry stack pointer plus a
// constant, at a plain constant, or somewhere the analysis cannot see, and
// those three answers are what every memory reasoning pass gets to stand on.
//
// The classification is deliberately an analysis, not a pass: consumers ask
// questions, nobody rewrites. Rewriting is stack-prop's job, and it asks the
// same questions through this interface so the answers cannot drift.
#pragma once

#include <cstdint>
#include <optional>

#include "xdec/il/expr.h"
#include "xdec/il/function.h"
#include "xdec/il/op.h"

namespace xdec::analysis {

/// What an address expression denotes, as far as this analysis can tell.
enum class AddressKind : uint8_t {
  /// `entry(sp) + delta`: storage inside the current frame.
  StackSlot,
  /// A plain constant address: the image, the heap by absolute reference.
  Global,
  /// Everything else: pointer arithmetic, loaded pointers, phis.
  Other,
};

struct AddressInfo {
  AddressKind kind = AddressKind::Other;
  /// StackSlot: signed displacement from the entry stack pointer.
  int64_t delta = 0;
  /// Global: the address itself.
  uint64_t address = 0;
};

/// Three-valued alias answer. `Must` additionally implies the accesses cover
/// the same bytes, which is what makes a store-to-load forward exact.
enum class AliasResult : uint8_t { Must, May, No };

class StackFrame {
 public:
  /// Finds the stack-pointer root in the register file. Absent one (an
  /// exotic target), every classification is Other and every alias is May:
  /// honest degradation, never a guess.
  [[nodiscard]] static StackFrame compute(const il::Function& function);

  [[nodiscard]] AddressInfo classify(il::ExprId address) const;

  /// Ranges overlap answer for two accesses of `sizeA`/`sizeB` bytes. Frame
  /// and image never meet: a stack slot and a global cannot alias (the frame
  /// lives above the mapped image on every target we support).
  [[nodiscard]] AliasResult mayAlias(il::ExprId addressA, unsigned sizeA,
                                     il::ExprId addressB, unsigned sizeB) const;

  /// Observed frame extent over every classified memory op, [low, high) in
  /// entry-sp coordinates. Empty when nothing classified.
  [[nodiscard]] int64_t frameLow() const noexcept { return frameLow_; }
  [[nodiscard]] int64_t frameHigh() const noexcept { return frameHigh_; }

  [[nodiscard]] il::RegId stackPointer() const noexcept { return spRoot_; }

 private:
  explicit StackFrame(const il::Function& function, il::RegId spRoot)
      : function_(&function), spRoot_(spRoot) {}

  /// The displacement from `entry(sp)` an expression denotes, when it is a
  /// constant-offset chain over the stack pointer's entry leaf. Nothing
  /// crosses a phi, a load, or a non-stack entry register.
  [[nodiscard]] std::optional<int64_t> frameDelta(il::ExprId address,
                                                  unsigned depth = 0) const;

  /// The function the analysis walks. Like every analysis object here, the
  /// frame must not outlive the function it was computed from.
  const il::Function* function_;
  il::RegId spRoot_;
  int64_t frameLow_ = 0;
  int64_t frameHigh_ = 0;

  static constexpr unsigned kMaxDepth = 64;
};

}  // namespace xdec::analysis
