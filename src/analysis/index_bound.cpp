// boundOnIndex (see the header for what is being proved and why it has to be).
#include "xdec/analysis/index_bound.h"

#include <algorithm>
#include <cstdint>

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

/// Every operand of a well-formed IL expression carries an integer width, and
/// that width is itself an upper bound: nothing wider than i1 ever holds more
/// than 1. It is usually a loose bound -- most i32 values are nowhere near
/// 0xffffffff -- but a value that is a zero-extension chain off a genuinely
/// narrow source (a flag, a byte-sized state) makes it a tight one, for free:
/// the compiler already recorded the width, this just reads it back instead
/// of assuming the full 64 bits are live.
[[nodiscard]] uint64_t typeBound(const il::Type& type) noexcept {
  const unsigned bits = type.bits();
  return bits >= 64 ? ~uint64_t{0} : (uint64_t{1} << bits) - 1;
}

std::optional<uint64_t> localBound(const il::Function& function, il::ExprId id, unsigned depth);

/// Whether `expr`'s whole value is provably clear of the sign bit -- every
/// value localBound admits fits in the positive half of the type -- so a
/// signed and an unsigned reading of it agree on every comparison. This is
/// the fact readComparison refuses to assume about an arbitrary expression
/// (see its own comment on why a signed guard is not trusted); here it is
/// not assumed, it is proven from the same structural bound the rest of this
/// file already computes, and only ever used once it is.
[[nodiscard]] std::optional<uint64_t> nonNegativeBound(const il::Function& function,
                                                       il::ExprId expr, unsigned depth) {
  const std::optional<uint64_t> bound = localBound(function, expr, depth);
  if (!bound.has_value() || *bound > uint64_t{INT64_MAX}) {
    return std::nullopt;
  }
  return bound;
}

/// Whether an integer comparison with a constant on one side is decided
/// outright because the other side's whole range -- `[0, bound]`, from
/// nonNegativeBound -- agrees on the answer everywhere in it. This is what
/// lets a select's condition be recognised as always-true or always-false
/// from structure alone: `3 <s x` is never satisfied by an `x` a zero-extend
/// chain has already bounded to 1, whatever sign the comparison reads it
/// with, because every value that chain can produce is at most 1.
[[nodiscard]] std::optional<bool> evaluateAgainstBoundedRange(const il::Function& function,
                                                              il::ExprId condition,
                                                              unsigned depth) {
  const il::Expr& cond = function.expr(condition);
  if (cond.operandCount != 2) {
    return std::nullopt;
  }
  switch (cond.op) {
    case il::ExprOp::CmpLtU:
    case il::ExprOp::CmpLeU:
    case il::ExprOp::CmpLtS:
    case il::ExprOp::CmpLeS:
    case il::ExprOp::CmpEq:
    case il::ExprOp::CmpNe:
      break;
    default:
      return std::nullopt;
  }
  uint64_t constant = 0;
  il::ExprId variable{};
  bool constantOnLeft = false;
  if (function.asConstant(cond.operands[0], constant)) {
    constantOnLeft = true;
    variable = cond.operands[1];
  } else if (function.asConstant(cond.operands[1], constant)) {
    variable = cond.operands[0];
  } else {
    return std::nullopt;
  }
  const std::optional<uint64_t> bound = nonNegativeBound(function, variable, depth + 1);
  if (!bound.has_value()) {
    return std::nullopt;
  }
  const uint64_t lo = 0;
  const uint64_t hi = *bound;
  if (cond.op == il::ExprOp::CmpEq || cond.op == il::ExprOp::CmpNe) {
    // Eq/Ne are not monotone in the variable, so the endpoints alone do not
    // decide it -- but whether the constant is in range at all does, except
    // when the range is a single point, which the endpoint check below
    // handles fine on its own.
    if (constant < lo || constant > hi) {
      return cond.op == il::ExprOp::CmpNe;
    }
    if (lo != hi) {
      return std::nullopt;  // the constant is one of several values in range
    }
  }
  const auto evalAt = [&](uint64_t value) -> bool {
    const uint64_t a = constantOnLeft ? constant : value;
    const uint64_t b = constantOnLeft ? value : constant;
    switch (cond.op) {
      case il::ExprOp::CmpLtU:
      case il::ExprOp::CmpLtS:
        return a < b;
      case il::ExprOp::CmpLeU:
      case il::ExprOp::CmpLeS:
        return a <= b;
      case il::ExprOp::CmpEq:
        return a == b;
      default:  // CmpNe, the only case left once operandCount/op are checked
        return a != b;
    }
  };
  const bool atLo = evalAt(lo);
  const bool atHi = evalAt(hi);
  return atLo == atHi ? std::optional<bool>{atLo} : std::nullopt;
}

/// What a select's own condition proves about one of its two arms -- the
/// branchless twin of what a dominating branch proves about the dispatch:
/// `state > k ? replacement : state` is a clamp compiled with no branch at
/// all for boundOnIndex's dominator walk to climb, and CSEL/CMOV compile
/// every saturating clamp this way. The comparison only ever tightens the
/// answer; where it says nothing about this arm (a signed guard, an operand
/// it does not mention), the arm's own structure -- typically a constant
/// replacement or a narrow computed value -- still answers on its own.
[[nodiscard]] std::optional<uint64_t> armBound(const il::Function& function, il::ExprId cond,
                                               il::ExprId arm, bool onTrueEdge, unsigned depth) {
  // An edge structurally proven never taken contributes nothing: whatever
  // this arm computes, the select can never produce it, so folding its bound
  // into the max would be answering a question about a value nothing ever
  // holds. Zero is the correct contribution, not a guess standing in for one
  // -- an index bound is never negative, so it cannot pull the other arm's
  // bound down, only fail to raise it.
  if (const std::optional<bool> decided = evaluateAgainstBoundedRange(function, cond, depth)) {
    if (*decided != onTrueEdge) {
      return uint64_t{0};
    }
  }
  if (const std::optional<Comparison> comparison = readComparison(function, cond, arm)) {
    if (const std::optional<uint64_t> bound = upperBound(*comparison, onTrueEdge)) {
      return bound;
    }
  }
  return localBound(function, arm, depth + 1);
}

/// The tightest bound provable from `id`'s own structure alone: no branch, no
/// dominance, nothing but the expression tree and the widths its own casts
/// carry. This is what closes the gap boundOnIndex's dominator walk cannot:
/// a branchless clamp, a mask, a shift, or a narrow value nested inside any
/// of those, however deep the obfuscator's arithmetic buries it -- the same
/// handful of local rules apply at every level rather than one hard-coded
/// instruction shape at the top.
///
/// Never wrong when it answers, and always allowed to give up: a caller with
/// a tighter fact from elsewhere (a comparison in scope, say) uses this only
/// as one candidate among several and keeps whichever bound is smallest.
[[nodiscard]] std::optional<uint64_t> localBound(const il::Function& function, il::ExprId id,
                                                 unsigned depth) {
  constexpr unsigned kMaxDepth = 12;
  if (depth > kMaxDepth) {
    return std::nullopt;
  }
  uint64_t constant = 0;
  if (function.asConstant(id, constant)) {
    return constant;
  }
  const il::Expr& expr = function.expr(id);
  switch (expr.op) {
    case il::ExprOp::ZExt: {
      // The value is exactly the source's, just with zero bits in front, so
      // the bound comes from the *source's* width -- a zext to i64 off an i1
      // still only ever holds 0 or 1. Using the result type here instead
      // would answer "at most 0xffffffffffffffff", which is true and useless.
      const uint64_t bound = typeBound(function.expr(expr.operands[0]).type);
      const std::optional<uint64_t> inner = localBound(function, expr.operands[0], depth + 1);
      return inner.has_value() ? std::optional<uint64_t>{std::min(*inner, bound)} : bound;
    }
    case il::ExprOp::Trunc: {
      // The opposite shape: a trunc's *result* type is what bounds it, since
      // truncation keeps only the low bits and can turn any source value into
      // any value that fits there. A tighter bound the source already met
      // (and so survives truncation unchanged) still wins where it applies.
      const uint64_t bound = typeBound(expr.type);
      const std::optional<uint64_t> inner = localBound(function, expr.operands[0], depth + 1);
      return inner.has_value() ? std::optional<uint64_t>{std::min(*inner, bound)} : bound;
    }
    case il::ExprOp::And: {
      // A mask bounds the result to itself regardless of the other operand,
      // and to the other operand's own bound regardless of the mask -- AND
      // only ever clears bits, on either side.
      uint64_t mask = 0;
      il::ExprId other{};
      if (function.asConstant(expr.operands[1], mask)) {
        other = expr.operands[0];
      } else if (function.asConstant(expr.operands[0], mask)) {
        other = expr.operands[1];
      } else {
        return std::nullopt;
      }
      const std::optional<uint64_t> inner = localBound(function, other, depth + 1);
      return inner.has_value() ? std::optional<uint64_t>{std::min(*inner, mask)} : mask;
    }
    case il::ExprOp::ShrU: {
      uint64_t shift = 0;
      if (!function.asConstant(expr.operands[1], shift)) {
        return std::nullopt;
      }
      if (shift >= 64) {
        return uint64_t{0};
      }
      const std::optional<uint64_t> inner = localBound(function, expr.operands[0], depth + 1);
      return (inner.value_or(typeBound(expr.type))) >> shift;
    }
    case il::ExprOp::Select: {
      // Both arms must answer, or neither the select nor anything above it in
      // an index expression can be claimed bounded: whichever arm runs, that
      // is the whole value, so the proof needs every arm covered, not most.
      const std::optional<uint64_t> onTrue =
          armBound(function, expr.operands[0], expr.operands[1], /*onTrueEdge=*/true, depth);
      const std::optional<uint64_t> onFalse =
          armBound(function, expr.operands[0], expr.operands[2], /*onTrueEdge=*/false, depth);
      if (!onTrue.has_value() || !onFalse.has_value()) {
        return std::nullopt;
      }
      return std::max(*onTrue, *onFalse);
    }
    default:
      return std::nullopt;
  }
}

}  // namespace

std::optional<uint64_t> boundOnIndex(const il::Function& function,
                                     const Dominators& dominators, il::BlockId dispatch,
                                     il::ExprId index) {
  // The index's own structure first: a branchless clamp needs no dominator at
  // all, so this applies even where nothing below finds a guard to climb to.
  std::optional<uint64_t> best = localBound(function, index, 0);

  // Then up the dominator tree, taking the tightest bound any guard on the
  // way proves. More than one can apply -- a range check is often split in
  // two, or a branch guard sits above a clamp already bounded above -- and
  // each is independently true of every path here, so the smallest wins.
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
