// boundOnIndex (see the header for what is being proved and why it has to be).
#include "xdec/analysis/index_bound.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "xdec/support/bits.h"

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
  // The same comparison, read through a constant offset the arm adds to what
  // was compared. This is what `cinc` leaves behind: `state + 1` selected
  // against `state` is one instruction, so the value the guard names and the
  // value this arm computes differ by a literal, and readComparison -- which
  // insists on the *same* value, for good reason -- cannot see past it. The
  // offset is exact, so adding it back to the guard's bound is too.
  if (const il::Expr& shifted = function.expr(arm);
      shifted.op == il::ExprOp::Add || shifted.op == il::ExprOp::Sub) {
    uint64_t literal = 0;
    if (function.asConstant(shifted.operands[1], literal)) {
      if (const std::optional<Comparison> comparison =
              readComparison(function, cond, shifted.operands[0])) {
        if (const std::optional<uint64_t> bound = upperBound(*comparison, onTrueEdge)) {
          const uint64_t ceiling = typeBound(shifted.type);
          if (shifted.op == il::ExprOp::Sub) {
            return *bound < literal ? uint64_t{0} : *bound - literal;
          }
          if (literal <= ceiling && *bound <= ceiling - literal) {
            return *bound + literal;
          }
        }
      }
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
    case il::ExprOp::Or: {
      // OR only ever sets bits, so bounding it needs at least one side known
      // exactly -- a literal, the same requirement And's own mask has, just
      // pointed the other way (And's mask bounds the result regardless of
      // the other side; here the literal's bits are simply always present,
      // and it is the *other* side's bits that must be bounded to know which
      // more can join them). "Bounded to at most N" does not mean "share N's
      // bit pattern": an operand `localBound` proves is at most 1 can still
      // be exactly 1, so bit 0 is live and the result must count it, not just
      // the numeral 1. `significantBits`/`lowMask` (support/bits.h) turn that
      // magnitude bound into the actual set of bit positions it can touch --
      // one less than the next power of two above it -- which the literal's
      // own exact bits are then ORed onto.
      uint64_t literal = 0;
      il::ExprId other{};
      if (function.asConstant(expr.operands[1], literal)) {
        other = expr.operands[0];
      } else if (function.asConstant(expr.operands[0], literal)) {
        other = expr.operands[1];
      } else {
        return std::nullopt;
      }
      const std::optional<uint64_t> inner = localBound(function, other, depth + 1);
      const uint64_t otherBound = inner.value_or(typeBound(expr.type));
      return lowMask(significantBits(otherBound)) | literal;
    }
    case il::ExprOp::Add:
    case il::ExprOp::Sub: {
      // A constant offset shifts a bound by exactly itself -- but only while
      // the shifted bound still fits the type. Past that the arithmetic wraps,
      // and a wrapped value is small again, so the shifted bound is not a
      // bound at all: `x + 1` on an `x` bounded to 0xffffffff is 0 as often as
      // it is 0x100000000. There is a true answer in that case (the type's own
      // width) and it is worth nothing to a caller -- resolve-indirect reads a
      // proven bound as "every index below this is a real entry" -- so this
      // gives up instead, on the same "nothing rather than useless" footing as
      // the rest of the file.
      uint64_t literal = 0;
      const bool rightIsConstant = function.asConstant(expr.operands[1], literal);
      const bool leftIsConstant =
          !rightIsConstant && function.asConstant(expr.operands[0], literal);
      if (!rightIsConstant && !leftIsConstant) {
        return std::nullopt;
      }
      const std::optional<uint64_t> inner = localBound(
          function, rightIsConstant ? expr.operands[0] : expr.operands[1], depth + 1);
      if (!inner.has_value()) {
        return std::nullopt;
      }
      const uint64_t ceiling = typeBound(expr.type);
      if (literal > ceiling) {
        return std::nullopt;
      }
      if (expr.op == il::ExprOp::Add) {
        return *inner > ceiling - literal ? std::nullopt
                                          : std::optional<uint64_t>{*inner + literal};
      }
      if (leftIsConstant) {
        // `k - x`, which is at most k, and only while x cannot take it past
        // zero into a wrap.
        return *inner > literal ? std::nullopt : std::optional<uint64_t>{literal};
      }
      return *inner < literal ? std::nullopt : std::optional<uint64_t>{*inner - literal};
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

/// The most values the walk below carries before conceding. Past this an
/// "enumerable" index is not one any caller here wants enumerated, and the
/// honest answer is that nothing was proved. Larger than `ValueSet::kCap`
/// (analysis/image_eval.h, 16): that cap is sized for a single site's direct
/// fan-out, but a chain of dispatchers each folding one more independent
/// complementary-bit term (BitFactor, above) into the next site's index
/// multiplies the *intermediate* set at every step even when the final
/// answer stays small -- four independent bits are only 16 raw combinations
/// before the arithmetic's own coincidental collisions thin them, and a
/// fifth already needs headroom past that to be counted rather than given up
/// on. This is still bounded, and still small next to a real table's
/// hundreds of entries -- it only has to outlast a handful of accumulated
/// booleans, not become one.
constexpr std::size_t kSetCap = 64;

void insertUnique(std::vector<uint64_t>& values, uint64_t value) {
  if (std::find(values.begin(), values.end(), value) == values.end()) {
    values.push_back(value);
  }
}

/// Every value a *narrow result* can take, whatever it was computed from:
/// `0 .. lowMask(width)`, enumerated when that is few enough to be worth
/// having. This is the rule the whole walk turns on -- an operation whose
/// result type (or remaining bits, after a shift) is a handful of bits wide
/// has a handful of possible results, and no knowledge of its input is needed
/// to say which.
[[nodiscard]] std::optional<std::vector<uint64_t>> valuesOfWidth(unsigned width) {
  const uint64_t ceiling = lowMask(width);
  if (ceiling >= kSetCap) {
    return std::nullopt;
  }
  std::vector<uint64_t> out;
  for (uint64_t value = 0; value <= ceiling; ++value) {
    out.push_back(value);
  }
  return out;
}

/// One side of a binary op reduced to `multiplier * bit`, where `bit` is a
/// single boolean this analysis cannot see through the source of, but can
/// still tell is a boolean -- a sign bit, `shr.u(x, k)`, or a condition,
/// `cmp.eq(a, b)` -- or either one's logical complement.
///
/// Two sides that each match this over the *same* underlying question are not
/// two independent unknowns: `shr.u(x,k) + shr.u(not(x),k)` is 1 on every
/// path, and so is `(a == b) + (a != b)` -- neither is a coin flipped twice.
/// Resolving them independently -- as the generic cross product below does
/// for two subtrees that really are unrelated -- invents the combinations
/// where both bits agree, which do not occur, alongside the ones where they
/// legitimately disagree; a caller that could have known there is only one
/// bit here should not have to also rule out the impossible ones down the
/// line. Matched, there is exactly one bit and exactly two combinations,
/// which is what the caller's cross product over `{0, 1}` computes.
struct BitFactor {
  enum class Kind : uint8_t { SignBit, Comparison };
  Kind kind = Kind::SignBit;
  // SignBit: `key` is the shifted source, `shift` the shift amount, `key2`
  // unused. Comparison: `key`/`key2` are the two operands compared, `shift`
  // unused.
  il::ExprId key{};
  il::ExprId key2{};
  uint64_t shift = 0;
  bool complemented = false;
  uint64_t multiplier = 1;

  [[nodiscard]] bool sameQuestionAs(const BitFactor& other) const {
    return kind == other.kind && key == other.key && key2 == other.key2 && shift == other.shift;
  }
};

[[nodiscard]] std::optional<BitFactor> matchBitFactor(const il::Function& function, il::ExprId id) {
  const il::Expr& expr = function.expr(id);
  if (expr.op == il::ExprOp::Mul || expr.op == il::ExprOp::Shl) {
    uint64_t literal = 0;
    il::ExprId inner{};
    if (function.asConstant(expr.operands[1], literal)) {
      inner = expr.operands[0];
    } else if (expr.op == il::ExprOp::Mul && function.asConstant(expr.operands[0], literal)) {
      inner = expr.operands[1];
    } else {
      return std::nullopt;
    }
    if (expr.op == il::ExprOp::Shl) {
      if (literal >= 64) {
        return std::nullopt;
      }
      literal = uint64_t{1} << literal;
    }
    std::optional<BitFactor> factor = matchBitFactor(function, inner);
    if (!factor.has_value()) {
      return std::nullopt;
    }
    factor->multiplier *= literal;
    return factor;
  }
  if (expr.op == il::ExprOp::ShrU) {
    uint64_t shift = 0;
    if (!function.asConstant(expr.operands[1], shift)) {
      return std::nullopt;
    }
    const il::Expr& operand = function.expr(expr.operands[0]);
    if (operand.op == il::ExprOp::Not && operand.operandCount == 1) {
      return BitFactor{BitFactor::Kind::SignBit, operand.operands[0], il::ExprId{}, shift,
                       /*complemented=*/true, 1};
    }
    return BitFactor{BitFactor::Kind::SignBit, expr.operands[0], il::ExprId{}, shift,
                     /*complemented=*/false, 1};
  }
  // A widened boolean is the boolean, 0 or 1 either way: the width change
  // that matters is already the outer combineBinary's job.
  if ((expr.op == il::ExprOp::ZExt || expr.op == il::ExprOp::SExt) && expr.operandCount == 1 &&
      function.expr(expr.operands[0]).type.bits() == 1) {
    return matchBitFactor(function, expr.operands[0]);
  }
  // The boolean itself: a comparison, seen through however many `not`s wrap
  // it. Each `not` flips the bit this factor stands for -- flip the polarity
  // it is recorded with, rather than the value, so two factors compare equal
  // by their question (which two things are compared) independent of how
  // many times either side happened to be negated on the way here.
  {
    il::ExprId cursor = id;
    bool negated = false;
    while (true) {
      const il::Expr& node = function.expr(cursor);
      if (node.op != il::ExprOp::Not || node.operandCount != 1) {
        break;
      }
      negated = !negated;
      cursor = node.operands[0];
    }
    const il::Expr& cmp = function.expr(cursor);
    if ((cmp.op == il::ExprOp::CmpEq || cmp.op == il::ExprOp::CmpNe) && cmp.operandCount == 2) {
      // CmpNe is CmpEq's own complement, so folding it into the polarity
      // alongside any `not`s makes every comparison of the same two operands
      // -- eq, ne, or either negated -- key identically and differ only in
      // `complemented`.
      const bool complemented = (cmp.op == il::ExprOp::CmpNe) != negated;
      return BitFactor{BitFactor::Kind::Comparison, cmp.operands[0], cmp.operands[1], 0,
                       complemented, 1};
    }
  }
  return std::nullopt;
}

/// `lhs op rhs` for the binary ops `exactValues` combines known values with,
/// shared between the bit-factor path and the generic cross product so the
/// two agree on what the op means.
[[nodiscard]] uint64_t combineBinary(il::ExprOp op, uint64_t lhs, uint64_t rhs, unsigned width) {
  uint64_t combined = 0;
  switch (op) {
    case il::ExprOp::Or:
      combined = lhs | rhs;
      break;
    case il::ExprOp::And:
      combined = lhs & rhs;
      break;
    case il::ExprOp::Xor:
      combined = lhs ^ rhs;
      break;
    case il::ExprOp::Add:
      combined = lhs + rhs;
      break;
    case il::ExprOp::Sub:
      combined = lhs - rhs;
      break;
    case il::ExprOp::Shl:
      combined = rhs >= width ? uint64_t{0} : (lhs << rhs);
      break;
    default:  // Mul, the only case left
      combined = lhs * rhs;
      break;
  }
  return zeroExtend(combined, width);
}

std::optional<std::vector<uint64_t>> exactValues(const il::Function& function, il::ExprId id,
                                                 unsigned depth);

/// Every leaf of a chain of the same binary op, found by unfolding nesting on
/// either side: `a op (b op c)` flattens the same as `(a op b) op c` would.
/// The obfuscator's own tree only ever associates one way, and absd's OR
/// dispatchers chain three or more complementary-flag terms this way (see
/// exactOrChainValues) -- matchBitFactor's direct-sibling check below cannot
/// see a pair split across that nesting, since a nested `Or` node is not
/// itself a bit factor.
void flattenChain(const il::Function& function, il::ExprOp op, il::ExprId id,
                  std::vector<il::ExprId>& leaves) {
  const il::Expr& expr = function.expr(id);
  if (expr.op == op && expr.operandCount == 2) {
    flattenChain(function, op, expr.operands[0], leaves);
    flattenChain(function, op, expr.operands[1], leaves);
    return;
  }
  leaves.push_back(id);
}

/// An OR chain of three or more terms, at least one complementary pair among
/// them (see BitFactor and its direct-sibling use just below in `exactValues`
/// for the two-term case this generalises). Each complementary pair
/// contributes its combined outcomes as one group -- for a true complement
/// that is the singleton `{1}`, not the `{0, 1}` treating the two flags as
/// independent would produce -- and every unpaired leaf contributes its own
/// value set; the groups are then folded together with the same cap the rest
/// of this file uses. Absd's own shape is `t26 | t27 | ... | !(t_n != t_m)`:
/// none of those terms sit as direct siblings of every other one, so without
/// flattening the chain first, the two-term check a few lines down never
/// gets the chance to fire at all.
[[nodiscard]] std::optional<std::vector<uint64_t>> exactOrChainValues(const il::Function& function,
                                                                      il::ExprId id,
                                                                      unsigned width,
                                                                      unsigned depth) {
  std::vector<il::ExprId> leaves;
  flattenChain(function, il::ExprOp::Or, id, leaves);
  if (leaves.size() <= 2) {
    // Nothing this walk can do that the direct two-term check below cannot;
    // let that path run instead of duplicating it.
    return std::nullopt;
  }
  std::vector<bool> claimed(leaves.size(), false);
  std::vector<std::vector<uint64_t>> groups;
  bool anyPair = false;
  for (std::size_t i = 0; i < leaves.size(); ++i) {
    if (claimed[i]) {
      continue;
    }
    const std::optional<BitFactor> left = matchBitFactor(function, leaves[i]);
    bool paired = false;
    if (left.has_value()) {
      for (std::size_t j = i + 1; j < leaves.size(); ++j) {
        if (claimed[j]) {
          continue;
        }
        const std::optional<BitFactor> right = matchBitFactor(function, leaves[j]);
        if (right.has_value() && left->sameQuestionAs(*right) &&
            left->complemented != right->complemented) {
          std::vector<uint64_t> combo;
          for (uint64_t bit = 0; bit <= 1; ++bit) {
            const uint64_t leftBit = left->complemented ? 1 - bit : bit;
            const uint64_t rightBit = right->complemented ? 1 - bit : bit;
            insertUnique(combo, combineBinary(il::ExprOp::Or, leftBit * left->multiplier,
                                              rightBit * right->multiplier, width));
          }
          groups.push_back(std::move(combo));
          claimed[i] = claimed[j] = true;
          paired = true;
          anyPair = true;
          break;
        }
      }
    }
    if (!paired) {
      const std::optional<std::vector<uint64_t>> values =
          exactValues(function, leaves[i], depth + 1);
      if (!values.has_value()) {
        return std::nullopt;
      }
      groups.push_back(*values);
      claimed[i] = true;
    }
  }
  if (!anyPair) {
    // Every leaf resolved independently: the same answer the generic
    // two-at-a-time recursion below already reaches, just computed once
    // more. Defer to it rather than duplicate the work.
    return std::nullopt;
  }
  std::vector<uint64_t> out{0};  // OR's identity
  for (const std::vector<uint64_t>& group : groups) {
    if (out.size() * group.size() > kSetCap) {
      return std::nullopt;
    }
    std::vector<uint64_t> next;
    for (const uint64_t a : out) {
      for (const uint64_t b : group) {
        insertUnique(next, combineBinary(il::ExprOp::Or, a, b, width));
      }
    }
    out = std::move(next);
  }
  return out.size() > kSetCap ? std::nullopt : std::optional<std::vector<uint64_t>>{out};
}

/// See `preciseIndexSet` in the header for the contract; this is the walk.
[[nodiscard]] std::optional<std::vector<uint64_t>> exactValues(const il::Function& function,
                                                               il::ExprId id, unsigned depth) {
  constexpr unsigned kMaxDepth = 12;
  if (depth > kMaxDepth) {
    return std::nullopt;
  }
  uint64_t constant = 0;
  if (function.asConstant(id, constant)) {
    return std::vector<uint64_t>{constant};
  }
  const il::Expr& expr = function.expr(id);
  const unsigned width = expr.type.bits();
  switch (expr.op) {
    case il::ExprOp::ZExt:
    case il::ExprOp::SExt:
    case il::ExprOp::Trunc: {
      const unsigned fromWidth = function.expr(expr.operands[0]).type.bits();
      const std::optional<std::vector<uint64_t>> inner =
          exactValues(function, expr.operands[0], depth + 1);
      if (!inner.has_value()) {
        // A cast off an unknown source still answers when it is a truncation
        // to a narrow type: those bits are all that survives, so the result
        // is one of very few values. Widening an unknown source cannot say
        // the same -- it preserves every value the source had.
        return expr.op == il::ExprOp::Trunc ? valuesOfWidth(width) : std::nullopt;
      }
      std::vector<uint64_t> out;
      for (const uint64_t value : *inner) {
        insertUnique(out, expr.op == il::ExprOp::SExt ? signExtendTo(value, fromWidth, width)
                                                     : zeroExtend(value, width));
      }
      return out;
    }
    case il::ExprOp::Value: {
      // A value backed by a phi is the same merge ImageEval's evalValue reads
      // (analysis/image_eval.cpp) -- unioning its arms here, not just leaving
      // it opaque, is what lets a loop-carried dispatcher state (absd's
      // reg:x9/x11 phis) answer through a `val:iN(%k)` the same way it would
      // if the tree had been built with the arms spliced in directly. A raw
      // EntryReg arm next to a real one is dropped rather than unioned, same
      // policy and same reason as ImageEval::unionEntryRegAware: it is a
      // platform fact this walk cannot look up (no EntryRegFacts reaches
      // here), and folding it in would widen an otherwise small merge with a
      // value that only ever flows in on the one edge that does not matter.
      const il::ValueId valueId{static_cast<uint32_t>(expr.immediate)};
      if (!function.hasValue(valueId)) {
        return std::nullopt;
      }
      const il::ValueInfo& info = function.value(valueId);
      if (!function.hasOp(info.definition) ||
          function.op(info.definition).code != il::OpCode::Phi) {
        // Loads, calls, register reads: no shape to read past, same fallback
        // as any other op this walk cannot see through.
        return valuesOfWidth(width);
      }
      const std::span<const il::ExprId> arms = function.operands(function.op(info.definition));
      bool anyComputed = false;
      for (const il::ExprId arm : arms) {
        if (function.expr(arm).op != il::ExprOp::EntryReg) {
          anyComputed = true;
          break;
        }
      }
      std::vector<uint64_t> out;
      for (const il::ExprId arm : arms) {
        if (anyComputed && function.expr(arm).op == il::ExprOp::EntryReg) {
          continue;
        }
        const std::optional<std::vector<uint64_t>> armValues =
            exactValues(function, arm, depth + 1);
        if (!armValues.has_value()) {
          return std::nullopt;
        }
        for (const uint64_t value : *armValues) {
          insertUnique(out, value);
        }
        if (out.size() > kSetCap) {
          return std::nullopt;
        }
      }
      return out;
    }
    case il::ExprOp::ShrU: {
      uint64_t shift = 0;
      if (!function.asConstant(expr.operands[1], shift)) {
        return std::nullopt;
      }
      if (shift >= width) {
        return std::vector<uint64_t>{0};
      }
      if (const std::optional<std::vector<uint64_t>> inner =
              exactValues(function, expr.operands[0], depth + 1)) {
        std::vector<uint64_t> out;
        for (const uint64_t value : *inner) {
          insertUnique(out, zeroExtend(value, width) >> shift);
        }
        return out;
      }
      // The shape this walk was written for. A shift right by 31 out of 32
      // leaves one bit standing, so the result is 0 or 1 for every input
      // there is -- which is exactly the fact an evaluator that needs the
      // input first can never reach.
      return valuesOfWidth(width - static_cast<unsigned>(shift));
    }
    case il::ExprOp::Or:
    case il::ExprOp::And:
    case il::ExprOp::Xor:
    case il::ExprOp::Add:
    case il::ExprOp::Sub:
    case il::ExprOp::Mul:
    case il::ExprOp::Shl: {
      // A chain of three or more OR terms, a complementary pair among them
      // separated by nesting rather than sitting as direct siblings: see
      // exactOrChainValues. Tried first because the direct two-term check
      // just below never sees past that nesting on its own.
      if (expr.op == il::ExprOp::Or) {
        if (auto chained = exactOrChainValues(function, id, width, depth)) {
          return chained;
        }
      }
      // A single shared bit read on both sides (see BitFactor above) is
      // resolved on its own two values before anything else is tried: the
      // generic path below would treat the two sides as independent and
      // manufacture two combinations that cannot occur.
      if (const std::optional<BitFactor> left = matchBitFactor(function, expr.operands[0])) {
        if (const std::optional<BitFactor> right = matchBitFactor(function, expr.operands[1])) {
          if (left->sameQuestionAs(*right) && left->complemented != right->complemented) {
            std::vector<uint64_t> out;
            for (uint64_t bit = 0; bit <= 1; ++bit) {
              const uint64_t leftBit = left->complemented ? 1 - bit : bit;
              const uint64_t rightBit = right->complemented ? 1 - bit : bit;
              insertUnique(out, combineBinary(expr.op, leftBit * left->multiplier,
                                              rightBit * right->multiplier, width));
            }
            return out;
          }
        }
      }
      // Otherwise, both sides independently: a literal is a set of one (the
      // recursive call resolves it that way immediately, see the constant
      // check at the top of this function), so this also covers the older
      // "one side is a literal" shape without a separate case for it. The
      // product is capped the same as everywhere else in this file -- two
      // genuinely unrelated small sets are rare enough that paying for the
      // full cross product when they occur costs nothing most of the time.
      const std::optional<std::vector<uint64_t>> left =
          exactValues(function, expr.operands[0], depth + 1);
      if (!left.has_value()) {
        return std::nullopt;
      }
      const std::optional<std::vector<uint64_t>> right =
          exactValues(function, expr.operands[1], depth + 1);
      if (!right.has_value()) {
        return std::nullopt;
      }
      if (left->size() * right->size() > kSetCap) {
        return std::nullopt;
      }
      std::vector<uint64_t> out;
      for (const uint64_t lhs : *left) {
        for (const uint64_t rhs : *right) {
          insertUnique(out, combineBinary(expr.op, lhs, rhs, width));
        }
      }
      return out.size() > kSetCap ? std::nullopt : std::optional<std::vector<uint64_t>>{out};
    }
    case il::ExprOp::Select: {
      // Whichever arm runs is the whole value, so ordinarily both must
      // answer -- the same requirement localBound's Select case has, for the
      // same reason.
      const il::ExprId trueArm = expr.operands[1];
      const il::ExprId falseArm = expr.operands[2];
      const std::optional<std::vector<uint64_t>> onTrue =
          exactValues(function, trueArm, depth + 1);
      const std::optional<std::vector<uint64_t>> onFalse =
          exactValues(function, falseArm, depth + 1);
      if (onTrue.has_value() && onFalse.has_value()) {
        std::vector<uint64_t> out = *onTrue;
        for (const uint64_t value : *onFalse) {
          insertUnique(out, value);
        }
        return out.size() > kSetCap ? std::nullopt : std::optional<std::vector<uint64_t>>{out};
      }
      // cinc/csinc: `select(cond, state + 1, state)` is one instruction, so
      // when exactly one arm fails to answer on its own but is structurally
      // the *other* arm plus (or minus) a literal -- the same relationship
      // armBound reads to shift a guard's bound by the offset -- the known
      // arm's values, shifted by that same literal, are the unknown arm's
      // values too. This is what lets a table index built on an
      // EntryReg-anchored `state` (a value set, not a structural bound) still
      // enumerate exactly: armBound already covers the guard-bound case,
      // this covers the value-set one.
      if (onTrue.has_value() != onFalse.has_value()) {
        const il::ExprId knownArm = onTrue.has_value() ? trueArm : falseArm;
        const std::vector<uint64_t>& knownValues = onTrue.has_value() ? *onTrue : *onFalse;
        const il::ExprId unknownArm = onTrue.has_value() ? falseArm : trueArm;
        const il::Expr& shifted = function.expr(unknownArm);
        uint64_t literal = 0;
        if ((shifted.op == il::ExprOp::Add || shifted.op == il::ExprOp::Sub) &&
            sameValue(function, shifted.operands[0], knownArm) &&
            function.asConstant(shifted.operands[1], literal)) {
          std::vector<uint64_t> out = knownValues;
          for (const uint64_t value : knownValues) {
            const uint64_t shiftedValue =
                shifted.op == il::ExprOp::Add ? value + literal : value - literal;
            insertUnique(out, zeroExtend(shiftedValue, width));
          }
          return out.size() > kSetCap ? std::nullopt : std::optional<std::vector<uint64_t>>{out};
        }
      }
      return std::nullopt;
    }
    default:
      // Every op this switch does not otherwise know how to read still
      // produces a value of its own result type, and a narrow enough type is
      // itself a small set -- a comparison is i1, so it is 0 or 1 whichever
      // way it goes, exactly like `shr.u`'s fallback above but for the type
      // rather than a shift. This is what lets a lone `not(cmp.ne(a,b))`
      // (unpaired with the complementary comparison BitFactor above looks
      // for) still answer: nothing here reads `a` or `b`, but nothing needs
      // to.
      return valuesOfWidth(width);
  }
}

}  // namespace

std::optional<std::vector<uint64_t>> preciseIndexSet(const il::Function& function,
                                                     il::ExprId index) {
  std::optional<std::vector<uint64_t>> values = exactValues(function, index, 0);
  if (!values.has_value() || values->empty() || values->size() > kSetCap) {
    return std::nullopt;
  }
  std::sort(values->begin(), values->end());
  return values;
}

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
