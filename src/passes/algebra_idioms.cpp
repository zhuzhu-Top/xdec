// The idiom matchers (see the header for what belongs here and what does not).
#include "algebra_idioms.h"

#include <optional>

#include "xdec/support/bits.h"

namespace xdec::passes {

namespace {

/// The constant an expression denotes, or absent. Every matcher below is
/// written against constants that are already folded: the caller simplifies
/// children first, so a constant subtree is a Const node by the time it
/// arrives.
struct Konst {
  bool present = false;
  uint64_t value = 0;

  [[nodiscard]] explicit operator bool() const noexcept { return present; }
};

[[nodiscard]] Konst constantOf(const il::Function& function, il::ExprId id) {
  Konst out;
  out.present = function.asConstant(id, out.value);
  return out;
}

/// Whether `a` and `b` are the same unordered pair as `x` and `y`. Needed by
/// every MBA identity: obfuscators are not consistent about which side of a
/// pair they write first, and hash-consing makes the comparison a handle test.
[[nodiscard]] bool samePair(il::ExprId a, il::ExprId b, il::ExprId x, il::ExprId y) {
  return (a == x && b == y) || (a == y && b == x);
}

/// The operand of a doubling, however it is spelled: `v * 2` and `v << 1` are
/// the same node to everyone except a pattern matcher.
[[nodiscard]] il::ExprId doubledOperand(const il::Function& function, il::ExprId id) {
  const il::Expr& expr = function.expr(id);
  const bool isMul = expr.op == il::ExprOp::Mul;
  const bool isShl = expr.op == il::ExprOp::Shl;
  if (!isMul && !isShl) {
    return il::ExprId{};
  }
  const Konst factor = constantOf(function, expr.operands[1]);
  if (!factor || factor.value != (isMul ? 2u : 1u)) {
    return il::ExprId{};
  }
  return expr.operands[0];
}

/// A binary node's operands when it has the given op, else nothing.
struct Pair {
  bool present = false;
  il::ExprId lhs;
  il::ExprId rhs;

  [[nodiscard]] explicit operator bool() const noexcept { return present; }
};

[[nodiscard]] Pair operandsOf(const il::Function& function, il::ExprId id, il::ExprOp op) {
  const il::Expr& expr = function.expr(id);
  if (expr.op != op || expr.operandCount != 2) {
    return {};
  }
  return Pair{true, expr.operands[0], expr.operands[1]};
}

/// Whether `bit` is bit `index` of `source`, as a one-bit value. Two spellings
/// mean the same thing here and both occur: `extract(source, index)`, and — for
/// index zero only — `trunc(source)`, which is what the local cast rules rename
/// a zero-anchored extract to.
[[nodiscard]] bool isBitOf(const il::Expr& bit, unsigned index, il::ExprId source) {
  if (!bit.type.isBoolean() || bit.operandCount != 1 || bit.operands[0] != source) {
    return false;
  }
  if (bit.op == il::ExprOp::Extract) {
    return bit.immediate == index;
  }
  return bit.op == il::ExprOp::Trunc && index == 0;
}

/// Which bits of a `w`-bit result a shift can leave set: everything a mask has
/// to cover for the mask to be doing nothing.
[[nodiscard]] uint64_t reachableBits(il::ExprOp op, unsigned width, unsigned amount) {
  if (amount >= width) {
    return 0;  // shifted entirely out; the fold below turns the node constant
  }
  switch (op) {
    case il::ExprOp::Shl:
      return zeroExtend(~uint64_t{0} << amount, width);
    case il::ExprOp::ShrU:
      return lowMask(width - amount);
    default:
      return zeroExtend(~uint64_t{0}, width);  // unknown: covers everything
  }
}

/// The shared shape behind both carry-compare idioms: one operand is
/// `cmpOp(sum, other)` or `cmpOp(other, sum)` (either position — the two
/// callers need opposite ones, see their own comments) and the sibling
/// operand is `eqOp(sum, 0)`, where `sum` is exactly `other + C` for some
/// nonzero constant `C`. `other` is what fold.cpp's `rewriteAdd` called `a`;
/// `C` is what it called `b`.
struct CarryBound {
  il::ExprId other;
  uint64_t bound = 0;
  il::Type type;
};

[[nodiscard]] std::optional<CarryBound> matchCarryBound(const il::Function& function,
                                                        il::ExprId cmpId, il::ExprId eqId,
                                                        il::ExprOp cmpOp, il::ExprOp eqOp) {
  const il::Expr& cmp = function.expr(cmpId);
  if (cmp.op != cmpOp || cmp.operandCount != 2) {
    return std::nullopt;
  }
  const Pair eq = operandsOf(function, eqId, eqOp);
  if (!eq) {
    return std::nullopt;
  }
  const Konst zero = constantOf(function, eq.rhs);
  if (!zero || zero.value != 0) {
    return std::nullopt;
  }
  for (const int arm : {0, 1}) {
    if (cmp.operands[arm] != eq.lhs) {
      continue;
    }
    const il::ExprId sumId = cmp.operands[arm];
    const il::ExprId other = cmp.operands[arm ^ 1];
    const il::Expr& sum = function.expr(sumId);
    if (sum.op != il::ExprOp::Add || sum.operandCount != 2 || sum.operands[0] != other) {
      continue;
    }
    if (!sum.type.isScalarInteger() || sum.type.bits() == 0) {
      continue;
    }
    const Konst added = constantOf(function, sum.operands[1]);
    if (!added || added.value == 0) {
      continue;
    }
    const unsigned width = sum.type.bits();
    const uint64_t bound = zeroExtend(0 - added.value, width);
    return CarryBound{other, bound, sum.type};
  }
  return std::nullopt;
}

}  // namespace

il::ExprId matchCarryCompare(il::Function& function, const il::Expr& andExpr) {
  // hi: `(sum <u a) & (sum != 0)`, sum's cmp puts sum first.
  for (const int arm : {0, 1}) {
    if (const auto match = matchCarryBound(function, andExpr.operands[arm],
                                           andExpr.operands[arm ^ 1], il::ExprOp::CmpLtU,
                                           il::ExprOp::CmpNe)) {
      return function.binary(il::ExprOp::CmpLtU, function.constant(match->type, match->bound),
                             match->other);
    }
  }
  return {};
}

il::ExprId matchCarryCompareOr(il::Function& function, const il::Expr& orExpr) {
  // ls: `(a <=u sum) | (sum == 0)`, sum's cmp puts sum second.
  for (const int arm : {0, 1}) {
    if (const auto match = matchCarryBound(function, orExpr.operands[arm],
                                           orExpr.operands[arm ^ 1], il::ExprOp::CmpLeU,
                                           il::ExprOp::CmpEq)) {
      return function.binary(il::ExprOp::CmpLeU, match->other,
                             function.constant(match->type, match->bound));
    }
  }
  return {};
}

il::ExprId matchSignExtend(il::Function& function, const il::Expr& orExpr) {
  const unsigned width = orExpr.type.bits();
  if (!orExpr.type.isScalarInteger() || width == 0) {
    return {};
  }
  // The two halves in either order: `(sext-of-a-bit & high) | (x & low)`.
  for (const int arm : {0, 1}) {
    const Pair high = operandsOf(function, orExpr.operands[arm], il::ExprOp::And);
    const Pair low = operandsOf(function, orExpr.operands[arm ^ 1], il::ExprOp::And);
    if (!high || !low) {
      continue;
    }
    const Konst highBits = constantOf(function, high.rhs);
    const Konst lowBits = constantOf(function, low.rhs);
    if (!highBits || !lowBits) {
      continue;
    }
    // The masks must partition the width at a boundary the result can be
    // written at: 8, 16 or 32 give a C type to truncate to, and 1 gives a
    // negation (below). A 5-bit field is provably a sign extension too, but
    // `sext(trunc(x, 5))` prints as a type nobody wrote, so it is left as the
    // masks it already is. Every sign extension the architecture has an
    // instruction for — sxtb, sxth, sxtw, and sbfx anchored at bit zero —
    // lands on one of the widths handled here.
    const unsigned fieldBits = countTrailingZeros(~lowBits.value);
    if (fieldBits != 1 && fieldBits != 8 && fieldBits != 16 && fieldBits != 32) {
      continue;
    }
    if (fieldBits >= width || lowBits.value != lowMask(fieldBits) ||
        highBits.value != zeroExtend(~lowBits.value, width)) {
      continue;
    }
    // The high half must be the low half's own sign bit, spread: sext of bit
    // `fieldBits - 1` of the very same value the low half masks.
    const il::Expr& spread = function.expr(high.lhs);
    if (spread.op != il::ExprOp::SExt || spread.type != orExpr.type) {
      continue;
    }
    if (!isBitOf(function.expr(spread.operands[0]), fieldBits - 1, low.lhs)) {
      continue;
    }
    // Spreading a single bit across a whole word is negation: bit clear gives
    // zero, bit set gives all ones, which is what `0 - (x & 1)` computes. This
    // is the `sbfm` case with the field one bit wide, and the shape a flattened
    // dispatcher's state comparisons arrive in.
    if (fieldBits == 1) {
      return function.unary(il::ExprOp::Neg,
                            function.binary(il::ExprOp::And, low.lhs, low.rhs));
    }
    return function.cast(il::ExprOp::SExt, orExpr.type,
                         function.cast(il::ExprOp::Trunc, il::Type::integer(fieldBits),
                                       low.lhs));
  }
  return {};
}

il::ExprId matchMaskedRotate(il::Function& function, const il::Expr& andExpr) {
  const unsigned width = andExpr.type.bits();
  if (!andExpr.type.isScalarInteger() || width == 0) {
    return {};
  }
  const Konst mask = constantOf(function, andExpr.operands[1]);
  if (!mask) {
    return {};
  }
  const il::Expr& rotate = function.expr(andExpr.operands[0]);
  if (rotate.op != il::ExprOp::RotR && rotate.op != il::ExprOp::RotL) {
    return {};
  }
  const Konst amount = constantOf(function, rotate.operands[1]);
  if (!amount || amount.value == 0 || amount.value >= width) {
    return {};
  }
  // A rotate is two shifts sharing a result: `rotr(v, k)` puts `v >> k` in the
  // low `w - k` bits and `v << (w - k)` in the rest. Whichever of the two the
  // mask keeps nothing of may be dropped, and what remains is that shift.
  const unsigned right =
      rotate.op == il::ExprOp::RotR ? static_cast<unsigned>(amount.value)
                                    : width - static_cast<unsigned>(amount.value);
  const uint64_t fromRight = lowMask(width - right);          // bits fed by v >> k
  const uint64_t fromLeft = zeroExtend(~fromRight, width);    // bits fed by v << (w-k)
  const il::ExprId value = rotate.operands[0];
  const il::Type type = andExpr.type;
  if ((mask.value & fromRight) == 0) {
    return function.binary(
        il::ExprOp::And,
        function.binary(il::ExprOp::Shl, value, function.constant(type, width - right)),
        andExpr.operands[1]);
  }
  if ((mask.value & fromLeft) == 0) {
    return function.binary(
        il::ExprOp::And,
        function.binary(il::ExprOp::ShrU, value, function.constant(type, right)),
        andExpr.operands[1]);
  }
  return {};
}

il::ExprId matchMaskedShift(il::Function& function, const il::Expr& andExpr) {
  const unsigned width = andExpr.type.bits();
  if (!andExpr.type.isScalarInteger() || width == 0) {
    return {};
  }
  const Konst mask = constantOf(function, andExpr.operands[1]);
  if (!mask) {
    return {};
  }
  const il::Expr& shift = function.expr(andExpr.operands[0]);
  if (shift.op != il::ExprOp::Shl && shift.op != il::ExprOp::ShrU) {
    return {};
  }
  const Konst amount = constantOf(function, shift.operands[1]);
  if (!amount || amount.value >= width) {
    return {};
  }
  const uint64_t reachable =
      reachableBits(shift.op, width, static_cast<unsigned>(amount.value));
  if ((mask.value & reachable) != reachable) {
    return {};
  }
  return andExpr.operands[0];
}

il::ExprId matchMbaAdd(il::Function& function, const il::Expr& addExpr) {
  for (const int arm : {0, 1}) {
    const il::ExprId lhs = addExpr.operands[arm];
    const il::ExprId rhs = addExpr.operands[arm ^ 1];

    // (x ^ y) + 2·(x & y) → x + y.
    if (const Pair xorPair = operandsOf(function, lhs, il::ExprOp::Xor); xorPair) {
      const il::ExprId doubled = doubledOperand(function, rhs);
      if (doubled.valid()) {
        if (const Pair andPair = operandsOf(function, doubled, il::ExprOp::And);
            andPair && samePair(xorPair.lhs, xorPair.rhs, andPair.lhs, andPair.rhs)) {
          return function.binary(il::ExprOp::Add, xorPair.lhs, xorPair.rhs);
        }
      }
    }
    const Pair orPair = operandsOf(function, lhs, il::ExprOp::Or);
    const Pair andPair = operandsOf(function, rhs, il::ExprOp::And);
    const Pair xorPair = operandsOf(function, rhs, il::ExprOp::Xor);
    // (x | y) + (x & y) → x + y.
    if (orPair && andPair && samePair(orPair.lhs, orPair.rhs, andPair.lhs, andPair.rhs)) {
      return function.binary(il::ExprOp::Add, orPair.lhs, orPair.rhs);
    }
    // (x & y) + (x ^ y) → x | y: the two operands select disjoint bits — those
    // in both, and those in exactly one — so no carry can cross between them
    // and the sum is the union.
    const Pair andLhs = operandsOf(function, lhs, il::ExprOp::And);
    if (andLhs && xorPair && samePair(andLhs.lhs, andLhs.rhs, xorPair.lhs, xorPair.rhs)) {
      return function.binary(il::ExprOp::Or, andLhs.lhs, andLhs.rhs);
    }
  }
  return {};
}

il::ExprId matchMbaOr(il::Function& function, const il::Expr& orExpr) {
  for (const int arm : {0, 1}) {
    // (x & y) | (x ^ y) → x | y, by the same disjoint-halves argument as the
    // addition above: bits in both, plus bits in exactly one, is bits in either.
    const Pair andPair = operandsOf(function, orExpr.operands[arm], il::ExprOp::And);
    const Pair xorPair = operandsOf(function, orExpr.operands[arm ^ 1], il::ExprOp::Xor);
    if (andPair && xorPair && samePair(andPair.lhs, andPair.rhs, xorPair.lhs, xorPair.rhs)) {
      return function.binary(il::ExprOp::Or, andPair.lhs, andPair.rhs);
    }
  }
  return {};
}

il::ExprId matchMbaXor(il::Function& function, const il::Expr& xorExpr) {
  for (const int arm : {0, 1}) {
    // (x | y) ^ (x & y) → x ^ y: the union minus the intersection.
    const Pair orPair = operandsOf(function, xorExpr.operands[arm], il::ExprOp::Or);
    const Pair andPair = operandsOf(function, xorExpr.operands[arm ^ 1], il::ExprOp::And);
    if (orPair && andPair && samePair(orPair.lhs, orPair.rhs, andPair.lhs, andPair.rhs)) {
      return function.binary(il::ExprOp::Xor, orPair.lhs, orPair.rhs);
    }
  }
  return {};
}

il::ExprId matchMbaSub(il::Function& function, const il::Expr& subExpr) {
  const il::ExprId lhs = subExpr.operands[0];
  const il::ExprId rhs = subExpr.operands[1];

  // (x | y) - (x & y) → x ^ y.
  {
    const Pair orPair = operandsOf(function, lhs, il::ExprOp::Or);
    const Pair andPair = operandsOf(function, rhs, il::ExprOp::And);
    if (orPair && andPair && samePair(orPair.lhs, orPair.rhs, andPair.lhs, andPair.rhs)) {
      return function.binary(il::ExprOp::Xor, orPair.lhs, orPair.rhs);
    }
  }
  // 2·(x | y) - (x ^ y) → x + y. Since `x | y` is `(x & y) + (x ^ y)`, doubling
  // it and taking one copy of the xor back leaves `2·(x & y) + (x ^ y)`, which
  // is the carry-and-sum decomposition of the addition itself. The samples
  // spell the doubling as a masked rotate, which the rewrites above have
  // already turned into the shift this matcher expects.
  {
    const il::ExprId doubled = doubledOperand(function, lhs);
    const Pair orPair = doubled.valid() ? operandsOf(function, doubled, il::ExprOp::Or)
                                        : Pair{};
    const Pair xorPair = operandsOf(function, rhs, il::ExprOp::Xor);
    if (orPair && xorPair && samePair(orPair.lhs, orPair.rhs, xorPair.lhs, xorPair.rhs)) {
      return function.binary(il::ExprOp::Add, orPair.lhs, orPair.rhs);
    }
  }
  return {};
}

il::ExprId matchObfuscatedIncrement(il::Function& function, const il::Expr& subExpr) {
  const Pair orPair = operandsOf(function, subExpr.operands[0], il::ExprOp::Or);
  if (!orPair) {
    return {};
  }
  const il::ExprId v = doubledOperand(function, orPair.lhs);
  const Konst two = constantOf(function, orPair.rhs);
  if (!v.valid() || !two || two.value != 2) {
    return {};
  }
  const Pair xorPair = operandsOf(function, subExpr.operands[1], il::ExprOp::Xor);
  if (!xorPair || xorPair.lhs != v) {
    return {};
  }
  const Konst one = constantOf(function, xorPair.rhs);
  if (!one || one.value != 1) {
    return {};
  }
  return function.binary(il::ExprOp::Add, v, function.constant(subExpr.type, 1));
}

bool matchEvenProduct(const il::Function& function, il::ExprId mulId) {
  const il::Expr& expr = function.expr(mulId);
  if (expr.op != il::ExprOp::Mul) {
    return false;
  }
  for (const int arm : {0, 1}) {
    const il::ExprId other = expr.operands[arm ^ 1];
    const il::Expr& factor = function.expr(expr.operands[arm]);
    // v * (v±1), either association: two consecutive integers.
    if (factor.op == il::ExprOp::Sub || factor.op == il::ExprOp::Add) {
      const Konst one = constantOf(function, factor.operands[1]);
      if (one && one.value == 1 && factor.operands[0] == other) {
        return true;
      }
    }
    // v * ~v, either association: NOT flips bit 0, so exactly one of the pair
    // is even no matter what v is.
    if (factor.op == il::ExprOp::Not && factor.operands[0] == other) {
      return true;
    }
  }
  return false;
}

il::ExprId matchShiftedCompare(il::Function& function, const il::Expr& cmpExpr) {
  // The shifted side can be either one: the IL has no `>` or `>=`, so half of
  // the comparisons a program makes arrive with their operands swapped.
  const bool mirrored = !operandsOf(function, cmpExpr.operands[0], il::ExprOp::Shl);
  const il::ExprId lhs = cmpExpr.operands[mirrored ? 1 : 0];
  const il::ExprId rhs = cmpExpr.operands[mirrored ? 0 : 1];
  const Pair shifted = operandsOf(function, lhs, il::ExprOp::Shl);
  if (!shifted) {
    return {};
  }
  const Konst amount = constantOf(function, shifted.rhs);
  const unsigned width = function.expr(lhs).type.bits();
  if (!amount || amount.value == 0 || amount.value >= width) {
    return {};
  }
  // Only widths the IL has a type for, which is what makes the narrowed
  // comparison expressible at all.
  const unsigned narrow = width - static_cast<unsigned>(amount.value);
  if (narrow != 8 && narrow != 16 && narrow != 32) {
    return {};
  }

  const il::Type type = il::Type::integer(narrow);
  il::ExprId other;
  if (const Konst constant = constantOf(function, rhs); constant) {
    // The shift has to be undoable on the constant too: a low bit set there is
    // a value the shifted side can never take, and dropping it would change
    // which way the comparison goes.
    if ((constant.value & lowMask(static_cast<unsigned>(amount.value))) != 0) {
      return {};
    }
    other = function.constant(type, constant.value >> amount.value);
  } else if (const Pair otherShift = operandsOf(function, rhs, il::ExprOp::Shl);
             otherShift) {
    const Konst otherAmount = constantOf(function, otherShift.rhs);
    if (!otherAmount || otherAmount.value != amount.value) {
      return {};
    }
    other = function.cast(il::ExprOp::Trunc, type, otherShift.lhs);
  } else {
    return {};
  }
  const il::ExprId narrowed = function.cast(il::ExprOp::Trunc, type, shifted.lhs);
  return mirrored ? function.binary(cmpExpr.op, other, narrowed)
                  : function.binary(cmpExpr.op, narrowed, other);
}

il::ExprId matchCancelledSubtrahend(il::Function& function, const il::Expr& cmpExpr) {
  if (cmpExpr.op != il::ExprOp::CmpEq && cmpExpr.op != il::ExprOp::CmpNe) {
    return {};
  }
  for (const int arm : {0, 1}) {
    const il::ExprId sumId = cmpExpr.operands[arm];
    const Pair sum = operandsOf(function, sumId, il::ExprOp::Add);
    const Pair sub = operandsOf(function, cmpExpr.operands[arm ^ 1], il::ExprOp::Sub);
    if (!sum || !sub) {
      continue;
    }
    for (const bool subIsLhs : {true, false}) {
      const il::ExprId a = subIsLhs ? sum.rhs : sum.lhs;
      const Pair inner =
          operandsOf(function, subIsLhs ? sum.lhs : sum.rhs, il::ExprOp::Sub);
      if (!inner || inner.rhs != sub.rhs) {
        continue;  // the subtracted value must be the very same node on both sides
      }
      const Konst k1 = constantOf(function, inner.lhs);
      const Konst k2 = constantOf(function, sub.lhs);
      if (!k1 || !k2) {
        continue;
      }
      const unsigned width = function.expr(sumId).type.bits();
      if (width == 0) {
        continue;
      }
      const uint64_t folded = zeroExtend(k2.value - k1.value, width);
      return function.binary(cmpExpr.op, a, function.constant(function.expr(sumId).type, folded));
    }
  }
  return {};
}

}  // namespace xdec::passes
