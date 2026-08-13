// The Ssa-level fixpoint: SCCP, branch folding, phi simplification, and
// demand-driven DCE. These tests build Cfg-level functions and run the whole
// pipeline to ssa — construction feeds optimisation, which is how production
// drives it too.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "il/il_test_support.h"
#include "xdec/il/function.h"
#include "xdec/il/printer.h"
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

/// The full pipeline to ssa: local-simplify, cfg-finalize, ssa-construct and
/// the ssa-optimize fixpoint, with verification between every pass.
void runToSsa(Function& function) {
  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);
  xdec::pass::Manager manager;
  auto ran = manager.runTo(function, registry, Maturity::Ssa);
  const std::string error = ran ? std::string{} : ran.error().format();
  INFO(error);
  REQUIRE(ran);
  REQUIRE(function.maturity() == Maturity::Ssa);
}

[[nodiscard]] std::size_t phiCount(const Function& function, BlockId block) {
  std::size_t count = 0;
  for (const il::OpId opId : function.block(block).ops) {
    if (function.op(opId).code != OpCode::Phi) {
      break;
    }
    ++count;
  }
  return count;
}

//   entry: cond = (x0 == 0); brc cond, left, right   (x0 unknown: both arms live)
//   left:  x1 = 7; br join
//   right: x1 = 7; br join
//   join:  store(x1); ret
TEST_CASE("a phi of one constant folds away entirely", "[passes][ssaopt]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  const BlockId left = b.block(0x2000);
  const BlockId right = b.block(0x3000);
  const BlockId join = b.block(0x4000);

  const ExprId cond =
      b.function.binary(ExprOp::CmpEq, b.read(entry, b.reg("x0"), 0x1000), b.i64(0));
  b.function.appendCondBranch(entry, 0x1004, cond, left, right);
  b.function.appendWriteReg(left, 0x2000, b.reg("x1"), b.i64(7));
  b.function.appendBranch(left, 0x2004, join);
  b.function.appendWriteReg(right, 0x3000, b.reg("x1"), b.i64(7));
  b.function.appendBranch(right, 0x3004, join);
  b.function.appendStore(join, 0x4000, Type::integer(64), b.i64(0x9000),
                         b.read(join, b.reg("x1"), 0x4004));
  b.function.appendReturn(join, 0x4008);

  b.atCfg();
  runToSsa(b.function);
  REQUIRE(verifiesCleanAt(b.function, Maturity::Ssa));

  CHECK(phiCount(b.function, join) == 0);
  const il::Op& store = b.function.op(b.function.block(join).ops.front());
  REQUIRE(store.code == OpCode::Store);
  uint64_t value = 0;
  REQUIRE(b.function.asConstant(b.function.operands(store)[1], value));
  CHECK(value == 7);
}

//   entry: x0 = 1; brc (x0 == 1), left, right   (constant: folds to br left)
//   left:  x1 = 2; br join
//   right: x1 = 3; br join
//   join:  store(x1); ret
TEST_CASE("a constant condition folds the branch and prunes the phi", "[passes][ssaopt]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  const BlockId left = b.block(0x2000);
  const BlockId right = b.block(0x3000);
  const BlockId join = b.block(0x4000);

  b.function.appendWriteReg(entry, 0x1000, b.reg("x0"), b.i64(1));
  const ExprId cond =
      b.function.binary(ExprOp::CmpEq, b.read(entry, b.reg("x0"), 0x1004), b.i64(1));
  b.function.appendCondBranch(entry, 0x1008, cond, left, right);
  b.function.appendWriteReg(left, 0x2000, b.reg("x1"), b.i64(2));
  b.function.appendBranch(left, 0x2004, join);
  b.function.appendWriteReg(right, 0x3000, b.reg("x1"), b.i64(3));
  b.function.appendBranch(right, 0x3004, join);
  b.function.appendStore(join, 0x4000, Type::integer(64), b.i64(0x9000),
                         b.read(join, b.reg("x1"), 0x4004));
  b.function.appendReturn(join, 0x4008);

  b.atCfg();
  runToSsa(b.function);
  REQUIRE(verifiesCleanAt(b.function, Maturity::Ssa));

  // The branch folded to its true arm.
  const il::Op& terminator = b.function.op(b.function.block(entry).terminator());
  REQUIRE(terminator.code == OpCode::Branch);
  CHECK(b.function.targets(terminator)[0] == left);

  // The join's phi collapsed to the surviving arm's constant.
  CHECK(phiCount(b.function, join) == 0);
  const il::Op& store = b.function.op(b.function.block(join).ops.front());
  uint64_t value = 0;
  REQUIRE(b.function.asConstant(b.function.operands(store)[1], value));
  CHECK(value == 2);
}

//   entry: call f; brc (x0 == 0), left, right   (call-clobbered: must not fold)
TEST_CASE("a call-clobbered condition is honest: no fold", "[passes][ssaopt]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  const BlockId left = b.block(0x2000);
  const BlockId right = b.block(0x3000);

  b.function.appendCall(entry, 0x1000, b.i64(0x8000));
  const ExprId cond =
      b.function.binary(ExprOp::CmpEq, b.read(entry, b.reg("x0"), 0x1004), b.i64(0));
  b.function.appendCondBranch(entry, 0x1008, cond, left, right);
  b.function.appendReturn(left, 0x2000);
  b.function.appendReturn(right, 0x3000);

  b.atCfg();
  runToSsa(b.function);
  REQUIRE(verifiesCleanAt(b.function, Maturity::Ssa));

  const il::Op& terminator = b.function.op(b.function.block(entry).terminator());
  CHECK(terminator.code == OpCode::CondBranch);
}

//   entry: x2 = 0; br header
//   header: x2 = phi(0, x2+1); brc (x1 == 0), body, exit   (x1 unknown)
//   body:  x2 = x2 + 1; br header
//   exit:  store(9); ret                                   (x2 never observed)
//
// The counter is x2 rather than x0 deliberately: `ret` carries the result
// register's version as the returned value, so a cycle through x0 has an
// observer -- the return -- and is not dead at all.
TEST_CASE("an unobserved phi cycle is dead, however busy it looks", "[passes][ssaopt]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  const BlockId header = b.block(0x2000);
  const BlockId body = b.block(0x3000);
  const BlockId exit = b.block(0x4000);

  b.function.appendWriteReg(entry, 0x1000, b.reg("x2"), b.i64(0));
  b.function.appendBranch(entry, 0x1004, header);
  const ExprId cond =
      b.function.binary(ExprOp::CmpEq, b.read(header, b.reg("x1"), 0x2000), b.i64(0));
  b.function.appendCondBranch(header, 0x2004, cond, body, exit);
  const ExprId next =
      b.function.binary(ExprOp::Add, b.read(body, b.reg("x2"), 0x3000), b.i64(1));
  b.function.appendWriteReg(body, 0x3004, b.reg("x2"), next);
  b.function.appendBranch(body, 0x3008, header);
  b.function.appendStore(exit, 0x4000, Type::integer(64), b.i64(0x9000), b.i64(9));
  b.function.appendReturn(exit, 0x4004);

  b.atCfg();
  runToSsa(b.function);
  REQUIRE(verifiesCleanAt(b.function, Maturity::Ssa));

  CHECK(phiCount(b.function, header) == 0);
  // The body's increment had no observer either; the block is down to its edge.
  const il::Block& bodyBlock = b.function.block(body);
  CHECK(bodyBlock.ops.size() == 1);
  CHECK(b.function.op(bodyBlock.ops.front()).code == OpCode::Branch);
}

// A dispatcher: one indirect branch to many state blocks, each assigning a
// different constant and branching back to a common join. The join's phi
// therefore has one operand per state, and its cell must come out overdefined --
// the constants disagree.
//
// The shape is the point. SCCP learns the join's incoming edges one at a time,
// and meets each into the phi as it arrives rather than re-merging every
// predecessor, so a phi this wide is where an incremental merge would differ from
// a full one if it were wrong. It is also where the cost of the full merge lived:
// quadratic in predecessors, which for a flattened function means quadratic in
// the number of states it was flattened into.
TEST_CASE("a phi over many dispatcher predecessors merges to overdefined",
          "[passes][ssaopt]") {
  static constexpr unsigned kStates = 64;
  Builder b;
  const BlockId entry = b.block(0x1000);
  const BlockId join = b.block(0x2000);

  std::vector<BlockId> states;
  for (unsigned state = 0; state < kStates; ++state) {
    const BlockId at = b.block(0x3000 + state * 0x10);
    b.function.appendWriteReg(at, 0x3000 + state * 0x10, b.reg("x1"), b.i64(state));
    b.function.appendBranch(at, 0x3004 + state * 0x10, join);
    states.push_back(at);
  }
  const il::OpId dispatch =
      b.function.appendIndirectBranch(entry, 0x1000, b.read(entry, b.reg("x0"), 0x1004));
  b.function.setTargets(dispatch, states);
  b.function.appendStore(join, 0x2000, Type::integer(64), b.i64(0x9000),
                         b.read(join, b.reg("x1"), 0x2004));
  b.function.appendReturn(join, 0x2008);

  b.atCfg();
  runToSsa(b.function);
  REQUIRE(verifiesCleanAt(b.function, Maturity::Ssa));

  // The phi survives with an operand per state, and the stored value is that phi
  // rather than any one state's constant.
  CHECK(phiCount(b.function, join) == 1);
  const il::OpId phiId = b.function.block(join).ops.front();
  CHECK(b.function.operands(b.function.op(phiId)).size() == kStates);
  const il::OpId storeId = b.function.block(join).ops[1];
  const il::Op& store = b.function.op(storeId);
  REQUIRE(store.code == OpCode::Store);
  uint64_t folded = 0;
  CHECK_FALSE(b.function.asConstant(b.function.operands(store)[1], folded));
}

// The same dispatcher, but every state assigns the *same* constant, so the wide
// phi does collapse and the store sees the constant. Paired with the test above
// this pins the incremental merge from both sides: it must not lose a
// disagreement, and it must not invent one.
TEST_CASE("a wide phi of one repeated constant still folds", "[passes][ssaopt]") {
  static constexpr unsigned kStates = 64;
  Builder b;
  const BlockId entry = b.block(0x1000);
  const BlockId join = b.block(0x2000);

  std::vector<BlockId> states;
  for (unsigned state = 0; state < kStates; ++state) {
    const BlockId at = b.block(0x3000 + state * 0x10);
    b.function.appendWriteReg(at, 0x3000 + state * 0x10, b.reg("x1"), b.i64(0x2a));
    b.function.appendBranch(at, 0x3004 + state * 0x10, join);
    states.push_back(at);
  }
  const il::OpId dispatch =
      b.function.appendIndirectBranch(entry, 0x1000, b.read(entry, b.reg("x0"), 0x1004));
  b.function.setTargets(dispatch, states);
  b.function.appendStore(join, 0x2000, Type::integer(64), b.i64(0x9000),
                         b.read(join, b.reg("x1"), 0x2004));
  b.function.appendReturn(join, 0x2008);

  b.atCfg();
  runToSsa(b.function);
  REQUIRE(verifiesCleanAt(b.function, Maturity::Ssa));

  CHECK(phiCount(b.function, join) == 0);
  const il::Op& store = b.function.op(b.function.block(join).ops.front());
  REQUIRE(store.code == OpCode::Store);
  uint64_t value = 0;
  REQUIRE(b.function.asConstant(b.function.operands(store)[1], value));
  CHECK(value == 0x2a);
}

//   entry: brc (x0 == 0), left, right             (x0 unknown: both arms live)
//   left:  nzcv = flagdef.sub(x1, x2); br join
//   right: nzcv = flagdef.sub(x3, x4); br join    (a different comparison)
//   join:  cond = flagcond(nzcv, eq); brc cond, whenTrue, whenFalse
//
// The two arms set flags from unrelated operands, so the phi at join cannot
// collapse to one shared FlagDef the way a single-definition cross-block flag
// already does (see SsaOptimize's own comment on why folding flags again
// here matters) -- this is the merge that comment does not cover: distinct
// FlagDefs reaching the same test. distributeFlagCondThroughPhi (fold.cpp)
// exists to resolve it anyway, by rewriting each arm's own comparison and
// merging the two answers behind one synthesized boolean phi instead of
// leaving the whole test as a shared opaque stub.
TEST_CASE("a flags merge from two different comparisons distributes the test across the phi",
          "[passes][ssaopt][flags]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  const BlockId left = b.block(0x2000);
  const BlockId right = b.block(0x3000);
  const BlockId join = b.block(0x4000);
  const BlockId whenTrue = b.block(0x5000);
  const BlockId whenFalse = b.block(0x6000);

  const ExprId entryCond =
      b.function.binary(ExprOp::CmpEq, b.read(entry, b.reg("x0"), 0x1000), b.i64(0));
  b.function.appendCondBranch(entry, 0x1004, entryCond, left, right);

  const il::ExprId leftOperands[2] = {b.read(left, b.reg("x1"), 0x2000),
                                      b.read(left, b.reg("x2"), 0x2004)};
  b.function.appendWriteReg(left, 0x2008, b.reg("nzcv"),
                            b.function.flagDef(il::FlagOp::Sub, 64, leftOperands));
  b.function.appendBranch(left, 0x200c, join);

  const il::ExprId rightOperands[2] = {b.read(right, b.reg("x3"), 0x3000),
                                       b.read(right, b.reg("x4"), 0x3004)};
  b.function.appendWriteReg(right, 0x3008, b.reg("nzcv"),
                            b.function.flagDef(il::FlagOp::Sub, 64, rightOperands));
  b.function.appendBranch(right, 0x300c, join);

  const ExprId joinCond = b.function.flagCondition(b.read(join, b.reg("nzcv"), 0x4000),
                                                    il::ConditionCode::Equal);
  b.function.appendCondBranch(join, 0x4004, joinCond, whenTrue, whenFalse);
  b.function.appendStore(whenTrue, 0x5000, Type::integer(64), b.i64(0x9000), b.i64(1));
  b.function.appendReturn(whenTrue, 0x5004);
  b.function.appendStore(whenFalse, 0x6000, Type::integer(64), b.i64(0x9000), b.i64(2));
  b.function.appendReturn(whenFalse, 0x6004);

  b.atCfg();
  runToSsa(b.function);
  REQUIRE(verifiesCleanAt(b.function, Maturity::Ssa));

  // No flagcond survives: both arms resolved to plain compares behind one
  // synthesized boolean phi, not one shared stub.
  const std::string printed = il::print(b.function);
  CHECK(printed.find("flagcond") == std::string::npos);

  // The original flags phi is dead (its only use was the flagcond just
  // resolved away) and DCE removed it; the boolean phi that replaced its use
  // is the only one left.
  CHECK(phiCount(b.function, join) == 1);

  const il::Op& terminator = b.function.op(b.function.block(join).terminator());
  REQUIRE(terminator.code == OpCode::CondBranch);
  const ExprId cond = b.function.operands(terminator)[0];
  REQUIRE(b.function.expr(cond).op == ExprOp::Value);
  const il::ValueId condValue{static_cast<uint32_t>(b.function.expr(cond).immediate)};
  const il::OpId condDef = b.function.value(condValue).definition;
  REQUIRE(b.function.op(condDef).code == OpCode::Phi);
  const auto arms = b.function.operands(b.function.op(condDef));
  REQUIRE(arms.size() == 2);
  CHECK(b.function.expr(arms[0]).op == ExprOp::CmpEq);
  CHECK(b.function.expr(arms[1]).op == ExprOp::CmpEq);
}

}  // namespace
