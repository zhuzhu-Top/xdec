// SSA construction: phi placement, renaming, and the verifier contract at
// ssa maturity.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "il/il_test_support.h"
#include "xdec/il/function.h"
#include "xdec/il/printer.h"
#include "xdec/il/verify.h"
#include "xdec/pass/manager.h"
#include "xdec/pass/registry.h"
#include "xdec/passes/ssa_construct.h"

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

  /// Marks the function ready for the SSA pass: well-formed at cfg, and the
  /// registry walk sees only ssa-construct (earlier levels are behind it).
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

void runSsa(Function& function) {
  xdec::pass::Registry registry;
  (void)registry.add(xdec::passes::makeSsaConstructPass());
  xdec::pass::Manager manager;
  auto ran = manager.runTo(function, registry, Maturity::Ssa);
  const std::string error = ran ? std::string{} : ran.error().format();
  INFO(error);
  REQUIRE(ran);
  REQUIRE(function.maturity() == Maturity::Ssa);
}

//   entry: cond = (x0 == 0); brc cond, left, right
//   left:  x0 = 1; br join
//   right: x0 = 2; br join
//   join:  store(x0 + x0); ret
TEST_CASE("a diamond merges the register with one phi", "[passes][ssa]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  const BlockId left = b.block(0x2000);
  const BlockId right = b.block(0x3000);
  const BlockId join = b.block(0x4000);

  const ExprId cond =
      b.function.binary(ExprOp::CmpEq, b.read(entry, b.reg("x0"), 0x1000), b.i64(0));
  b.function.appendCondBranch(entry, 0x1004, cond, left, right);
  b.function.appendWriteReg(left, 0x2000, b.reg("x0"), b.i64(1));
  b.function.appendBranch(left, 0x2004, join);
  b.function.appendWriteReg(right, 0x3000, b.reg("x0"), b.i64(2));
  b.function.appendBranch(right, 0x3004, join);
  const ExprId sum = b.function.binary(ExprOp::Add, b.read(join, b.reg("x0"), 0x4000),
                                       b.read(join, b.reg("x0"), 0x4004));
  b.function.appendStore(join, 0x4008, Type::integer(64), b.i64(0x9000), sum);
  b.function.appendReturn(join, 0x400c);

  b.atCfg();
  REQUIRE(verifiesCleanAt(b.function, Maturity::Cfg));
  runSsa(b.function);
  REQUIRE(verifiesCleanAt(b.function, Maturity::Ssa));

  // Exactly one phi, heading the join, typed i64, with the two arm constants
  // as inputs in predecessor order.
  const il::Block& joinBlock = b.function.block(join);
  REQUIRE(!joinBlock.ops.empty());
  const il::Op& phi = b.function.op(joinBlock.ops.front());
  REQUIRE(phi.code == OpCode::Phi);
  REQUIRE(phi.type == Type::integer(64));
  const auto incoming = b.function.operands(phi);
  REQUIRE(incoming.size() == 2);
  const auto& preds = joinBlock.predecessors;
  REQUIRE(preds.size() == 2);
  for (std::size_t i = 0; i < 2; ++i) {
    uint64_t value = 0;
    REQUIRE(b.function.asConstant(incoming[i], value));
    CHECK(value == (preds[i] == left ? 1 : 2));
  }

  // The store's data is the add, and the add reads the phi's value twice.
  const il::Op& store = b.function.op(joinBlock.ops[1]);
  REQUIRE(store.code == OpCode::Store);
  const ExprId sumExpr = b.function.operands(store)[1];
  const il::Expr& addExpr = b.function.expr(sumExpr);
  REQUIRE(addExpr.op == ExprOp::Add);
  for (uint32_t i = 0; i < 2; ++i) {
    const il::Expr& operand = b.function.expr(addExpr.operands[i]);
    CHECK(operand.op == ExprOp::Value);
    CHECK(operand.immediate == phi.result.index());
  }

  // The arms' writes are gone; only their branches remain.
  CHECK(b.function.block(left).ops.size() == 1);
  CHECK(b.function.block(right).ops.size() == 1);
}

//   entry: x0 = 0; br header
//   header: phi; cond = (x0 < 10); brc cond, body, exit
//   body: x0 = x0 + 1; br header
//   exit: ret
TEST_CASE("a loop's header phi closes over the latch's version", "[passes][ssa]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  const BlockId header = b.block(0x2000);
  const BlockId body = b.block(0x3000);
  const BlockId exit = b.block(0x4000);

  b.function.appendWriteReg(entry, 0x1000, b.reg("x0"), b.i64(0));
  b.function.appendBranch(entry, 0x1004, header);
  const ExprId cond =
      b.function.binary(ExprOp::CmpLtU, b.read(header, b.reg("x0"), 0x2000), b.i64(10));
  b.function.appendCondBranch(header, 0x2004, cond, body, exit);
  const ExprId next =
      b.function.binary(ExprOp::Add, b.read(body, b.reg("x0"), 0x3000), b.i64(1));
  b.function.appendWriteReg(body, 0x3004, b.reg("x0"), next);
  b.function.appendBranch(body, 0x3008, header);
  b.function.appendReturn(exit, 0x4000);

  b.atCfg();
  runSsa(b.function);
  REQUIRE(verifiesCleanAt(b.function, Maturity::Ssa));

  const il::Block& headerBlock = b.function.block(header);
  const il::Op& phi = b.function.op(headerBlock.ops.front());
  REQUIRE(phi.code == OpCode::Phi);
  const auto incoming = b.function.operands(phi);
  REQUIRE(incoming.size() == 2);

  // One input is the entry's zero; the other is the body's add, whose left
  // operand reads the phi itself — the loop closes.
  uint64_t fromEntry = 0;
  il::ExprId fromBody;
  for (std::size_t i = 0; i < 2; ++i) {
    if (b.function.asConstant(incoming[i], fromEntry)) {
      CHECK(fromEntry == 0);
    } else {
      fromBody = incoming[i];
    }
  }
  REQUIRE(fromBody.valid());
  REQUIRE(b.function.expr(fromBody).op == ExprOp::Add);
  const il::Expr& add = b.function.expr(fromBody);
  const il::Expr& addLhs = b.function.expr(add.operands[0]);
  CHECK(addLhs.op == ExprOp::Value);
  CHECK(addLhs.immediate == phi.result.index());

  // The body's condition reads the phi value as well.
  const il::Op& branch = b.function.op(headerBlock.ops.back());
  const il::Expr& condExpr = b.function.expr(b.function.operands(branch)[0]);
  const il::Expr& cmpLhs = b.function.expr(condExpr.operands[0]);
  CHECK(cmpLhs.op == ExprOp::Value);
  CHECK(cmpLhs.immediate == phi.result.index());
}

//   entry:  brc (x1 == 0), exit, header
//   header: x1 = x1 - 1; brc (the pre-decrement x1 != 1), header, exit
//   exit:   ret
//
// A countdown loop: the register is written in exactly one block, and that block
// is its own successor. Counting def-sites alone says one def needs no merge,
// which is only true because the value the function was entered with was not
// counted as a def of its own.
TEST_CASE("a register written only inside a loop still gets the header's phi",
          "[passes][ssa]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  const BlockId header = b.block(0x2000);
  const BlockId exit = b.block(0x3000);

  b.function.appendCondBranch(
      entry, 0x1000,
      b.function.binary(ExprOp::CmpEq, b.read(entry, b.reg("x1"), 0x1000), b.i64(0)),
      exit, header);
  const ExprId counter = b.read(header, b.reg("x1"), 0x2000);
  b.function.appendWriteReg(header, 0x2004, b.reg("x1"),
                            b.function.binary(ExprOp::Sub, counter, b.i64(1)));
  b.function.appendCondBranch(
      header, 0x2008, b.function.binary(ExprOp::CmpNe, counter, b.i64(1)), header, exit);
  b.function.appendReturn(exit, 0x3000);

  b.atCfg();
  runSsa(b.function);
  REQUIRE(verifiesCleanAt(b.function, Maturity::Ssa));

  const il::Block& headerBlock = b.function.block(header);
  const il::Op& phi = b.function.op(headerBlock.ops.front());
  REQUIRE(phi.code == OpCode::Phi);
  // The latch tests the counter the loop carries. Reading the entry value here
  // makes the test a constant and the loop something the code never does.
  const il::Op& branch = b.function.op(headerBlock.ops.back());
  const il::Expr& cond = b.function.expr(b.function.operands(branch)[0]);
  const il::Expr& tested = b.function.expr(cond.operands[0]);
  CHECK(tested.op == ExprOp::Value);
  CHECK(tested.immediate == phi.result.index());
}

TEST_CASE("an untouched register reads as its entry leaf", "[passes][ssa]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  // x5 was never written anywhere: its value is whatever the caller put there.
  b.function.appendStore(entry, 0x1000, Type::integer(64), b.i64(0x9000),
                         b.read(entry, b.reg("x5"), 0x1004));
  b.function.appendReturn(entry, 0x1008);

  b.atCfg();
  runSsa(b.function);
  REQUIRE(verifiesCleanAt(b.function, Maturity::Ssa));

  const il::Block& entryBlock = b.function.block(entry);
  const il::Op& store = b.function.op(entryBlock.ops.front());
  REQUIRE(store.code == OpCode::Store);
  const il::Expr& data = b.function.expr(b.function.operands(store)[1]);
  REQUIRE(data.op == ExprOp::EntryReg);
  CHECK(il::RegId{static_cast<uint32_t>(data.immediate)} == b.reg("x5"));
}

TEST_CASE("a call's result register reads as the call's value, not an unknown",
          "[passes][ssa]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  const BlockId after = b.block(0x2000);
  // call sub_2000 (a result-typed call, as the lifter emits); then store x0.
  b.function.appendCall(entry, 0x1000, b.i64(0x9000), Type::integer(64));
  b.function.appendBranch(entry, 0x1004, after);
  b.function.appendStore(after, 0x2000, Type::integer(64), b.i64(0xa000),
                         b.read(after, b.reg("x0"), 0x2004));
  b.function.appendReturn(after, 0x2008);

  b.atCfg();
  runSsa(b.function);
  REQUIRE(verifiesCleanAt(b.function, Maturity::Ssa));

  const il::Block& afterBlock = b.function.block(after);
  const il::Op& store = b.function.op(afterBlock.ops.front());
  const il::Expr& data = b.function.expr(b.function.operands(store)[1]);
  // x0 after the call is the call's defined value: readable, never undef.
  REQUIRE(data.op == ExprOp::Value);
  const il::ValueInfo& info = b.function.value(il::ValueId{static_cast<uint32_t>(data.immediate)});
  const il::Op& definer = b.function.op(info.definition);
  CHECK(definer.code == OpCode::Call);
}

TEST_CASE("a callee-saved register's version survives a call", "[passes][ssa]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  const BlockId after = b.block(0x2000);
  // x5 (inside x19..x28 conventions via the test file's general class? no —
  // the test register file models x0..x7 only, all caller-saved names; this
  // test therefore uses the name-pattern boundary: x5 is caller-saved and
  // must be clobbered, while the stack pointer's entry leaf never dies.
  b.function.appendCall(entry, 0x1000, b.i64(0x9000), Type::integer(64));
  b.function.appendBranch(entry, 0x1004, after);
  b.function.appendStore(after, 0x2000, Type::integer(64), b.i64(0xa000),
                         b.read(after, b.reg("sp"), 0x2004));
  b.function.appendStore(after, 0x2008, Type::integer(64), b.i64(0xa008),
                         b.read(after, b.reg("x5"), 0x200c));
  b.function.appendReturn(after, 0x2010);

  b.atCfg();
  runSsa(b.function);
  REQUIRE(verifiesCleanAt(b.function, Maturity::Ssa));

  const il::Block& afterBlock = b.function.block(after);
  const il::Op& spStore = b.function.op(afterBlock.ops.front());
  const il::Expr& spData = b.function.expr(b.function.operands(spStore)[1]);
  // sp is callee discipline: its entry leaf flows straight through the call.
  CHECK(spData.op == ExprOp::EntryReg);
  const il::Op& x5Store = b.function.op(afterBlock.ops[1]);
  const il::Expr& x5Data = b.function.expr(b.function.operands(x5Store)[1]);
  // x5 is caller-saved: unknown after the call.
  CHECK(x5Data.op == ExprOp::Undef);
}

//   entry: x1 = 7; brind x0    -- an exit whose meaning is not decided yet
//
// The snapshot the tail-call recovery stands on. An unresolved computed branch
// might be a jump inside this function or a call out of it, and by the time
// anything can tell, the writes that set up the arguments are dead: nothing in
// the IL reads x1 here. Recording the argument versions on the branch is what
// keeps them, and it claims nothing -- resolve-indirect drops them again as soon
// as the branch turns out to go somewhere in this function.
TEST_CASE("an unresolved indirect branch carries the argument registers",
          "[passes][ssa]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  b.function.appendWriteReg(entry, 0x1000, b.reg("x1"), b.i64(7));
  const il::OpId branch =
      b.function.appendIndirectBranch(entry, 0x1004, b.read(entry, b.reg("x0"), 0x1004));

  b.atCfg();
  runSsa(b.function);
  REQUIRE(verifiesCleanAt(b.function, Maturity::Ssa));

  const auto operands = b.function.operands(b.function.op(branch));
  // The target, then x0..x7 as the test register file names them.
  REQUIRE(operands.size() == 9);
  CHECK(b.function.expr(operands[0]).op == ExprOp::EntryReg);
  uint64_t written = 0;
  CHECK(b.function.asConstant(operands[2], written));
  CHECK(written == 7);
}

//   entry: brc.indirect target   (target is not a constant: stays unresolved)
//   orphan: t = read x1; store t; ret     -- reachable only through the
//     indirect branch above, which nothing has resolved into a real edge yet
TEST_CASE(
    "a block unreachable through an unresolved indirect branch keeps its "
    "register ops intact",
    "[passes][ssa]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  const BlockId orphan = b.block(0x5000);

  const ExprId target = b.read(entry, b.reg("x0"), 0x1000);
  b.function.appendIndirectBranch(entry, 0x1004, target);

  const il::ValueId x1Read = b.function.appendReadReg(orphan, 0x5000, b.reg("x1"));
  b.function.appendStore(orphan, 0x5004, Type::integer(64), b.i64(0x9000),
                         b.function.valueRef(x1Read));
  b.function.appendReturn(orphan, 0x5008);

  b.atCfg();
  runSsa(b.function);
  REQUIRE(verifiesCleanAt(b.function, Maturity::Ssa));

  // The read must still be the store's own value's definition: ssa-construct
  // must not tombstone a definition it never renamed, or the store's address
  // becomes a dangling reference nothing prints correctly.
  REQUIRE(b.function.hasValue(x1Read));
  CHECK(b.function.value(x1Read).definition.valid());
  const il::Block& orphanBlock = b.function.block(orphan);
  const il::Op& store = b.function.op(orphanBlock.ops[1]);
  REQUIRE(store.code == OpCode::Store);
  const il::Expr& data = b.function.expr(b.function.operands(store)[1]);
  REQUIRE(data.op == ExprOp::Value);
  CHECK(data.immediate == x1Read.index());
}

}  // namespace
