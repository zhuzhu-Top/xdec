// Generic effect & addressing-pattern summary over IL block regions.
//
// Extracts memory slot reads/writes (decomposed as `base + index*scale + offset`),
// register writes, calls, conditional branches, and unwrapped root expressions
// from an arbitrary slice of IL blocks (for example, a single `switch` case
// inside an interpreter or a dispatch-region handler). Completely target- and
// protector-agnostic: carries no assumptions about any specific VM layout.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "xdec/il/function.h"

namespace xdec::analysis {

struct IndexedSlotRef {
  il::ExprId rawAddress{};
  il::ExprId base{};
  il::ExprId index{};  // invalid when fixed displacement only
  int64_t scale = 1;
  int64_t offset = 0;

  [[nodiscard]] bool valid() const noexcept { return rawAddress.valid() && base.valid(); }
  [[nodiscard]] bool isIndexed() const noexcept { return index.valid(); }
};

struct ExprRootSummary {
  il::ExprId rawExpr{};
  il::ExprId coreExpr{};  // after unwrapping top-level ZExt/SExt/Trunc/Bitcast
  il::ExprOp rootOp = il::ExprOp::Undef;
  uint32_t resultBits = 0;
  bool signExtended = false;
  bool zeroExtended = false;
  il::ExprId lhs{};
  il::ExprId rhs{};
  il::ExprId cond{};

  // Populated when `coreExpr` is a `Value` defined by a `Load` op.
  bool isMemoryLoad = false;
  il::OpId loadOp{};
  IndexedSlotRef loadSlot;
  uint32_t loadBits = 0;
};

struct SlotWriteEffect {
  il::OpId op{};
  uint64_t va = 0;
  uint32_t widthBits = 0;
  IndexedSlotRef target;
  ExprRootSummary value;
};

struct SlotReadEffect {
  il::OpId op{};
  uint64_t va = 0;
  uint32_t widthBits = 0;
  IndexedSlotRef source;
};

struct RegWriteEffect {
  il::OpId op{};
  uint64_t va = 0;
  il::RegId reg{};
  ExprRootSummary value;
};

struct CallEffect {
  il::OpId op{};
  uint64_t va = 0;
  il::ExprId target{};
  std::optional<uint64_t> constantTarget;
};

struct CondBranchEffect {
  il::OpId op{};
  uint64_t va = 0;
  il::ExprId condition{};
  il::ExprOp condOp = il::ExprOp::Undef;
  il::BlockId trueTarget{};
  il::BlockId falseTarget{};
};

struct CaseEffectSummary {
  std::vector<il::BlockId> blocks;
  std::vector<SlotWriteEffect> stores;
  std::vector<SlotReadEffect> loads;
  std::vector<RegWriteEffect> regWrites;
  std::vector<CallEffect> calls;
  std::vector<CondBranchEffect> condBranches;
  std::vector<il::OpId> returns;
  std::vector<il::OpId> indirectBranches;
};

/// Decomposes an address expression into `base + index * scale + offset`.
[[nodiscard]] IndexedSlotRef decomposeIndexedSlot(const il::Function& function,
                                                  il::ExprId address);

/// Summarizes the unwrapped root shape of `expr`, linking `Value` loads back
/// to their defining `Load` op and decomposed address slot when available.
[[nodiscard]] ExprRootSummary summarizeExprRoot(const il::Function& function,
                                                il::ExprId expr);

/// Summarizes stores, loads, register writes, calls, conditional branches, and
/// exits across `blocks`.
[[nodiscard]] CaseEffectSummary summarizeCaseEffects(
    const il::Function& function, std::span<const il::BlockId> blocks);

}  // namespace xdec::analysis
