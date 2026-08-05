// Constant evaluation of pure expressions, shared with the interpreter (see
// ceval.h for why sharing is the point). The U128 toolkit comes from u128.h,
// the same file the interpreter uses — one implementation, two consumers.
#include "xdec/il/ceval.h"

#include "u128.h"

namespace xdec::il {

uint8_t evalFlagDef(FlagOp op, unsigned width,
                    std::span<const ConcreteValue> args) noexcept {
  const uint64_t m = lowMask(width);
  const auto signAt = [&](uint64_t v) { return ((v >> (width - 1)) & 1) != 0; };
  bool n = false;
  bool z = false;
  bool c = false;
  bool v = false;
  switch (op) {
    case FlagOp::Add:
    case FlagOp::AddCarry: {
      const uint64_t a = args[0].lo & m;
      const uint64_t b = args[1].lo & m;
      const uint64_t carryIn = op == FlagOp::AddCarry ? (args[2].lo & 1) : 0;
      uint64_t r;
      if (width == 64) {
        const uint64_t r1 = a + b;
        const bool c1 = r1 < a;
        r = r1 + carryIn;
        const bool c2 = r < r1;
        c = c1 || c2;
      } else {
        const uint64_t sum = a + b + carryIn;  // fits: width <= 63 here
        c = ((sum >> width) & 1) != 0;
        r = sum & m;
      }
      n = signAt(r);
      z = r == 0;
      v = (signAt(a) == signAt(b)) && (signAt(r) != signAt(a));
      break;
    }
    case FlagOp::Sub:
    case FlagOp::SubCarry: {
      const uint64_t a = args[0].lo & m;
      const uint64_t b = args[1].lo & m;
      // Plain subtraction is a + ~b + 1; sbc is a + ~b + carryIn.
      const uint64_t carryIn = op == FlagOp::SubCarry ? (args[2].lo & 1) : 1;
      // r = a - b - (1 - carryIn), C is "no borrow", computed as the carry out
      // of a + ~b + carryIn.
      const uint64_t notB = ~b;
      uint64_t r;
      if (width == 64) {
        const uint64_t r1 = a + notB;
        const bool c1 = r1 < a;
        r = r1 + carryIn;
        const bool c2 = r < r1;
        c = c1 || c2;
      } else {
        const uint64_t sum = a + (notB & m) + carryIn;
        c = ((sum >> width) & 1) != 0;
        r = sum & m;
      }
      n = signAt(r);
      z = r == 0;
      v = (signAt(a) != signAt(b)) && (signAt(r) != signAt(a));
      break;
    }
    case FlagOp::Logical: {
      const uint64_t r = args[0].lo & m;
      n = signAt(r);
      z = r == 0;
      break;
    }
    case FlagOp::Const:
      // The bundle is the value, NZCV in bits 3..0.
      return static_cast<uint8_t>(args[0].lo & 0xF);
    case FlagOp::Count:
      break;
  }
  return static_cast<uint8_t>((n ? 8 : 0) | (z ? 4 : 0) | (c ? 2 : 0) | (v ? 1 : 0));
}

bool evalCondition(ConditionCode code, uint8_t nzcv) noexcept {
  const bool n = (nzcv & 8) != 0;
  const bool z = (nzcv & 4) != 0;
  const bool c = (nzcv & 2) != 0;
  const bool v = (nzcv & 1) != 0;
  switch (code) {
    case ConditionCode::Equal: return z;
    case ConditionCode::NotEqual: return !z;
    case ConditionCode::CarrySet: return c;
    case ConditionCode::CarryClear: return !c;
    case ConditionCode::Negative: return n;
    case ConditionCode::NonNegative: return !n;
    case ConditionCode::Overflow: return v;
    case ConditionCode::NoOverflow: return !v;
    case ConditionCode::UnsignedGreater: return c && !z;
    case ConditionCode::UnsignedLessEqual: return !c || z;
    case ConditionCode::SignedGreaterEqual: return n == v;
    case ConditionCode::SignedLess: return n != v;
    case ConditionCode::SignedGreater: return !z && (n == v);
    case ConditionCode::SignedLessEqual: return z || (n != v);
    case ConditionCode::Always: return true;
    case ConditionCode::Never: return false;
    case ConditionCode::Count: break;
  }
  return false;
}

namespace {

/// Depth bound for the recursive walk. Legitimate lifted expressions nest a
/// handful of levels deep; an absurd depth means a pathological tree, and the
/// honest answer is "not folded", never a stack overflow.
constexpr unsigned kMaxEvalDepth = 256;

[[nodiscard]] ConcreteValue fromU(U128 v) { return ConcreteValue{v.lo, v.hi}; }

[[nodiscard]] bool evalRec(const Function& function, ExprId id, unsigned depth,
                           ConcreteValue& out) {
  if (depth > kMaxEvalDepth || !function.hasExpr(id)) {
    return false;
  }
  const Expr& expr = function.expr(id);
  const unsigned width = expr.type.bits();

  if (expr.op == ExprOp::Const) {
    out = fromU(mask(U128{expr.immediate, 0}, width));
    return true;
  }
  if (expr.operandCount == 0) {
    return false;  // Value, Undef: not constants
  }

  ConcreteValue args[kMaxExprOperands];
  for (unsigned index = 0; index < expr.operandCount; ++index) {
    if (!evalRec(function, expr.operands[index], depth + 1, args[index])) {
      return false;
    }
  }
  const auto at = [&args](unsigned index) { return U128{args[index].lo, args[index].hi}; };
  const auto boolean = [](bool value) {
    return ConcreteValue{value ? uint64_t{1} : uint64_t{0}, 0};
  };

  // The flags type has no width, so it precedes the width guard. Nothing but a
  // FlagDef produces a bundle; the value is the materialised NZCV in `lo`.
  if (expr.type.isFlags()) {
    if (expr.op != ExprOp::FlagDef) {
      return false;
    }
    out = ConcreteValue{evalFlagDef(flagDefOp(expr.immediate),
                                    flagDefWidth(expr.immediate),
                                    std::span<const ConcreteValue>{args, expr.operandCount}),
                        0};
    return true;
  }
  // Widths above 128 are outside everything the IL currently lifts; the
  // interpreter errors on them and the folder declines them.
  if (width == 0 || width > 128) {
    return false;
  }

  switch (expr.op) {
    case ExprOp::Add: {
      bool carry = false;
      out = fromU(add(at(0), at(1), width, carry));
      return true;
    }
    case ExprOp::Sub:
      out = fromU(sub(at(0), at(1), width));
      return true;
    case ExprOp::Mul:
      out = fromU(mulLow(at(0), at(1), width));
      return true;
    case ExprOp::MulHiU: {
      if (width > 64) {
        return false;  // mul64x64 covers 64 x 64
      }
      const auto [hi, lo] = mul64x64(at(0).lo, at(1).lo);
      (void)lo;
      out = fromU(mask(U128{hi, 0}, width));
      return true;
    }
    case ExprOp::MulHiS: {
      if (width > 64) {
        return false;
      }
      // Signed high half: unsigned product plus the sign corrections.
      const bool negA = signBit(at(0), width);
      const bool negB = signBit(at(1), width);
      const auto [hiU, lo] = mul64x64(at(0).lo, at(1).lo);
      (void)lo;
      uint64_t hi = hiU;
      if (negA) {
        hi -= at(1).lo;
      }
      if (negB) {
        hi -= at(0).lo;
      }
      out = fromU(mask(U128{hi, 0}, width));
      return true;
    }
    case ExprOp::DivU:
      out = at(1).lo == 0 ? ConcreteValue{} : fromU(mask(U128{at(0).lo / at(1).lo, 0}, width));
      return true;
    case ExprOp::RemU:
      out = at(1).lo == 0 ? ConcreteValue{} : fromU(mask(U128{at(0).lo % at(1).lo, 0}, width));
      return true;
    case ExprOp::DivS:
    case ExprOp::RemS: {
      if (at(1).lo == 0) {
        out = ConcreteValue{};
        return true;
      }
      const U128 dividend = sext(at(0), width, 128);
      const U128 divisor = sext(at(1), width, 128);
      const auto toI64 = [](U128 v) { return static_cast<int64_t>(v.lo); };
      const int64_t a = toI64(dividend);
      const int64_t b = toI64(divisor);
      // INT64_MIN / -1 overflows; AArch64 sdiv wraps the same way the
      // unsigned-friendly path below does, so mirror it.
      const int64_t r = a == INT64_MIN && b == -1
                            ? (expr.op == ExprOp::DivS ? a : 0)
                            : (expr.op == ExprOp::DivS ? a / b : a % b);
      out = fromU(mask(U128{static_cast<uint64_t>(r), 0}, width));
      return true;
    }
    case ExprOp::Neg:
      out = fromU(sub(U128{}, at(0), width));
      return true;
    case ExprOp::And:
      out = fromU(mask(U128{at(0).lo & at(1).lo, at(0).hi & at(1).hi}, width));
      return true;
    case ExprOp::Or:
      out = fromU(mask(U128{at(0).lo | at(1).lo, at(0).hi | at(1).hi}, width));
      return true;
    case ExprOp::Xor:
      out = fromU(mask(U128{at(0).lo ^ at(1).lo, at(0).hi ^ at(1).hi}, width));
      return true;
    case ExprOp::Not: {
      const U128 ones = maskOf(width);
      out = fromU(U128{at(0).lo ^ ones.lo, at(0).hi ^ ones.hi});
      return true;
    }
    case ExprOp::Shl:
      out = fromU(shl(at(0), width, static_cast<unsigned>(at(1).lo)));
      return true;
    case ExprOp::ShrU:
      out = fromU(shrU(at(0), width, static_cast<unsigned>(at(1).lo)));
      return true;
    case ExprOp::ShrS:
      out = fromU(shrS(at(0), width, static_cast<unsigned>(at(1).lo)));
      return true;
    case ExprOp::RotR:
      out = fromU(rotR(at(0), width, static_cast<unsigned>(at(1).lo)));
      return true;
    case ExprOp::RotL:
      out = fromU(rotL(at(0), width, static_cast<unsigned>(at(1).lo)));
      return true;
    // A comparison's own width is one bit -- it is a boolean -- so these are
    // the one family that must mask by the width of what is being compared.
    // Masking by the result width would compare bit zero of each operand and
    // call `4 == 6` true.
    case ExprOp::CmpEq:
    case ExprOp::CmpNe:
    case ExprOp::CmpLtU:
    case ExprOp::CmpLeU:
    case ExprOp::CmpLtS:
    case ExprOp::CmpLeS: {
      const unsigned operandWidth = function.expr(expr.operands[0]).type.bits();
      if (operandWidth == 0 || operandWidth > 128) {
        return false;
      }
      const U128 lhs = mask(at(0), operandWidth);
      const U128 rhs = mask(at(1), operandWidth);
      switch (expr.op) {
        case ExprOp::CmpEq: out = boolean(cmpU(lhs, rhs) == 0); return true;
        case ExprOp::CmpNe: out = boolean(cmpU(lhs, rhs) != 0); return true;
        case ExprOp::CmpLtU: out = boolean(cmpU(lhs, rhs) < 0); return true;
        case ExprOp::CmpLeU: out = boolean(cmpU(lhs, rhs) <= 0); return true;
        case ExprOp::CmpLtS: out = boolean(cmpS(lhs, rhs, operandWidth) < 0); return true;
        default: out = boolean(cmpS(lhs, rhs, operandWidth) <= 0); return true;
      }
    }
    case ExprOp::ZExt: {
      const unsigned fromWidth = function.expr(expr.operands[0]).type.bits();
      if (fromWidth == 0) {
        return false;
      }
      out = fromU(zext(at(0), fromWidth, width));
      return true;
    }
    case ExprOp::SExt: {
      const unsigned fromWidth = function.expr(expr.operands[0]).type.bits();
      out = fromU(sext(at(0), fromWidth, width));
      return true;
    }
    case ExprOp::Trunc:
      out = fromU(mask(at(0), width));
      return true;
    case ExprOp::Bitcast:
      out = args[0];
      return true;
    case ExprOp::Extract:
      out = fromU(extract(at(0), function.expr(expr.operands[0]).type.bits(),
                          static_cast<unsigned>(expr.immediate), width));
      return true;
    case ExprOp::Concat: {
      const unsigned lowWidth = function.expr(expr.operands[1]).type.bits();
      const unsigned highWidth = function.expr(expr.operands[0]).type.bits();
      if (width != lowWidth + highWidth) {
        return false;
      }
      out = fromU(insert(mask(at(1), lowWidth), mask(at(0), highWidth), lowWidth,
                         highWidth, width));
      return true;
    }
    case ExprOp::Clz:
      out = ConcreteValue{clzOf(at(0), width), 0};
      return true;
    case ExprOp::Ctz:
      out = ConcreteValue{ctzOf(at(0), width), 0};
      return true;
    case ExprOp::PopCount: {
      const U128 v = mask(at(0), width);
      out = ConcreteValue{static_cast<uint64_t>(std::popcount(v.lo) + std::popcount(v.hi)), 0};
      return true;
    }
    case ExprOp::ByteSwap:
      out = fromU(bswapOf(at(0), width));
      return true;
    case ExprOp::BitReverse:
      out = fromU(brevOf(at(0), width));
      return true;
    case ExprOp::Select:
      out = (args[0].lo & 1) != 0 ? args[1] : args[2];
      return true;
    case ExprOp::FlagCond:
      out = boolean(evalCondition(static_cast<ConditionCode>(expr.immediate),
                                  static_cast<uint8_t>(args[0].lo & 0xF)));
      return true;
    case ExprOp::FlagBit: {
      const auto bit = static_cast<FlagBitIndex>(expr.immediate);
      const unsigned position = bit == FlagBitIndex::Negative  ? 3
                                : bit == FlagBitIndex::Zero    ? 2
                                : bit == FlagBitIndex::Carry   ? 1
                                                                 : 0;
      out = boolean(((args[0].lo >> position) & 1) != 0);
      return true;
    }
    default:
      // Floats and anything not listed: not folded. See ceval.h for why the
      // honest answer to an uncovered op is to leave it alone.
      return false;
  }
}

}  // namespace

bool tryEvalConst(const Function& function, ExprId id, ConcreteValue& out) noexcept {
  return evalRec(function, id, 0, out);
}

}  // namespace xdec::il
