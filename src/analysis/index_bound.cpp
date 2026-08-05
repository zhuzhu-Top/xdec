// boundOnIndex (see the header for what is being proved and why it has to be).
#include "xdec/analysis/index_bound.h"

#include <algorithm>

namespace xdec::analysis {

namespace {

/// The value underneath the width adjustments between a comparison and a table
/// index.
///
/// The guard and the dispatch rarely name the index the same way: the comparison
/// is on the 32-bit state (`w8`) and the address arithmetic on its 64-bit form
/// (`x8`), so one side carries a zero-extension the other does not, and a
/// low-bits mask may stand in for either. None of that changes which value is
/// being talked about — and for the bound to mean anything, it must be the same
/// value. Only *widening* is stripped: a truncation discards bits, so a bound on
/// the wide value says nothing about the narrow one, and `and` is left alone
/// because a mask changes the value rather than restating it.
[[nodiscard]] il::ExprId stripWidening(const il::Function& function, il::ExprId id) {
  while (true) {
    const il::Expr& expr = function.expr(id);
    if (expr.op != il::ExprOp::ZExt || expr.operandCount != 1) {
      return id;
    }
    id = expr.operands[0];
  }
}

/// Whether two expressions denote the same run-time value, as far as this
/// analysis needs to know. Same SSA value, or the same expression node.
[[nodiscard]] bool sameValue(const il::Function& function, il::ExprId a, il::ExprId b) {
  a = stripWidening(function, a);
  b = stripWidening(function, b);
  if (a == b) {
    return true;
  }
  const il::Expr& left = function.expr(a);
  const il::Expr& right = function.expr(b);
  return left.op == il::ExprOp::Value && right.op == il::ExprOp::Value &&
         left.immediate == right.immediate;
}

/// An unsigned comparison of `index` against a constant, as the condition
/// states it. The IL canonicalises comparisons to the two "less" forms, so which
/// side the index is on is the whole of the difference between an upper bound
/// and a lower one.
struct Comparison {
  /// True for `cmp.leu`, false for `cmp.ltu`.
  bool orEqual = false;
  /// True when the condition reads `index < constant`, false for `constant <
  /// index`.
  bool indexOnLeft = false;
  uint64_t constant = 0;
};

[[nodiscard]] std::optional<Comparison> readComparison(const il::Function& function,
                                                       il::ExprId condition,
                                                       il::ExprId index) {
  const il::Expr& expr = function.expr(condition);
  if (expr.op != il::ExprOp::CmpLtU && expr.op != il::ExprOp::CmpLeU) {
    // Signed comparisons are deliberately not read. A signed guard admits
    // negative indices, so it bounds the index from above without bounding the
    // table's length -- `i <=s 0x90` is satisfied by -1, which addresses the
    // bytes in front of the table. Real dispatch guards are unsigned for exactly
    // this reason, and reading a signed one as if it were unsigned would be
    // claiming a length the code does not promise.
    return std::nullopt;
  }
  Comparison out;
  out.orEqual = expr.op == il::ExprOp::CmpLeU;
  if (sameValue(function, expr.operands[0], index) &&
      function.asConstant(expr.operands[1], out.constant)) {
    out.indexOnLeft = true;
    return out;
  }
  if (sameValue(function, expr.operands[1], index) &&
      function.asConstant(expr.operands[0], out.constant)) {
    out.indexOnLeft = false;
    return out;
  }
  return std::nullopt;
}

/// The upper bound a comparison puts on the index, on one of its two edges.
///
/// Only two of the four combinations bound the index from above at all; the
/// other two say it is at least something, which no table length follows from.
[[nodiscard]] std::optional<uint64_t> upperBound(const Comparison& comparison,
                                                 bool onTrueEdge) {
  // `index < c` / `index <= c` hold on the true edge; on the false edge they
  // become lower bounds.
  if (comparison.indexOnLeft) {
    if (!onTrueEdge) {
      return std::nullopt;
    }
    if (comparison.orEqual) {
      return comparison.constant;
    }
    return comparison.constant == 0 ? std::nullopt
                                    : std::optional<uint64_t>{comparison.constant - 1};
  }
  // `c < index` / `c <= index` are lower bounds where they hold, and upper
  // bounds where they do not: this is the `b.hi default` form, whose *fall
  // through* is the dispatch.
  if (onTrueEdge) {
    return std::nullopt;
  }
  if (comparison.orEqual) {
    return comparison.constant == 0 ? std::nullopt
                                    : std::optional<uint64_t>{comparison.constant - 1};
  }
  return comparison.constant;
}

/// Whether every path to `reached` leaves `from` by the edge into `via` — so
/// that arriving at `reached` means the condition selecting that edge held.
///
/// Block dominance alone does not answer this. The dispatchers here loop: the
/// guard's other edge goes to a default handler that jumps straight back to the
/// guard, which folds into an edge from the guard to itself, and a block trivially
/// dominates what it dominates through either of two edges. So the successor must
/// also be entered from nowhere else — then reaching it is reaching it *from
/// here*, and the condition is a fact about the path.
[[nodiscard]] bool edgeDominates(const il::Function& function,
                                 const Dominators& dominators, il::BlockId from,
                                 il::BlockId via, il::BlockId reached) {
  if (!dominators.dominates(via, reached)) {
    return false;
  }
  const auto predecessors = function.block(via).predecessors;
  return std::all_of(predecessors.begin(), predecessors.end(),
                     [from](il::BlockId predecessor) { return predecessor == from; }) &&
         !predecessors.empty();
}

/// Which of `block`'s two conditional edges is the only way to reach `reached`,
/// or nothing when neither is or both could be.
[[nodiscard]] std::optional<bool> guardedEdge(const il::Function& function,
                                              const Dominators& dominators,
                                              il::BlockId block, il::BlockId reached) {
  const il::Block& source = function.block(block);
  if (source.ops.empty()) {
    return std::nullopt;
  }
  const il::Op& terminator = function.op(source.ops.back());
  if (terminator.code != il::OpCode::CondBranch) {
    return std::nullopt;
  }
  const auto targets = function.targets(terminator);
  if (targets.size() != 2) {
    return std::nullopt;
  }
  const bool viaTrue = edgeDominates(function, dominators, block, targets[0], reached);
  const bool viaFalse = edgeDominates(function, dominators, block, targets[1], reached);
  if (viaTrue == viaFalse) {
    return std::nullopt;  // both, or neither: the branch decides nothing here
  }
  return viaTrue;
}

}  // namespace

std::optional<uint64_t> boundOnIndex(const il::Function& function,
                                     const Dominators& dominators, il::BlockId dispatch,
                                     il::ExprId index) {
  // Up the dominator tree, taking the tightest bound any guard on the way
  // proves. More than one can apply -- a range check is often split in two --
  // and each is independently true of every path here, so the smallest wins.
  std::optional<uint64_t> best;
  for (il::BlockId block = dispatch; block.valid(); block = dominators.idom(block)) {
    const std::optional<bool> onTrueEdge =
        guardedEdge(function, dominators, block, dispatch);
    if (!onTrueEdge.has_value()) {
      continue;
    }
    const il::Op& terminator = function.op(function.block(block).ops.back());
    const auto operands = function.operands(terminator);
    if (operands.empty()) {
      continue;
    }
    const std::optional<Comparison> comparison =
        readComparison(function, operands[0], index);
    if (!comparison.has_value()) {
      continue;
    }
    if (const std::optional<uint64_t> bound = upperBound(*comparison, *onTrueEdge)) {
      best = best.has_value() ? std::min(*best, *bound) : *bound;
    }
  }
  return best;
}

}  // namespace xdec::analysis
