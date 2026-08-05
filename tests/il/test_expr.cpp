#include <catch2/catch_test_macros.hpp>

#include <string>
#include <unordered_set>

#include "il/il_test_support.h"
#include "xdec/il/expr.h"
#include "xdec/il/function.h"
#include "xdec/il/printer.h"

using namespace xdec::il;
using xdec::test::arm64Registers;

namespace {

Function makeFunction() { return Function{xdec::Arch::AArch64, arm64Registers(), 0x1000}; }

}  // namespace

TEST_CASE("the op table is consistent with its printed names", "[il][expr]") {
  std::unordered_set<std::string> names;
  for (unsigned index = 0; index < static_cast<unsigned>(ExprOp::Count); ++index) {
    const auto op = static_cast<ExprOp>(index);
    const std::string_view text = toString(op);
    REQUIRE_FALSE(text.empty());
    // Duplicate spellings would make the parser ambiguous.
    CHECK(names.insert(std::string{text}).second);

    ExprOp parsed = ExprOp::Undef;
    REQUIRE(parseExprOp(text, parsed));
    CHECK(parsed == op);

    const ExprOpInfo& opInfo = info(op);
    CHECK(opInfo.minArity <= opInfo.maxArity);
    CHECK(opInfo.maxArity <= kMaxExprOperands);
  }

  SECTION("an unknown spelling fails instead of defaulting") {
    ExprOp parsed = ExprOp::Undef;
    CHECK_FALSE(parseExprOp("frobnicate", parsed));
    CHECK_FALSE(parseExprOp("", parsed));
  }
}

TEST_CASE("the opcode table is consistent with its printed names", "[il][op]") {
  std::unordered_set<std::string> names;
  for (unsigned index = 0; index < static_cast<unsigned>(OpCode::Count); ++index) {
    const auto code = static_cast<OpCode>(index);
    const std::string_view text = toString(code);
    REQUIRE_FALSE(text.empty());
    CHECK(names.insert(std::string{text}).second);

    OpCode parsed = OpCode::Nop;
    REQUIRE(parseOpCode(text, parsed));
    CHECK(parsed == code);
  }

  SECTION("exactly the control-flow opcodes end a block") {
    CHECK(isTerminator(OpCode::Branch));
    CHECK(isTerminator(OpCode::CondBranch));
    CHECK(isTerminator(OpCode::IndirectBranch));
    CHECK(isTerminator(OpCode::Return));
    CHECK(isTerminator(OpCode::Unreachable));
    // An unlifted instruction ends its block: nothing after it can be trusted.
    CHECK(isTerminator(OpCode::Unimplemented));
    // A call is not a terminator; control normally comes back.
    CHECK_FALSE(isTerminator(OpCode::Call));
    CHECK_FALSE(isTerminator(OpCode::Store));
  }
}

TEST_CASE("expressions are hash-consed", "[il][expr]") {
  Function function = makeFunction();

  SECTION("structurally identical expressions are the same node") {
    const ExprId first = function.constant(Type::integer(64), 0x40);
    const ExprId second = function.constant(Type::integer(64), 0x40);
    CHECK(first == second);
    // Which means structural equality costs one integer compare, and common
    // subexpression elimination happens for free.
    CHECK(function.exprCount() == 1);
  }

  SECTION("the same bits at different widths are different nodes") {
    const ExprId wide = function.constant(Type::integer(64), 1);
    const ExprId narrow = function.constant(Type::integer(32), 1);
    CHECK(wide != narrow);
  }

  SECTION("constants are normalised to their declared width") {
    // Both spellings of -0x60 in 32 bits must land on one node, or the printer
    // and parser would disagree about which is canonical.
    const ExprId sign = function.constant(Type::integer(32), ~uint64_t{0x60} + 1);
    const ExprId masked = function.constant(Type::integer(32), 0xFFFFFFA0);
    CHECK(sign == masked);
    uint64_t value = 0;
    REQUIRE(function.asConstant(sign, value));
    CHECK(value == 0xFFFFFFA0);
  }

  SECTION("a shared subtree is stored once") {
    const ExprId base = function.constant(Type::integer(64), 0x1000);
    const ExprId offset = function.constant(Type::integer(64), 8);
    const ExprId sumA = function.binary(ExprOp::Add, base, offset);
    const ExprId sumB = function.binary(ExprOp::Add, base, offset);
    CHECK(sumA == sumB);
    CHECK(function.exprCount() == 3);
  }

  SECTION("operand order matters for non-commutative ops") {
    const ExprId a = function.constant(Type::integer(64), 1);
    const ExprId b = function.constant(Type::integer(64), 2);
    CHECK(function.binary(ExprOp::Sub, a, b) != function.binary(ExprOp::Sub, b, a));
  }

  SECTION("operands always precede their user, which makes the graph acyclic") {
    const ExprId leaf = function.constant(Type::integer(64), 7);
    const ExprId negated = function.unary(ExprOp::Neg, leaf);
    CHECK(leaf.index() < negated.index());
  }
}

TEST_CASE("expression builders derive the result type", "[il][expr]") {
  Function function = makeFunction();
  const ExprId a = function.constant(Type::integer(64), 3);
  const ExprId b = function.constant(Type::integer(64), 5);

  CHECK(function.expr(function.binary(ExprOp::Add, a, b)).type == Type::integer(64));
  // A comparison narrows to i1 no matter how wide its operands are.
  CHECK(function.expr(function.binary(ExprOp::CmpEq, a, b)).type == Type::boolean());
  CHECK(function.expr(function.cast(ExprOp::ZExt, Type::integer(64), a)).type ==
        Type::integer(64));
  CHECK(function.expr(function.extract(Type::integer(8), a, 8)).type == Type::integer(8));
  CHECK(function.expr(function.concat(Type::integer(128), a, b)).type == Type::integer(128));

  const ExprId condition = function.boolean(true);
  CHECK(function.expr(function.select(condition, a, b)).type == Type::integer(64));
}

TEST_CASE("flag bundles stay lazy", "[il][expr][flags]") {
  Function function = makeFunction();
  const ExprId a = function.constant(Type::integer(64), 0x10);
  const ExprId b = function.constant(Type::integer(64), 0x18);
  const ExprId operands[] = {a, b};

  const ExprId flags = function.flagDef(FlagOp::Sub, 64, operands);

  SECTION("a flagdef is one opaque node, not four bit computations") {
    CHECK(function.expr(flags).type == Type::flags());
    CHECK(function.expr(flags).operandCount == 2);
    // The whole point: the four bits are never materialised here. Only the two
    // constants and the bundle exist.
    CHECK(function.exprCount() == 3);
  }

  SECTION("the producing operation and width survive in the immediate") {
    const uint64_t immediate = function.expr(flags).immediate;
    CHECK(flagDefOp(immediate) == FlagOp::Sub);
    CHECK(flagDefWidth(immediate) == 64);
  }

  SECTION("consumers reference the bundle rather than expanding it") {
    const ExprId equal = function.flagCondition(flags, ConditionCode::Equal);
    const ExprId carry = function.flagBitOf(flags, FlagBitIndex::Carry);
    CHECK(function.expr(equal).type == Type::boolean());
    CHECK(function.expr(carry).type == Type::boolean());
    CHECK(function.expr(equal).operands[0] == flags);
    CHECK(function.expr(carry).operands[0] == flags);
    // Two consumers of one bundle: three nodes on top of the two constants.
    CHECK(function.exprCount() == 5);
  }

  SECTION("the same condition on the same bundle interns to one node") {
    CHECK(function.flagCondition(flags, ConditionCode::Equal) ==
          function.flagCondition(flags, ConditionCode::Equal));
    CHECK(function.flagCondition(flags, ConditionCode::Equal) !=
          function.flagCondition(flags, ConditionCode::NotEqual));
  }

  SECTION("bundles from different widths are distinct") {
    CHECK(function.flagDef(FlagOp::Sub, 32, operands) != flags);
  }
}

TEST_CASE("flag and condition spellings round-trip", "[il][expr][flags]") {
  for (unsigned index = 0; index < static_cast<unsigned>(FlagOp::Count); ++index) {
    const auto op = static_cast<FlagOp>(index);
    FlagOp parsed = FlagOp::Add;
    REQUIRE(parseFlagOp(toString(op), parsed));
    CHECK(parsed == op);
  }
  for (unsigned index = 0; index < static_cast<unsigned>(FlagBitIndex::Count); ++index) {
    const auto bit = static_cast<FlagBitIndex>(index);
    FlagBitIndex parsed = FlagBitIndex::Zero;
    REQUIRE(parseFlagBit(toString(bit), parsed));
    CHECK(parsed == bit);
  }
  for (unsigned index = 0; index < static_cast<unsigned>(ConditionCode::Count); ++index) {
    const auto code = static_cast<ConditionCode>(index);
    ConditionCode parsed = ConditionCode::Always;
    REQUIRE(parseConditionCode(toString(code), parsed));
    CHECK(parsed == code);
  }
}

TEST_CASE("inverting a condition twice is the identity", "[il][expr][flags]") {
  for (unsigned index = 0; index < static_cast<unsigned>(ConditionCode::Count); ++index) {
    const auto code = static_cast<ConditionCode>(index);
    CHECK(invert(invert(code)) == code);
    CHECK(invert(code) != code);
  }
  // Spot-check the pairs a branch inversion pass relies on.
  CHECK(invert(ConditionCode::Equal) == ConditionCode::NotEqual);
  CHECK(invert(ConditionCode::SignedLess) == ConditionCode::SignedGreaterEqual);
  CHECK(invert(ConditionCode::Always) == ConditionCode::Never);
}

TEST_CASE("expressions print in a form the grammar describes", "[il][expr][print]") {
  Function function = makeFunction();
  const ExprId base = function.constant(Type::integer(64), 0x1000);
  const ExprId offset = function.constant(Type::integer(64), 0x60);
  const ExprId sum = function.binary(ExprOp::Sub, base, offset);

  CHECK(printExpr(function, sum) == "sub:i64(const:i64(0x1000), const:i64(0x60))");

  SECTION("small negative constants print signed, because stack offsets read better") {
    const ExprId negative = function.constant(Type::integer(64), ~uint64_t{0x60} + 1);
    CHECK(printExpr(function, negative) == "const:i64(-0x60)");
  }

  SECTION("flag ops print their modifier rather than their type") {
    const ExprId operands[] = {base, offset};
    const ExprId flags = function.flagDef(FlagOp::Sub, 64, operands);
    CHECK(printExpr(function, flags) ==
          "flagdef:sub.64(const:i64(0x1000), const:i64(0x60))");
    CHECK(printExpr(function, function.flagCondition(flags, ConditionCode::Equal)) ==
          "flagcond:eq(flagdef:sub.64(const:i64(0x1000), const:i64(0x60)))");
  }
}
