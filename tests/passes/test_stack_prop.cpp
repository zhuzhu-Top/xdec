// stack-prop: frame-address canonicalisation and store-to-load forwarding.
// Tests drive the whole pipeline to ssa — stack-prop is the last pass in it.
#include <catch2/catch_test_macros.hpp>

#include <string>

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

  RegId reg(std::string_view name) { return function.registers().find(name); }
  ExprId read(BlockId b, RegId r, uint64_t va) {
    return function.valueRef(function.appendReadReg(b, va, r));
  }
  ExprId i64(uint64_t value) { return function.constant(Type::integer(64), value); }

  /// sp -= 0x80, the prologue shape everything here needs.
  void prologue(BlockId b) {
    function.appendWriteReg(b, 0x1000, reg("sp"),
                            function.binary(ExprOp::Sub, read(b, reg("sp"), 0x1000), i64(0x80)));
  }

  ExprId spPlus(BlockId b, uint64_t offset, uint64_t va) {
    return function.binary(ExprOp::Add, read(b, reg("sp"), va), i64(offset));
  }

  void atCfg() {
    function.rebuildEdges();
    function.setMaturity(Maturity::Cfg);
  }

  Function function;
};

[[nodiscard]] bool verifiesCleanAt(const Function& function, Maturity level) {
  const il::VerifyReport report = il::verify(function, level);
  for (const xdec::Diag& diag : report.errors) {
    INFO(diag.format());
  }
  return report.ok();
}

void runToSsa(Function& function) {
  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);
  xdec::pass::Manager manager;
  auto ran = manager.runTo(function, registry, Maturity::Ssa);
  const std::string error = ran ? std::string{} : ran.error().format();
  INFO(error);
  REQUIRE(ran);
}

/// The address operand of a memory op, requiring the canonical
/// `add(entry(sp), delta)` shape; returns the delta.
[[nodiscard]] int64_t canonicalDelta(const Function& function, const il::Op& op) {
  const il::Expr& address = function.expr(function.operands(op)[0]);
  REQUIRE(address.op == ExprOp::Add);
  const il::Expr& base = function.expr(address.operands[0]);
  REQUIRE(base.op == ExprOp::EntryReg);
  uint64_t delta = 0;
  REQUIRE(function.asConstant(address.operands[1], delta));
  return static_cast<int64_t>(delta);
}

//   entry: sp -= 0x80; store x0 -> [sp+24]; x1 = load [sp+24]; store x1 -> [0x9000]; ret
TEST_CASE("a store followed by a matching load forwards the value", "[passes][stackprop]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  b.prologue(entry);
  b.function.appendStore(entry, 0x1004, Type::integer(64), b.spPlus(entry, 24, 0x1004),
                         b.read(entry, b.reg("x0"), 0x1004));
  const ExprId reloaded = b.function.valueRef(
      b.function.appendLoad(entry, 0x1008, Type::integer(64), b.spPlus(entry, 24, 0x1008)));
  b.function.appendStore(entry, 0x100c, Type::integer(64), b.i64(0x9000), reloaded);
  b.function.appendReturn(entry, 0x1010);

  b.atCfg();
  runToSsa(b.function);
  REQUIRE(verifiesCleanAt(b.function, Maturity::Ssa));

  const il::Block& block = b.function.block(entry);
  // Two stores and the return survive; the load is gone.
  REQUIRE(block.ops.size() == 3);
  const il::Op& spill = b.function.op(block.ops[0]);
  const il::Op& sink = b.function.op(block.ops[1]);
  REQUIRE(spill.code == OpCode::Store);
  REQUIRE(sink.code == OpCode::Store);

  // The spill address is canonical: entry(sp) - 0x80 + 24.
  CHECK(canonicalDelta(b.function, spill) == -0x68);
  // The sink stores what x0 held at entry — the reload folded into the spill.
  const il::Expr& data = b.function.expr(b.function.operands(sink)[1]);
  REQUIRE(data.op == ExprOp::EntryReg);
  CHECK(il::RegId{static_cast<uint32_t>(data.immediate)} == b.reg("x0"));
}

//   entry: sp -= 0x80; store:i64 x0 -> [sp+16]; w2 = load:i32 [sp+16]; store w2 -> [0x9000]
TEST_CASE("a narrower load forwards the low bits", "[passes][stackprop]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  b.prologue(entry);
  b.function.appendStore(entry, 0x1004, Type::integer(64), b.spPlus(entry, 16, 0x1004),
                         b.read(entry, b.reg("x0"), 0x1004));
  const ExprId narrowed = b.function.valueRef(
      b.function.appendLoad(entry, 0x1008, Type::integer(32), b.spPlus(entry, 16, 0x1008)));
  b.function.appendStore(entry, 0x100c, Type::integer(32), b.i64(0x9000), narrowed);
  b.function.appendReturn(entry, 0x1010);

  b.atCfg();
  runToSsa(b.function);
  REQUIRE(verifiesCleanAt(b.function, Maturity::Ssa));

  const il::Block& block = b.function.block(entry);
  REQUIRE(block.ops.size() == 3);
  const il::Op& sink = b.function.op(block.ops[1]);
  const il::Expr& data = b.function.expr(b.function.operands(sink)[1]);
  // trunc:i32(entry(x0))
  REQUIRE(data.op == ExprOp::Trunc);
  CHECK(data.type == Type::integer(32));
  const il::Expr& source = b.function.expr(data.operands[0]);
  CHECK(source.op == ExprOp::EntryReg);
}

//   entry: sp -= 0x80; store x0 -> [sp+24]; call f; x1 = load [sp+24]; store x1 -> [0x9000]
TEST_CASE("a call between store and load bars the forward", "[passes][stackprop]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  b.prologue(entry);
  b.function.appendStore(entry, 0x1004, Type::integer(64), b.spPlus(entry, 24, 0x1004),
                         b.read(entry, b.reg("x0"), 0x1004));
  b.function.appendCall(entry, 0x1008, b.i64(0x8000));
  const ExprId reloaded = b.function.valueRef(
      b.function.appendLoad(entry, 0x100c, Type::integer(64), b.spPlus(entry, 24, 0x100c)));
  b.function.appendStore(entry, 0x1010, Type::integer(64), b.i64(0x9000), reloaded);
  b.function.appendReturn(entry, 0x1014);

  b.atCfg();
  runToSsa(b.function);
  REQUIRE(verifiesCleanAt(b.function, Maturity::Ssa));

  // The load survives: the callee may have written the frame.
  bool sawLoad = false;
  for (const il::OpId opId : b.function.block(entry).ops) {
    sawLoad = sawLoad || b.function.op(opId).code == OpCode::Load;
  }
  CHECK(sawLoad);
}

//   entry: sp -= 0x80; store x0 -> [sp+24]; store x1 -> [x2]; x3 = load [sp+24]; ...
TEST_CASE("a store through an unknown pointer bars the forward", "[passes][stackprop]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  b.prologue(entry);
  b.function.appendStore(entry, 0x1004, Type::integer(64), b.spPlus(entry, 24, 0x1004),
                         b.read(entry, b.reg("x0"), 0x1004));
  b.function.appendStore(entry, 0x1008, Type::integer(64),
                         b.read(entry, b.reg("x2"), 0x1008),
                         b.read(entry, b.reg("x1"), 0x1008));
  const ExprId reloaded = b.function.valueRef(
      b.function.appendLoad(entry, 0x100c, Type::integer(64), b.spPlus(entry, 24, 0x100c)));
  b.function.appendStore(entry, 0x1010, Type::integer(64), b.i64(0x9000), reloaded);
  b.function.appendReturn(entry, 0x1014);

  b.atCfg();
  runToSsa(b.function);
  REQUIRE(verifiesCleanAt(b.function, Maturity::Ssa));

  bool sawLoad = false;
  for (const il::OpId opId : b.function.block(entry).ops) {
    sawLoad = sawLoad || b.function.op(opId).code == OpCode::Load;
  }
  CHECK(sawLoad);
}

//   entry: a = load [0x30c420]; b = load [0x30c420]; store (a+b) -> [0x9000]
TEST_CASE("a reload of an untouched global is the first load's value", "[passes][stackprop]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  const ExprId first =
      b.function.valueRef(b.function.appendLoad(entry, 0x1000, Type::integer(64), b.i64(0x30c420)));
  const ExprId second =
      b.function.valueRef(b.function.appendLoad(entry, 0x1004, Type::integer(64), b.i64(0x30c420)));
  b.function.appendStore(entry, 0x1008, Type::integer(64), b.i64(0x9000),
                         b.function.binary(ExprOp::Add, first, second));
  b.function.appendReturn(entry, 0x100c);

  b.atCfg();
  runToSsa(b.function);
  REQUIRE(verifiesCleanAt(b.function, Maturity::Ssa));

  const il::Block& block = b.function.block(entry);
  // One load, the sink, the return: the reload folded into the first value.
  REQUIRE(block.ops.size() == 3);
  const il::Op& sink = b.function.op(block.ops[1]);
  const il::Expr& sum = b.function.expr(b.function.operands(sink)[1]);
  REQUIRE(sum.op == ExprOp::Add);
  // Both operands are the surviving load's value.
  const il::Expr& lhs = b.function.expr(sum.operands[0]);
  const il::Expr& rhs = b.function.expr(sum.operands[1]);
  CHECK(lhs.op == ExprOp::Value);
  CHECK(lhs == rhs);
  CHECK(b.function.op(block.ops[0]).code == OpCode::Load);
}

//   entry: a = load [0x30c420]; x2 = a; branch mid
//   mid:   b = load [0x30c420]; store (x2+b) -> [0x9000]
//
// `a` is carried across the branch through a register write/read (not a raw
// shared ExprId), matching how a value actually crosses a block boundary
// before SSA construction turns the register into a value the pass can see
// is the same one.
TEST_CASE("a reload of an untouched global from a different block is the first load's value",
          "[passes][stackprop]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  const BlockId mid = b.block(0x1008);
  const ExprId first =
      b.function.valueRef(b.function.appendLoad(entry, 0x1000, Type::integer(64), b.i64(0x30c420)));
  b.function.appendWriteReg(entry, 0x1004, b.reg("x2"), first);
  b.function.appendBranch(entry, 0x1008, mid);
  const ExprId firstInMid = b.read(mid, b.reg("x2"), 0x1008);
  const ExprId second =
      b.function.valueRef(b.function.appendLoad(mid, 0x100c, Type::integer(64), b.i64(0x30c420)));
  b.function.appendStore(mid, 0x1010, Type::integer(64), b.i64(0x9000),
                         b.function.binary(ExprOp::Add, firstInMid, second));
  b.function.appendReturn(mid, 0x1014);

  b.atCfg();
  runToSsa(b.function);
  REQUIRE(verifiesCleanAt(b.function, Maturity::Ssa));

  // The second block's load is gone: its value is the entry block's load.
  bool sawLoad = false;
  for (const il::OpId opId : b.function.block(mid).ops) {
    sawLoad = sawLoad || b.function.op(opId).code == OpCode::Load;
  }
  CHECK(!sawLoad);
  const il::Op& sink = b.function.op(b.function.block(mid).ops[0]);
  const il::Expr& sum = b.function.expr(b.function.operands(sink)[1]);
  REQUIRE(sum.op == ExprOp::Add);
  CHECK(sum.operands[0] == sum.operands[1]);
}

//   entry: a = load [0x30c420]; x2 = a; call f; branch mid
//   mid:   b = load [0x30c420]; store (x2+b) -> [0x9000]
TEST_CASE("a call anywhere in the function bars cross-block global reuse",
          "[passes][stackprop]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  const BlockId mid = b.block(0x100c);
  const ExprId first =
      b.function.valueRef(b.function.appendLoad(entry, 0x1000, Type::integer(64), b.i64(0x30c420)));
  b.function.appendWriteReg(entry, 0x1004, b.reg("x2"), first);
  b.function.appendCall(entry, 0x1008, b.i64(0x8000));
  b.function.appendBranch(entry, 0x100c, mid);
  const ExprId firstInMid = b.read(mid, b.reg("x2"), 0x100c);
  const ExprId second =
      b.function.valueRef(b.function.appendLoad(mid, 0x1010, Type::integer(64), b.i64(0x30c420)));
  b.function.appendStore(mid, 0x1014, Type::integer(64), b.i64(0x9000),
                         b.function.binary(ExprOp::Add, firstInMid, second));
  b.function.appendReturn(mid, 0x1018);

  b.atCfg();
  runToSsa(b.function);
  REQUIRE(verifiesCleanAt(b.function, Maturity::Ssa));

  // The call may have written the global: the second block's load survives.
  bool sawLoad = false;
  for (const il::OpId opId : b.function.block(mid).ops) {
    sawLoad = sawLoad || b.function.op(opId).code == OpCode::Load;
  }
  CHECK(sawLoad);
}

//   entry: a = load [0x30c420]; x2 = a; condbranch (x0 != 0) ? mid : other
//   mid:   b = load [0x30c420]; store (x2+b) -> [0x9000]; ret
//   other: store x1 -> [0x30c420]; ret
//
// The store never actually reaches `mid` on any real path, but the analysis
// does not do path-sensitive reasoning -- it is a whole-function precondition
// by design, the same way a call anywhere bars it. Proving that is the point:
// a smarter, path-aware version is a possible future extension, not a bug in
// this one.
TEST_CASE("a store to the global anywhere in the function bars cross-block reuse",
          "[passes][stackprop]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  const BlockId mid = b.block(0x100c);
  const BlockId other = b.block(0x1010);
  const ExprId first =
      b.function.valueRef(b.function.appendLoad(entry, 0x1000, Type::integer(64), b.i64(0x30c420)));
  b.function.appendWriteReg(entry, 0x1004, b.reg("x2"), first);
  const ExprId cond = b.function.binary(ExprOp::CmpNe, b.read(entry, b.reg("x0"), 0x1008),
                                        b.i64(0));
  b.function.appendCondBranch(entry, 0x1008, cond, mid, other);
  const ExprId firstInMid = b.read(mid, b.reg("x2"), 0x100c);
  const ExprId second =
      b.function.valueRef(b.function.appendLoad(mid, 0x100c, Type::integer(64), b.i64(0x30c420)));
  b.function.appendStore(mid, 0x1010, Type::integer(64), b.i64(0x9000),
                         b.function.binary(ExprOp::Add, firstInMid, second));
  b.function.appendReturn(mid, 0x1014);
  b.function.appendStore(other, 0x1010, Type::integer(64), b.i64(0x30c420),
                         b.read(other, b.reg("x1"), 0x1010));
  b.function.appendReturn(other, 0x1014);

  b.atCfg();
  runToSsa(b.function);
  REQUIRE(verifiesCleanAt(b.function, Maturity::Ssa));

  // The store may reach `mid` for all the analysis knows: the load survives.
  bool sawLoad = false;
  for (const il::OpId opId : b.function.block(mid).ops) {
    sawLoad = sawLoad || b.function.op(opId).code == OpCode::Load;
  }
  CHECK(sawLoad);
}

}  // namespace
