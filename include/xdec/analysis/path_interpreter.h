// PathInterpreter: executes one block's ops against a PathState.
//
// Mirrors il::Interpreter's one-block-at-a-time contract (see its header for
// why that granularity): this class does not follow control flow either,
// PathExplorer does that. What it models is symbolic rather than concrete,
// and does not need a register file at all -- at Ssa maturity (the level
// resolve-indirect and therefore this runs at) a register's value already
// *is* the expression graph PathState::eval walks (EntryReg leaves and phi
// chains), so nothing here reads or writes a register bank. Only the ops
// that define an SSA value or touch memory need this class's attention:
// Phi (path-sensitive resolution -- see PathState::bindPhi), Load and Store
// (see PathState::bindLoad/recordStore). Everything else this evaluator does
// not model (a bare ReadReg/WriteReg SSA construction did not promote, an
// Intrinsic result, a Call's return value) is left at its default of top,
// the same degradation ImageEval gives an op it does not recognise.
#pragma once

#include <cstdint>
#include <span>

#include "xdec/analysis/path_state.h"
#include "xdec/il/function.h"

namespace xdec::analysis {

enum class PathStop : uint8_t {
  /// Unconditional edge to `target`.
  Branch,
  /// `condition` selects `ifTrue`/`ifFalse`.
  CondBranch,
  /// A computed edge whose target this walk should evaluate for itself
  /// (`indirectTarget`), unless `resolvedTargets` is already non-empty --
  /// a branch an earlier pass this same run already resolved, which this
  /// walk simply follows like any other multi-way edge.
  IndirectBranch,
  Return,
  Unreachable,
  /// Anything this evaluator declines to walk past: an opaque terminator,
  /// or a block with none (should not occur in verified IL, handled rather
  /// than assumed away). The path just ends here.
  Stopped,
};

struct PathOutcome {
  PathStop stop = PathStop::Stopped;
  uint64_t va = 0;
  /// The terminator op itself, so a caller can key auxiliary per-branch state
  /// (PathExplorer's canary-arm map) off something stabler than an address.
  il::OpId op;

  il::ExprId condition{};
  il::BlockId ifTrue;
  il::BlockId ifFalse;

  il::BlockId target;

  il::ExprId indirectTarget{};
  std::span<const il::BlockId> resolvedTargets;
};

/// Executes `block`'s ops against `state` in program order, updating its
/// bindings, and reports how the block ends. `cameFrom` is the block this
/// walk arrived from (invalid for the entry), needed to resolve a Phi to the
/// operand its own predecessor list associates with that edge -- see
/// PathState::bindPhi.
class PathInterpreter {
 public:
  explicit PathInterpreter(const il::Function& function) noexcept : function_(&function) {}

  [[nodiscard]] PathOutcome stepBlock(il::BlockId block, il::BlockId cameFrom,
                                      PathState& state) const;

 private:
  const il::Function* function_;
};

}  // namespace xdec::analysis
