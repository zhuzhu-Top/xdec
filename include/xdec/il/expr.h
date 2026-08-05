// Pure value expressions.
//
// THE central invariant of this IR: an Expr is a total, side-effect-free
// function of its operands. Two structurally identical expressions therefore
// denote the same value, always, which is what makes the expression pool
// hash-consed (structurally deduplicated).
//
// That invariant is why neither memory loads nor register reads are expressions.
// Two loads from the same address at different points in time may yield
// different values, so deduplicating them would silently change behaviour. Both
// are Ops that define a value instead, and expressions refer to those values.
// The same split exists in VEX (GET/PUT versus pure IRExpr) for the same reason.
//
// Hash-consing buys three things the later phases depend on: common
// subexpression elimination for free, O(1) structural equality (compare two
// ExprIds), and a canonical form for the MBA rewrite rules to match against.
#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

#include "xdec/il/type.h"
#include "xdec/support/handle.h"

namespace xdec::il {

struct ExprTag;
using ExprId = Handle<ExprTag>;

struct ValueTag;
/// A value defined exactly once by an Op. Values are SSA by construction.
using ValueId = Handle<ValueTag>;

/// How an expression's result type relates to its operands. The verifier uses
/// this instead of deriving types, so a malformed IR is reported rather than
/// silently reinterpreted.
enum class ResultRule : uint8_t {
  /// Result and all operands share one type.
  SameAsOperands,
  /// Result is i1 regardless of operand type; operands share one type.
  Boolean,
  /// Result type is carried by the expression and unconstrained by operands.
  Explicit,
  /// Result is the opaque flags type.
  FlagsResult,
  /// Result shares the type of operands 1 and 2 (Select).
  SameAsLastTwo,
};

enum class ExprCategory : uint8_t {
  Leaf,
  IntArithmetic,
  IntBitwise,
  IntShift,
  IntCompare,
  Cast,
  BitCount,
  Select,
  FlagProducer,
  FlagConsumer,
  FloatArithmetic,
  FloatCompare,
  FloatCast,
};

// name, text, minArity, maxArity, category, result rule
//
// `immediate` meaning per op is documented on ExprOp below.
#define XDEC_EXPR_OPS(X)                                                              \
  X(Const, "const", 0, 0, Leaf, Explicit)                                             \
  X(Value, "val", 0, 0, Leaf, Explicit)                                               \
  X(Undef, "undef", 0, 0, Leaf, Explicit)                                             \
  /* The value a register held at function entry. Distinct per register, which */   \
  /* undef cannot be: stack-frame and argument recovery stand on this leaf.    */   \
  X(EntryReg, "entry", 0, 0, Leaf, Explicit)                                           \
  X(Add, "add", 2, 2, IntArithmetic, SameAsOperands)                                   \
  X(Sub, "sub", 2, 2, IntArithmetic, SameAsOperands)                                   \
  X(Mul, "mul", 2, 2, IntArithmetic, SameAsOperands)                                   \
  X(MulHiU, "mulhi.u", 2, 2, IntArithmetic, SameAsOperands)                            \
  X(MulHiS, "mulhi.s", 2, 2, IntArithmetic, SameAsOperands)                            \
  X(DivU, "div.u", 2, 2, IntArithmetic, SameAsOperands)                                \
  X(DivS, "div.s", 2, 2, IntArithmetic, SameAsOperands)                                \
  X(RemU, "rem.u", 2, 2, IntArithmetic, SameAsOperands)                                \
  X(RemS, "rem.s", 2, 2, IntArithmetic, SameAsOperands)                                \
  X(Neg, "neg", 1, 1, IntArithmetic, SameAsOperands)                                   \
  X(And, "and", 2, 2, IntBitwise, SameAsOperands)                                       \
  X(Or, "or", 2, 2, IntBitwise, SameAsOperands)                                         \
  X(Xor, "xor", 2, 2, IntBitwise, SameAsOperands)                                       \
  X(Not, "not", 1, 1, IntBitwise, SameAsOperands)                                       \
  X(Shl, "shl", 2, 2, IntShift, SameAsOperands)                                         \
  X(ShrU, "shr.u", 2, 2, IntShift, SameAsOperands)                                      \
  X(ShrS, "shr.s", 2, 2, IntShift, SameAsOperands)                                      \
  X(RotR, "rotr", 2, 2, IntShift, SameAsOperands)                                       \
  X(RotL, "rotl", 2, 2, IntShift, SameAsOperands)                                       \
  X(CmpEq, "cmp.eq", 2, 2, IntCompare, Boolean)                                         \
  X(CmpNe, "cmp.ne", 2, 2, IntCompare, Boolean)                                         \
  X(CmpLtU, "cmp.ltu", 2, 2, IntCompare, Boolean)                                       \
  X(CmpLeU, "cmp.leu", 2, 2, IntCompare, Boolean)                                       \
  X(CmpLtS, "cmp.lts", 2, 2, IntCompare, Boolean)                                       \
  X(CmpLeS, "cmp.les", 2, 2, IntCompare, Boolean)                                       \
  X(ZExt, "zext", 1, 1, Cast, Explicit)                                                 \
  X(SExt, "sext", 1, 1, Cast, Explicit)                                                 \
  X(Trunc, "trunc", 1, 1, Cast, Explicit)                                               \
  X(Bitcast, "bitcast", 1, 1, Cast, Explicit)                                           \
  X(Extract, "extract", 1, 1, Cast, Explicit)                                           \
  X(Concat, "concat", 2, 2, Cast, Explicit)                                              \
  X(Clz, "clz", 1, 1, BitCount, SameAsOperands)                                          \
  X(Ctz, "ctz", 1, 1, BitCount, SameAsOperands)                                          \
  X(PopCount, "popcount", 1, 1, BitCount, SameAsOperands)                                \
  X(ByteSwap, "bswap", 1, 1, BitCount, SameAsOperands)                                   \
  X(BitReverse, "brev", 1, 1, BitCount, SameAsOperands)                                  \
  X(Select, "select", 3, 3, Select, SameAsLastTwo)                                        \
  X(FlagDef, "flagdef", 1, 3, FlagProducer, FlagsResult)                                  \
  X(FlagCond, "flagcond", 1, 1, FlagConsumer, Boolean)                                     \
  X(FlagBit, "flagbit", 1, 1, FlagConsumer, Boolean)                                        \
  X(FAdd, "fadd", 2, 2, FloatArithmetic, SameAsOperands)                                    \
  X(FSub, "fsub", 2, 2, FloatArithmetic, SameAsOperands)                                    \
  X(FMul, "fmul", 2, 2, FloatArithmetic, SameAsOperands)                                    \
  X(FDiv, "fdiv", 2, 2, FloatArithmetic, SameAsOperands)                                    \
  X(FNeg, "fneg", 1, 1, FloatArithmetic, SameAsOperands)                                     \
  X(FAbs, "fabs", 1, 1, FloatArithmetic, SameAsOperands)                                      \
  X(FSqrt, "fsqrt", 1, 1, FloatArithmetic, SameAsOperands)                                     \
  X(FCmpEq, "fcmp.eq", 2, 2, FloatCompare, Boolean)                                            \
  X(FCmpLt, "fcmp.lt", 2, 2, FloatCompare, Boolean)                                            \
  X(FCmpLe, "fcmp.le", 2, 2, FloatCompare, Boolean)                                            \
  X(FCmpUnordered, "fcmp.uno", 2, 2, FloatCompare, Boolean)                                    \
  X(FpConvert, "fpconvert", 1, 1, FloatCast, Explicit)                                          \
  X(FpToIntS, "fptoint.s", 1, 1, FloatCast, Explicit)                                            \
  X(FpToIntU, "fptoint.u", 1, 1, FloatCast, Explicit)                                            \
  X(IntToFpS, "inttofp.s", 1, 1, FloatCast, Explicit)                                            \
  X(IntToFpU, "inttofp.u", 1, 1, FloatCast, Explicit)

enum class ExprOp : uint8_t {
#define XDEC_EXPR_OP_ENUM(name, text, minArity, maxArity, category, result) name,
  XDEC_EXPR_OPS(XDEC_EXPR_OP_ENUM)
#undef XDEC_EXPR_OP_ENUM
      Count
};

struct ExprOpInfo {
  std::string_view text;
  uint8_t minArity;
  uint8_t maxArity;
  ExprCategory category;
  ResultRule result;
};

[[nodiscard]] const ExprOpInfo& info(ExprOp op) noexcept;
[[nodiscard]] std::string_view toString(ExprOp op) noexcept;
/// Looks up an op by its printed text. The printer and parser share this table,
/// so they cannot drift apart.
[[nodiscard]] bool parseExprOp(std::string_view text, ExprOp& out) noexcept;

/// Which arithmetic produced a flag bundle. Determines how the bits are defined
/// and which of them are meaningful.
enum class FlagOp : uint8_t {
  /// N and Z from the sum, C from unsigned overflow, V from signed overflow.
  Add,
  /// As Add but for subtraction; C is "no borrow", matching ARM and inverted
  /// relative to x86.
  Sub,
  /// Add with a carry-in operand.
  AddCarry,
  /// Subtract with a borrow-in operand.
  SubCarry,
  /// N and Z from a single result operand; C and V are cleared. This is what a
  /// bitwise instruction that sets flags produces.
  Logical,
  /// A literal flag bundle, read as NZCV in bits 3..0 of its single constant
  /// operand. Needed wherever an architecture writes the flags directly:
  /// AArch64 ccmp and `msr nzcv`, x86 popf.
  Const,
  Count
};

[[nodiscard]] std::string_view toString(FlagOp op) noexcept;
[[nodiscard]] bool parseFlagOp(std::string_view text, FlagOp& out) noexcept;

/// Individual flag bits, for the rare instruction that reads one directly.
enum class FlagBitIndex : uint8_t { Negative, Zero, Carry, Overflow, Count };

[[nodiscard]] std::string_view toString(FlagBitIndex bit) noexcept;
[[nodiscard]] bool parseFlagBit(std::string_view text, FlagBitIndex& out) noexcept;

/// Architecture-neutral condition codes. The set is ARM's sixteen because it is
/// a superset of what other targets need; x86 and RISC-V conditions map onto it.
enum class ConditionCode : uint8_t {
  Equal,
  NotEqual,
  /// Unsigned greater or equal (ARM cs/hs).
  CarrySet,
  /// Unsigned less than (ARM cc/lo).
  CarryClear,
  Negative,
  NonNegative,
  Overflow,
  NoOverflow,
  /// Unsigned greater than.
  UnsignedGreater,
  /// Unsigned less or equal.
  UnsignedLessEqual,
  SignedGreaterEqual,
  SignedLess,
  SignedGreater,
  SignedLessEqual,
  Always,
  Never,
  Count
};

[[nodiscard]] std::string_view toString(ConditionCode code) noexcept;
[[nodiscard]] bool parseConditionCode(std::string_view text, ConditionCode& out) noexcept;
/// The condition that is true exactly when `code` is false.
[[nodiscard]] ConditionCode invert(ConditionCode code) noexcept;

inline constexpr unsigned kMaxExprOperands = 3;

/// A node in the pure value graph.
///
/// The meaning of `immediate` depends on `op`:
///   Const     the constant value, zero-extended within the type's width
///   Value     the ValueId index this expression refers to
///   Undef     unused
///   EntryReg  the RegId whose entry value this denotes
///   Extract   the low bit offset of the extracted field
///   FlagDef   the FlagOp, plus the operand width in the high bits
///   FlagCond  the ConditionCode
///   FlagBit   the FlagBitIndex
///   others    unused, and required to be zero
struct Expr {
  ExprOp op = ExprOp::Undef;
  uint8_t operandCount = 0;
  Type type;
  ExprId operands[kMaxExprOperands];
  uint64_t immediate = 0;

  [[nodiscard]] ExprId operand(unsigned index) const noexcept {
    XDEC_DASSERT(index < operandCount, "expression operand index out of range");
    return operands[index];
  }

  friend bool operator==(const Expr& lhs, const Expr& rhs) noexcept {
    if (lhs.op != rhs.op || lhs.type != rhs.type || lhs.operandCount != rhs.operandCount ||
        lhs.immediate != rhs.immediate) {
      return false;
    }
    for (unsigned index = 0; index < lhs.operandCount; ++index) {
      if (lhs.operands[index] != rhs.operands[index]) {
        return false;
      }
    }
    return true;
  }
};

/// Packs a FlagOp and an operand width into the FlagDef immediate.
[[nodiscard]] constexpr uint64_t packFlagDef(FlagOp op, unsigned width) noexcept {
  return static_cast<uint64_t>(op) | (static_cast<uint64_t>(width) << 8);
}

[[nodiscard]] constexpr FlagOp flagDefOp(uint64_t immediate) noexcept {
  return static_cast<FlagOp>(immediate & 0xFF);
}

[[nodiscard]] constexpr unsigned flagDefWidth(uint64_t immediate) noexcept {
  return static_cast<unsigned>((immediate >> 8) & 0xFFFF);
}

}  // namespace xdec::il

template <>
struct std::hash<xdec::il::Expr> {
  [[nodiscard]] std::size_t operator()(const xdec::il::Expr& expr) const noexcept {
    // FNV-1a over the fields that participate in structural identity.
    std::size_t digest = 0xcbf29ce484222325ull;
    const auto mix = [&digest](uint64_t value) {
      for (unsigned byte = 0; byte < 8; ++byte) {
        digest ^= static_cast<std::size_t>((value >> (byte * 8)) & 0xFF);
        digest *= 0x100000001b3ull;
      }
    };
    mix(static_cast<uint64_t>(expr.op));
    mix(expr.type.packed());
    mix(expr.immediate);
    for (unsigned index = 0; index < expr.operandCount; ++index) {
      mix(expr.operands[index].index());
    }
    return digest;
  }
};
