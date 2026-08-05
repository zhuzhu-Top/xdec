// The block-local transforms: constant folding, lazy-flag condition folding,
// copy propagation, dead-code elimination — and the local-simplify fixpoint
// that composes them.
//
// The acceptance bar is not "the text looks simpler"; it is twofold:
// structural checks on the rewritten IL, and interpreter equivalence — the
// interpreter is the oracle, so a simplified block must compute exactly what
// the lifted block computed.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "il/il_test_support.h"
#include "xdec/il/ceval.h"
#include "xdec/il/function.h"
#include "xdec/il/interp.h"
#include "xdec/il/printer.h"
#include "xdec/il/verify.h"
#include "xdec/pass/manager.h"
#include "xdec/pass/registry.h"
#include "xdec/passes/builtin.h"

// The transforms are plain functions; the test binary reaches them through
// the same header the pass uses.
#include "../../src/passes/transform.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::il::BlockId;
using xdec::il::ConditionCode;
using xdec::il::ConcreteValue;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::FlagOp;
using xdec::il::Function;
using xdec::il::Maturity;
using xdec::il::RegId;
using xdec::il::Type;
using xdec::il::ValueId;

namespace {

struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {
    block = function.createBlock(0x1000);
    function.setEntryBlock(block);
    x0 = function.registers().find("x0");
    x1 = function.registers().find("x1");
    x2 = function.registers().find("x2");
    w0 = function.registers().find("w0");
    xzr = function.registers().find("xzr");
    nzcv = function.registers().find("nzcv");
  }

  /// A read of a register, as a value expression.
  ExprId read(RegId reg) {
    return function.valueRef(function.appendReadReg(block, 0x1000 + vaStep++ * 4, reg));
  }

  void write(RegId reg, ExprId value) {
    function.appendWriteReg(block, 0x1000 + vaStep++ * 4, reg, value);
  }

  ExprId constant(uint64_t value) {
    return function.constant(Type::integer(64), value);
  }

  void finish() {
    function.appendReturn(block, 0x1000 + vaStep++ * 4);
    function.rebuildEdges();
  }

  Function function;
  BlockId block;
  RegId x0, x1, x2, w0, xzr, nzcv;
  uint32_t vaStep = 0;
};

[[nodiscard]] bool verifiesClean(const Function& function) {
  const xdec::il::VerifyReport report = xdec::il::verify(function, function.maturity());
  for (const xdec::Diag& diag : report.errors) {
    INFO(diag.format());
  }
  return report.ok();
}

TEST_CASE("constants fold through arithmetic and into writes", "[passes][local]") {
  Fixture f;
  // x0 = 3; x1 = x0 + 5  ->  x1 = 8
  f.write(f.x0, f.constant(3));
  f.write(f.x1, f.function.binary(ExprOp::Add, f.read(f.x0), f.constant(5)));
  f.finish();

  // Nothing folds until copy propagation exposes the constant; then it fires.
  CHECK_FALSE(xdec::passes::foldConstants(f.function));
  CHECK(xdec::passes::copyPropagateBlock(f.function, f.block));
  CHECK(xdec::passes::foldConstants(f.function));
  CHECK(xdec::passes::dceBlock(f.function, f.block));
  REQUIRE(verifiesClean(f.function));

  // Both writes survive (live-out unknown); both carry constants now.
  const auto& ops = f.function.block(f.block).ops;
  REQUIRE(ops.size() == 3);
  uint64_t folded = 0;
  REQUIRE(f.function.asConstant(f.function.operands(f.function.op(ops[1]))[0], folded));
  CHECK(folded == 8);
}

TEST_CASE("subs followed by b.eq becomes a plain compare, flags and all",
          "[passes][local][flags]") {
  Fixture f;
  // nzcv = flagdef.sub(x0, x1); branch on eq.
  const ExprId a = f.read(f.x0);
  const ExprId b = f.read(f.x1);
  const il::ExprId defs[2] = {a, b};
  f.write(f.nzcv, f.function.flagDef(FlagOp::Sub, 64, defs));
  const ExprId flags = f.read(f.nzcv);
  const ExprId cond = f.function.flagCondition(flags, ConditionCode::Equal);
  f.function.appendCondBranch(f.block, 0x1100, cond, f.block, f.block);
  f.function.rebuildEdges();

  // The flagdef sits behind a value read, so the first fold finds no pattern;
  // copy propagation exposes it, and then the rewrite fires.
  CHECK_FALSE(xdec::passes::foldFlagConditions(f.function));
  CHECK(xdec::passes::copyPropagateBlock(f.function, f.block));
  CHECK(xdec::passes::foldFlagConditions(f.function));
  CHECK(xdec::passes::dceBlock(f.function, f.block));
  REQUIRE(verifiesClean(f.function));

  // The branch condition is now a plain compare; no flagcond survives. The
  // nzcv write itself stays: flags are live-out of the block, and killing
  // them is the global phase's call, not this sweep's.
  const auto& ops = f.function.block(f.block).ops;
  const il::Op& terminator = f.function.op(ops.back());
  const ExprId condition = f.function.operands(terminator)[0];
  CHECK(f.function.expr(condition).op == ExprOp::CmpEq);
  const std::string printed = xdec::il::print(f.function);
  CHECK(printed.find("flagcond") == std::string::npos);
}

TEST_CASE("tst-style logical flags fold to compares against zero",
          "[passes][local][flags]") {
  Fixture f;
  // nzcv = flagdef.logical(and(x0, 0xff)); condition ne.
  const ExprId masked = f.function.binary(ExprOp::And, f.read(f.x0), f.constant(0xff));
  const il::ExprId def[1] = {masked};
  f.write(f.nzcv, f.function.flagDef(FlagOp::Logical, 64, def));
  const ExprId cond =
      f.function.flagCondition(f.read(f.nzcv), ConditionCode::NotEqual);
  f.function.appendCondBranch(f.block, 0x1100, cond, f.block, f.block);
  f.function.rebuildEdges();

  [[maybe_unused]] const bool flagFolded = xdec::passes::foldFlagConditions(f.function);
  [[maybe_unused]] const bool propagated = xdec::passes::copyPropagateBlock(f.function, f.block);
  [[maybe_unused]] const bool flagFolded2 = xdec::passes::foldFlagConditions(f.function);
  REQUIRE(verifiesClean(f.function));

  const auto& ops = f.function.block(f.block).ops;
  const ExprId condition = f.function.operands(f.function.op(ops.back()))[0];
  CHECK(f.function.expr(condition).op == ExprOp::CmpNe);
}

TEST_CASE("a w-write then an x-read propagates through a zero extension",
          "[passes][local]") {
  Fixture f;
  // w0 = 0x1234; x1 = x0 + 1  ->  x1 = zext(0x1234) + 1 = 0x1235
  f.write(f.w0, f.function.constant(Type::integer(32), 0x1234));
  f.write(f.x1, f.function.binary(ExprOp::Add, f.read(f.x0), f.constant(1)));
  f.finish();

  [[maybe_unused]] const bool propagated = xdec::passes::copyPropagateBlock(f.function, f.block);
  [[maybe_unused]] const bool constFolded = xdec::passes::foldConstants(f.function);
  [[maybe_unused]] const bool dced = xdec::passes::dceBlock(f.function, f.block);
  REQUIRE(verifiesClean(f.function));

  const auto& ops = f.function.block(f.block).ops;
  // The x1 write carries a folded constant.
  uint64_t folded = 0;
  bool sawFoldedX1 = false;
  for (const il::OpId opId : ops) {
    const il::Op& op = f.function.op(opId);
    if (op.code == il::OpCode::WriteReg && op.reg() == f.x1) {
      const auto operands = f.function.operands(op);
      if (f.function.asConstant(operands[0], folded)) {
        sawFoldedX1 = true;
        CHECK(folded == 0x1235);
      }
    }
  }
  CHECK(sawFoldedX1);
}

TEST_CASE("dead overwrites die; a call between them keeps both alive",
          "[passes][local]") {
  {
    Fixture f;
    // x0 = 1; x0 = 2   ->  only the second survives.
    f.write(f.x0, f.constant(1));
    f.write(f.x0, f.constant(2));
    f.finish();
    CHECK(xdec::passes::dceBlock(f.function, f.block));
    REQUIRE(verifiesClean(f.function));
    const auto& ops = f.function.block(f.block).ops;
    CHECK(ops.size() == 2);  // one write + return
  }
  {
    Fixture f;
    // x0 = 1; call; x0 = 2   ->  the call may observe x0; both stay.
    f.write(f.x0, f.constant(1));
    f.function.appendCall(f.block, 0x1004, f.constant(0x8000));
    f.write(f.x0, f.constant(2));
    f.finish();
    CHECK_FALSE(xdec::passes::dceBlock(f.function, f.block));
  }
}

TEST_CASE("writes to xzr vanish and reads of it become zero", "[passes][local]") {
  Fixture f;
  // xzr = 7; x1 = xzr  ->  x1 = 0
  f.write(f.xzr, f.constant(7));
  f.write(f.x1, f.read(f.xzr));
  f.finish();

  [[maybe_unused]] const bool propagated = xdec::passes::copyPropagateBlock(f.function, f.block);
  [[maybe_unused]] const bool dced = xdec::passes::dceBlock(f.function, f.block);
  REQUIRE(verifiesClean(f.function));

  const std::string printed = xdec::il::print(f.function);
  CHECK(printed.find("xzr") == std::string::npos);
}

TEST_CASE("local-simplify reaches the same machine state as the lifted block",
          "[passes][local][oracle]") {
  Fixture f;
  // A block mixing everything: propagation, folding, flags, dead writes.
  //   x2 = x0 + x1
  //   w2 = trunc(and(x2, 0xff))   (also zero-extends x2: x2 = x2 & 0xff)
  //   nzcv = sub(x2, 0x10)
  //   x2 = select(eq, 0xaaaa, 0xbbbb)
  const ExprId sum = f.function.binary(ExprOp::Add, f.read(f.x0), f.read(f.x1));
  f.write(f.x2, sum);
  const ExprId masked = f.function.binary(ExprOp::And, f.read(f.x2), f.constant(0xff));
  f.write(f.function.registers().find("w2"),
          f.function.cast(ExprOp::Trunc, Type::integer(32), masked));
  const il::ExprId defs[2] = {f.read(f.x2), f.constant(0x10)};
  f.write(f.nzcv, f.function.flagDef(FlagOp::Sub, 64, defs));
  const ExprId cond = f.function.flagCondition(f.read(f.nzcv), ConditionCode::Equal);
  f.write(f.x2, f.function.select(cond, f.constant(0xaaaa), f.constant(0xbbbb)));
  f.finish();

  // The oracle: the simplified block, interpreted, must reproduce the
  // semantics worked out by hand. The w2 write zero-extends, so x2 is the
  // masked sum when the flags are taken.
  const auto expected = [](uint64_t a, uint64_t b) {
    const uint64_t low = (a + b) & 0xff;
    const bool eq = low == 0x10;
    return ConcreteValue{eq ? uint64_t{0xaaaa} : uint64_t{0xbbbb}, 0};
  };

  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);
  xdec::pass::Manager manager;
  auto ran = manager.runTo(f.function, registry, Maturity::Local);
  const std::string error = ran ? std::string{} : ran.error().format();
  INFO(error);
  REQUIRE(ran);
  REQUIRE(verifiesClean(f.function));

  for (const auto& [in0, in1] : {std::pair{5ull, 11ull}, {0x10ull, 0ull},
                                 {~0ull, 1ull}, {0ull, 0ull}, {7ull, 9ull}}) {
    il::Interpreter interp(f.function);
    interp.writeRegister(f.x0, ConcreteValue{in0, 0});
    interp.writeRegister(f.x1, ConcreteValue{in1, 0});
    const il::ExecOutcome outcome = interp.runBlock(f.function.entryBlock());
    REQUIRE(outcome.stop == il::ExecStop::Return);
    CHECK(interp.readRegister(f.x2) == expected(in0, in1));
  }
}

}  // namespace
