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

}  // namespace
