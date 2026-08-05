// The verifier's job is to turn a wrong result into an error message naming the
// pass and the address, so these tests are written as "break one invariant,
// check it is reported".
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

#include "il/il_test_support.h"
#include "xdec/il/function.h"
#include "xdec/il/parser.h"
#include "xdec/il/verify.h"

using namespace xdec::il;
using xdec::test::arm64Registers;

namespace {

RegId reg(const char* name) {
  const RegId id = arm64Registers().find(name);
  REQUIRE(id.valid());
  return id;
}

bool reports(const VerifyReport& report, std::string_view fragment) {
  for (const xdec::Diag& diag : report.errors) {
    if (diag.message().find(fragment) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool warns(const VerifyReport& report, std::string_view fragment) {
  for (const xdec::Diag& diag : report.warnings) {
    if (diag.message().find(fragment) != std::string::npos) {
      return true;
    }
  }
  return false;
}

/// A well-formed two-block function at Cfg maturity.
Function buildValid() {
  Function function{xdec::Arch::AArch64, arm64Registers(), 0x1000};
  function.setName("valid");
  const BlockId entry = function.createBlock(0x1000);
  const BlockId exit = function.createBlock(0x1008);
  function.block(entry).endVa = 0x1008;
  function.block(exit).endVa = 0x100C;

  const ValueId x0 = function.appendReadReg(entry, 0x1000, reg("x0"));
  const ExprId operands[] = {function.valueRef(x0), function.constant(Type::integer(64), 0)};
  const ExprId flags = function.flagDef(FlagOp::Sub, 64, operands);
  function.appendCondBranch(entry, 0x1004,
                            function.flagCondition(flags, ConditionCode::Equal), exit, exit);
  function.appendReturn(exit, 0x1008);
  function.rebuildEdges();
  function.setMaturity(Maturity::Cfg);
  return function;
}

}  // namespace

TEST_CASE("a well-formed function verifies clean", "[il][verify]") {
  const Function function = buildValid();
  const VerifyReport report = verify(function);
  INFO(report.format());
  CHECK(report.ok());
  CHECK(report.warnings.empty());
  CHECK(report.format() == "ok\n");
  CHECK(verifyOrFail(function).hasValue());
}

TEST_CASE("structural violations are reported", "[il][verify]") {
  SECTION("a function with no blocks") {
    const Function function{xdec::Arch::AArch64, arm64Registers(), 0x1000};
    const VerifyReport report = verify(function);
    CHECK_FALSE(report.ok());
    CHECK(reports(report, "no blocks"));
  }

  SECTION("an empty block") {
    Function function{xdec::Arch::AArch64, arm64Registers(), 0x1000};
    function.createBlock(0x1000);
    CHECK(reports(verify(function), "is empty"));
  }

  SECTION("a block that does not end in a terminator") {
    Function function{xdec::Arch::AArch64, arm64Registers(), 0x1000};
    const BlockId block = function.createBlock(0x1000);
    function.appendNop(block, 0x1000);
    function.rebuildEdges();

    // At Lifted the block is still being built, so this is not yet an error.
    CHECK(verify(function, Maturity::Lifted).ok());
    // By Cfg the graph is supposed to be complete.
    function.setMaturity(Maturity::Cfg);
    CHECK(reports(verify(function), "non-terminator"));
  }

  SECTION("two blocks claiming one address") {
    Function function{xdec::Arch::AArch64, arm64Registers(), 0x1000};
    const BlockId first = function.createBlock(0x1000);
    const BlockId second = function.createBlock(0x1000);
    function.appendReturn(first, 0x1000);
    function.appendReturn(second, 0x1000);
    function.rebuildEdges();
    CHECK(reports(verify(function), "two blocks start at"));
  }

  SECTION("an unreachable block is a warning, not an error") {
    Function function = buildValid();
    const BlockId orphan = function.createBlock(0x2000);
    function.appendReturn(orphan, 0x2000);
    function.rebuildEdges();

    const VerifyReport report = verify(function);
    // A pass may legitimately orphan a block before a cleanup pass removes it.
    CHECK(report.ok());
    CHECK(warns(report, "unreachable"));
  }
}

TEST_CASE("stale cached edges are caught", "[il][verify]") {
  Function function = buildValid();
  const BlockId third = function.createBlock(0x2000);
  function.appendReturn(third, 0x2000);

  // Retarget without rebuilding: exactly the mistake a structural pass makes.
  const BlockId retargeted[] = {third, third};
  const OpId terminator = function.block(function.entryBlock()).ops.back();
  function.setTargets(terminator, retargeted);

  CHECK(reports(verify(function), "rebuildEdges was not called"));
  function.rebuildEdges();
  INFO(verify(function).format());
  CHECK(verify(function).ok());
}

TEST_CASE("missing provenance is caught at lifted maturity", "[il][verify]") {
  Function function{xdec::Arch::AArch64, arm64Registers(), 0x1000};
  const BlockId block = function.createBlock(0x1000);
  function.appendReturn(block, kNoOpAddress);
  function.rebuildEdges();

  // Every op at this level corresponds to a machine instruction, so an op with
  // no address means the lifter lost track of where it came from.
  CHECK(reports(verify(function, Maturity::Lifted), "no source address"));
  // Later levels legitimately synthesise ops.
  function.setMaturity(Maturity::Cfg);
  CHECK(verify(function).ok());
}

TEST_CASE("type violations are caught", "[il][verify]") {
  SECTION("a branch condition that is not a boolean") {
    Function function{xdec::Arch::AArch64, arm64Registers(), 0x1000};
    const BlockId entry = function.createBlock(0x1000);
    const BlockId exit = function.createBlock(0x1004);
    function.appendCondBranch(entry, 0x1000, function.constant(Type::integer(64), 1), exit,
                              exit);
    function.appendReturn(exit, 0x1004);
    function.rebuildEdges();
    CHECK(reports(verify(function), "must be i1"));
  }

  SECTION("a register write of the wrong width") {
    Function function{xdec::Arch::AArch64, arm64Registers(), 0x1000};
    const BlockId block = function.createBlock(0x1000);
    function.appendWriteReg(block, 0x1000, reg("x0"),
                            function.constant(Type::integer(32), 1));
    function.appendReturn(block, 0x1004);
    function.rebuildEdges();
    CHECK(reports(verify(function), "register 'x0'"));
  }

  SECTION("a store whose value does not match the access width") {
    Function function{xdec::Arch::AArch64, arm64Registers(), 0x1000};
    const BlockId block = function.createBlock(0x1000);
    const ExprId address = function.constant(Type::integer(64), 0x2000);
    function.appendStore(block, 0x1000, Type::integer(32), address,
                         function.constant(Type::integer(64), 1));
    function.appendReturn(block, 0x1004);
    function.rebuildEdges();
    CHECK(reports(verify(function), "stores i64"));
  }

  SECTION("an add of mismatched operand widths") {
    Function function{xdec::Arch::AArch64, arm64Registers(), 0x1000};
    const BlockId block = function.createBlock(0x1000);
    // Built through intern() to bypass the type-deriving builders, which is how
    // a buggy pass would produce it.
    Expr expr;
    expr.op = ExprOp::Add;
    expr.type = Type::integer(64);
    expr.operandCount = 2;
    expr.operands[0] = function.constant(Type::integer(64), 1);
    expr.operands[1] = function.constant(Type::integer(32), 2);
    function.appendWriteReg(block, 0x1000, reg("x0"), function.intern(expr));
    function.appendReturn(block, 0x1004);
    function.rebuildEdges();
    CHECK(reports(verify(function), "does not match operand"));
  }

  SECTION("an extract that reaches past its source") {
    Function function{xdec::Arch::AArch64, arm64Registers(), 0x1000};
    const BlockId block = function.createBlock(0x1000);
    const ExprId source = function.constant(Type::integer(32), 0);
    const ExprId bad = function.extract(Type::integer(16), source, 24);
    function.appendWriteReg(block, 0x1000, reg("x0"),
                            function.cast(ExprOp::ZExt, Type::integer(64), bad));
    function.appendReturn(block, 0x1004);
    function.rebuildEdges();
    CHECK(reports(verify(function), "extracts 16 bits at offset 24"));
  }

  SECTION("a concat whose parts do not add up") {
    Function function{xdec::Arch::AArch64, arm64Registers(), 0x1000};
    const BlockId block = function.createBlock(0x1000);
    const ExprId half = function.constant(Type::integer(32), 0);
    function.appendWriteReg(block, 0x1000, reg("x0"),
                            function.concat(Type::integer(96), half, half));
    function.appendReturn(block, 0x1004);
    function.rebuildEdges();
    CHECK(reports(verify(function), "concatenates 64 bits"));
  }

  SECTION("a flag condition applied to something that is not a flag bundle") {
    Function function{xdec::Arch::AArch64, arm64Registers(), 0x1000};
    const BlockId block = function.createBlock(0x1000);
    Expr expr;
    expr.op = ExprOp::FlagCond;
    expr.type = Type::boolean();
    expr.operandCount = 1;
    expr.operands[0] = function.constant(Type::integer(64), 0);
    expr.immediate = static_cast<uint64_t>(ConditionCode::Equal);
    function.appendWriteReg(
        block, 0x1000, reg("x0"),
        function.cast(ExprOp::ZExt, Type::integer(64), function.intern(expr)));
    function.appendReturn(block, 0x1004);
    function.rebuildEdges();
    CHECK(reports(verify(function), "must be flags"));
  }

  SECTION("a flagdef with the wrong operand count for its operation") {
    Function function{xdec::Arch::AArch64, arm64Registers(), 0x1000};
    const BlockId block = function.createBlock(0x1000);
    // Logical flags come from one result operand; two means the producer is
    // confused about which operation it is modelling.
    const ExprId operands[] = {function.constant(Type::integer(64), 1),
                               function.constant(Type::integer(64), 2)};
    function.appendWriteReg(block, 0x1000, reg("nzcv"),
                            function.flagDef(FlagOp::Logical, 64, operands));
    function.appendReturn(block, 0x1004);
    function.rebuildEdges();
    CHECK(reports(verify(function), "flagdef.logic takes 1 operands"));
  }

  SECTION("a flags constant carries a value and nothing else") {
    // `ccmp` writes a literal NZCV when its condition fails, so this shape is
    // one a real lifter produces and the verifier must accept it.
    Function function{xdec::Arch::AArch64, arm64Registers(), 0x1000};
    const BlockId block = function.createBlock(0x1000);
    const ExprId value[] = {function.constant(Type::integer(64), 0b0010)};
    function.appendWriteReg(block, 0x1000, reg("nzcv"),
                            function.flagDef(FlagOp::Const, 64, value));
    function.appendReturn(block, 0x1004);
    function.rebuildEdges();
    CHECK(verify(function).ok());
  }
}

TEST_CASE("value violations are caught", "[il][verify]") {
  SECTION("a value used before its definition") {
    Function function{xdec::Arch::AArch64, arm64Registers(), 0x1000};
    const BlockId block = function.createBlock(0x1000);
    // Reserve a value by defining it, then reference it from an earlier op by
    // rewriting that op's operands.
    const ValueId later = function.appendReadReg(block, 0x1004, reg("x1"));
    const OpId write = function.appendWriteReg(block, 0x1000, reg("x0"),
                                               function.constant(Type::integer(64), 0));
    const ExprId operands[] = {function.valueRef(later)};
    function.setOperands(write, operands);
    // Swap the two ops so the use precedes the definition.
    Block& body = function.block(block);
    std::swap(body.ops[0], body.ops[1]);
    function.appendReturn(block, 0x1008);
    function.rebuildEdges();

    CHECK(reports(verify(function), "before its definition"));
  }

  SECTION("a value used across blocks below ssa maturity") {
    Function function{xdec::Arch::AArch64, arm64Registers(), 0x1000};
    const BlockId first = function.createBlock(0x1000);
    const BlockId second = function.createBlock(0x1004);
    const ValueId defined = function.appendReadReg(first, 0x1000, reg("x0"));
    function.appendBranch(first, 0x1000, second);
    function.appendWriteReg(second, 0x1004, reg("x1"), function.valueRef(defined));
    function.appendReturn(second, 0x1008);
    function.rebuildEdges();

    // Below SSA there is no phi machinery, so a cross-block value reference has
    // no defined meaning and must be reported rather than assumed.
    function.setMaturity(Maturity::Cfg);
    CHECK(reports(verify(function), "block-local"));

    // At SSA it becomes legal.
    function.setMaturity(Maturity::Ssa);
    INFO(verify(function).format());
    CHECK(verify(function).ok());
  }

  SECTION("a phi with the wrong number of operands") {
    auto parsed = parse(
        "function @0x1000 name=\"bad_phi\" arch=arm64 maturity=ssa {\n"
        "  block b0 @0x1000..0x1004 entry preds=[] {\n"
        "    @0x1000\n"
        "    %0 = read x0\n"
        "    brc cmp.eq:i1(val:i64(%0), const:i64(0x0)), b1, b2\n"
        "  }\n"
        "  block b1 @0x1004..0x1008 preds=[b0] {\n"
        "    @0x1004\n"
        "    %1 = read x1\n"
        "    br b3\n"
        "  }\n"
        "  block b2 @0x1008..0x100c preds=[b0] {\n"
        "    @0x1008\n"
        "    %2 = read x2\n"
        "    br b3\n"
        "  }\n"
        "  block b3 @0x100c..0x1010 preds=[b1, b2] {\n"
        "    @0x100c\n"
        "    %3 = phi:i64(val:i64(%1))\n"
        "    ret\n"
        "  }\n"
        "}\n",
        arm64Registers());
    REQUIRE(parsed.hasValue());
    CHECK(reports(verify(**parsed), "phi has 1 operands but block b3 has 2 predecessors"));
  }

  SECTION("a phi whose operand type disagrees with its result") {
    auto parsed = parse(
        "function @0x1000 name=\"bad_phi_type\" arch=arm64 maturity=ssa {\n"
        "  block b0 @0x1000..0x1004 entry preds=[] {\n"
        "    @0x1000\n"
        "    %0 = read w0\n"
        "    br b1\n"
        "  }\n"
        "  block b1 @0x1004..0x1008 preds=[b0] {\n"
        "    @0x1004\n"
        "    %1 = phi:i64(val:i32(%0))\n"
        "    ret\n"
        "  }\n"
        "}\n",
        arm64Registers());
    REQUIRE(parsed.hasValue());
    CHECK(reports(verify(**parsed), "phi operand type i32"));
  }
}

TEST_CASE("an unresolved indirect branch is legal until the resolved level",
          "[il][verify]") {
  Function function{xdec::Arch::AArch64, arm64Registers(), 0x1000};
  const BlockId block = function.createBlock(0x1000);
  const ValueId target = function.appendReadReg(block, 0x1000, reg("x0"));
  function.appendIndirectBranch(block, 0x1004, function.valueRef(target));
  function.rebuildEdges();

  function.setMaturity(Maturity::Cfg);
  INFO(verify(function).format());
  CHECK(verify(function).ok());

  // The Resolved contract is that every computed edge is either known or
  // recorded as unknowable, so silence here would be a lie.
  function.setMaturity(Maturity::Resolved);
  CHECK(reports(verify(function), "still unresolved"));
}

TEST_CASE("verifyOrFail collects every error as a note", "[il][verify]") {
  Function function{xdec::Arch::AArch64, arm64Registers(), 0x1000};
  const BlockId first = function.createBlock(0x1000);
  function.createBlock(0x1010);
  function.appendNop(first, 0x1000);
  function.setMaturity(Maturity::Cfg);

  const auto result = verifyOrFail(function);
  REQUIRE_FALSE(result.hasValue());
  CHECK(result.error().code() == xdec::DiagCode::VerifyFailure);
  CHECK(result.error().notes().size() >= 2);
  CHECK(result.error().message().find("error(s)") != std::string::npos);
}
