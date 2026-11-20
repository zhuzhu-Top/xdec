// vars: how many arguments a call really passes, recovered from the caller's
// side. The interesting inputs are all about what put a value in an argument
// register -- so each case here differs only in that.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "il/il_test_support.h"
#include "xdec/il/function.h"
#include "xdec/il/verify.h"
#include "xdec/pass/manager.h"
#include "xdec/pass/registry.h"
#include "xdec/passes/builtin.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::Maturity;
using xdec::il::OpCode;
using xdec::il::OpId;
using xdec::il::RegId;
using xdec::il::Type;

namespace {

struct Builder {
  Builder() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {}

  BlockId block(uint64_t va) {
    const BlockId id = function.createBlock(va);
    if (!function.entryBlock().valid()) {
      function.setEntryBlock(id);
    }
    return id;
  }

  [[nodiscard]] RegId reg(const char* name) const {
    const RegId id = function.registers().find(name);
    REQUIRE(id.valid());
    return id;
  }

  ExprId i64(uint64_t value) { return function.constant(Type::integer(64), value); }

  void set(BlockId at, uint64_t va, const char* name, uint64_t value) {
    function.appendWriteReg(at, va, reg(name), i64(value));
  }

  void atCfg() {
    function.rebuildEdges();
    function.setMaturity(Maturity::Cfg);
  }

  Function function;
};

/// The whole stock pipeline, up to and including vars. No image is wired: the
/// only call target these tests use is a constant, so nothing needs reading.
void runToVars(Function& function, xdec::pass::NameAt names = {}) {
  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);
  xdec::pass::Manager manager;
  manager.setImage([](uint64_t, std::span<std::byte>) -> xdec::Result<void> {
    return xdec::err(xdec::DiagCode::Internal, "no image in this test");
  });
  if (names) {
    manager.setNames(std::move(names));
  }
  auto ran = manager.runTo(function, registry, Maturity::Vars);
  const std::string error = ran ? std::string{} : ran.error().format();
  INFO(error);
  REQUIRE(ran);
  const il::VerifyReport report = il::verify(function, Maturity::Vars);
  for (const xdec::Diag& diag : report.errors) {
    INFO(diag.format());
  }
  REQUIRE(report.ok());
}

[[nodiscard]] OpId theCall(const Function& function) {
  for (const BlockId blockId : function.blockHandles()) {
    for (const OpId opId : function.block(blockId).ops) {
      if (function.op(opId).code == OpCode::Call) {
        return opId;
      }
    }
  }
  return OpId::invalid();
}

/// Argument slots the call still carries, target excluded.
[[nodiscard]] std::size_t arity(const Function& function) {
  const OpId call = theCall(function);
  REQUIRE(call.valid());
  return function.operands(function.op(call)).size() - 1;
}

}  // namespace

// Nothing wrote any argument register and no call came first, so every slot
// holds whatever this function was entered with. With no slot written there is
// no argument list of the caller's own to measure against: forwarding all of
// them and forwarding none look the same from here, so nothing is trimmed.
TEST_CASE("a call with no argument registers written keeps its entry values",
          "[passes][vars]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  b.function.appendCall(entry, 0x1000, b.i64(0x2000), Type::integer(64));
  b.function.appendReturn(entry, 0x1004);
  b.atCfg();

  runToVars(b.function);

  CHECK(arity(b.function) == 8);
}

// The adid MIG shape: two slots set up, the next one untouched. The untouched
// x7 is not an argument of the callee, and so not a read of this function's.
TEST_CASE("an untouched slot above the set-up ones is trimmed", "[passes][vars]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  b.set(entry, 0x1000, "x0", 1);
  b.set(entry, 0x1004, "x6", 2);
  b.function.appendCall(entry, 0x1008, b.i64(0x2000), Type::integer(64));
  b.function.appendReturn(entry, 0x100c);
  b.atCfg();

  runToVars(b.function);

  CHECK(arity(b.function) == 7);
}

// An untouched register the body also reads is one of this function's own
// parameters, and forwarding it is plausible: the slot stays.
TEST_CASE("an untouched slot the body reads elsewhere is kept", "[passes][vars]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  const il::ValueId x2 = b.function.appendReadReg(entry, 0x1000, b.reg("x2"));
  b.function.appendStore(entry, 0x1004, Type::integer(64), b.i64(0x8000),
                         b.function.valueRef(x2));
  b.set(entry, 0x1008, "x0", 1);
  b.function.appendCall(entry, 0x100c, b.i64(0x2000), Type::integer(64));
  b.function.appendReturn(entry, 0x1010);
  b.atCfg();

  runToVars(b.function);

  CHECK(arity(b.function) == 3);
}

// A stack probe in the prologue keeps every register but x16/x17 (see
// analysis/call_abi.h). Modelled as an ordinary call it would leave x0 Undef
// at the real call after it; here x0 is still the entry value, and the probe
// itself carries no arguments.
TEST_CASE("a stack-probe helper destroys no argument register", "[passes][vars]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  const OpId probe = b.function.appendCall(entry, 0x1000, b.i64(0x2000), Type::integer(64));
  b.set(entry, 0x1004, "x1", 5);
  const OpId call = b.function.appendCall(entry, 0x1008, b.i64(0x3000), Type::integer(64));
  b.function.appendReturn(entry, 0x100c);
  b.atCfg();

  runToVars(b.function, [](uint64_t va) {
    return va == 0x2000 ? xdec::pass::SymbolName{"___chkstk_darwin", true}
                        : xdec::pass::SymbolName{};
  });

  CHECK(b.function.operands(b.function.op(probe)).size() == 1);
  const auto operands = b.function.operands(b.function.op(call));
  REQUIRE(operands.size() - 1 == 2);
  CHECK(b.function.expr(operands[1]).op == ExprOp::EntryReg);
}

// The case the pass exists for: a call whose upper argument slots are a previous
// call's leftovers. x1..x7 are Undef and go.
//
// x0 does not, and that is not an oversight: after a call, the result register
// holds the value the callee returned, so `g(f())` and a stale x0 are the same
// shape from the caller's side. The trim keeps the slot rather than deciding
// which of the two it is looking at.
TEST_CASE("argument registers left clobbered by an earlier call are not arguments",
          "[passes][vars]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  b.function.appendCall(entry, 0x1000, b.i64(0x2000), Type::integer(64));
  const OpId second = b.function.appendCall(entry, 0x1004, b.i64(0x3000), Type::integer(64));
  b.function.appendReturn(entry, 0x1008);
  b.atCfg();

  runToVars(b.function);

  CHECK(b.function.operands(b.function.op(second)).size() - 1 == 1);
  CHECK(b.function.noteOn(second).empty());
}

// Two arguments set up after a clobbering call: the recovered arity is two, and
// the six slots above them go.
TEST_CASE("arity is the last argument register the caller set up", "[passes][vars]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  b.function.appendCall(entry, 0x1000, b.i64(0x2000), Type::integer(64));
  b.set(entry, 0x1004, "x0", 7);
  b.set(entry, 0x1008, "x1", 9);
  const OpId second = b.function.appendCall(entry, 0x100c, b.i64(0x3000), Type::integer(64));
  b.function.appendReturn(entry, 0x1010);
  b.atCfg();

  runToVars(b.function);

  CHECK(b.function.operands(b.function.op(second)).size() - 1 == 2);
}

// A gap: x2 is set up, x1 is a previous call's clobber. Positions belong to the
// convention, so the slot stays -- and the call says why it is there, because a
// caller that sets up argument three but not argument two is either passing a
// stale register or has lost a definition somewhere.
TEST_CASE("a gap in the argument registers is kept and reported", "[passes][vars]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  b.function.appendCall(entry, 0x1000, b.i64(0x2000), Type::integer(64));
  b.set(entry, 0x1004, "x2", 7);
  const OpId second = b.function.appendCall(entry, 0x1008, b.i64(0x3000), Type::integer(64));
  b.function.appendReturn(entry, 0x100c);
  b.atCfg();

  runToVars(b.function);

  CHECK(b.function.operands(b.function.op(second)).size() - 1 == 3);
  CHECK(std::string{b.function.noteOn(second)} ==
        "1 argument slot(s) hold values this function never wrote: either the callee "
        "reads a stale register or a definition was lost here");
}

// Recovery is per call, not per function: two calls in a row with different
// argument setups get different arities.
TEST_CASE("each call recovers its own arity", "[passes][vars]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  b.function.appendCall(entry, 0x1000, b.i64(0x2000), Type::integer(64));
  b.set(entry, 0x1004, "x0", 1);
  const OpId one = b.function.appendCall(entry, 0x1008, b.i64(0x3000), Type::integer(64));
  b.set(entry, 0x100c, "x1", 2);
  b.set(entry, 0x1010, "x2", 3);
  const OpId two = b.function.appendCall(entry, 0x1014, b.i64(0x4000), Type::integer(64));
  b.function.appendReturn(entry, 0x1018);
  b.atCfg();

  runToVars(b.function);

  // The first call sees x0 alone; x1..x7 are still the earlier call's clobber.
  CHECK(b.function.operands(b.function.op(one)).size() - 1 == 1);
  // The second sees x0 (still live from before), x1 and x2.
  CHECK(b.function.operands(b.function.op(two)).size() - 1 == 3);
}

// A call with only its target and no attached arguments at all -- what a
// pipeline that never ran ssa-construct's annotation would produce. Nothing to
// recover, and nothing to break.
TEST_CASE("a call with no attached arguments is left alone", "[passes][vars]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  const OpId call = b.function.appendCall(entry, 0x1000, b.i64(0x2000));
  b.function.appendReturn(entry, 0x1004);
  b.atCfg();

  runToVars(b.function);

  // The call defines no value, so SSA construction attaches the argument
  // registers but no result: the arity recovery still applies to what is there.
  CHECK(b.function.op(call).code == OpCode::Call);
  CHECK(b.function.noteOn(call).empty());
}

TEST_CASE("outgoing stack arguments past the 8 register slots attach to the call",
          "[passes][vars]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  const ExprId readSp0 =
      b.function.valueRef(b.function.appendReadReg(entry, 0x1000, b.reg("sp")));
  const ExprId newSp = b.function.binary(ExprOp::Sub, readSp0, b.i64(0x40));
  b.function.appendWriteReg(entry, 0x1000, b.reg("sp"), newSp);
  for (unsigned idx = 0; idx < 8; ++idx) {
    const std::string regName = "x" + std::to_string(idx);
    b.set(entry, 0x1004 + idx * 4, regName.c_str(), idx + 10);
  }
  const ExprId spNow =
      b.function.valueRef(b.function.appendReadReg(entry, 0x1024, b.reg("sp")));
  b.function.appendStore(entry, 0x1024, Type::integer(64), spNow, b.i64(0xaaa));
  const ExprId spNext =
      b.function.valueRef(b.function.appendReadReg(entry, 0x1028, b.reg("sp")));
  b.function.appendStore(entry, 0x1028, Type::integer(64),
                         b.function.binary(ExprOp::Add, spNext, b.i64(8)),
                         b.i64(0xbbb));
  const OpId call = b.function.appendCall(entry, 0x102c, b.i64(0x9000));
  b.function.appendReturn(entry, 0x1030);
  b.atCfg();

  runToVars(b.function);

  const auto ops = b.function.operands(b.function.op(call));
  REQUIRE(ops.size() == 11);  // target + x0..x7 + [sp+0] + [sp+8]
  uint64_t stack0 = 0;
  uint64_t stack1 = 0;
  REQUIRE(b.function.asConstant(ops[9], stack0));
  REQUIRE(b.function.asConstant(ops[10], stack1));
  CHECK(stack0 == 0xaaa);
  CHECK(stack1 == 0xbbb);
}
