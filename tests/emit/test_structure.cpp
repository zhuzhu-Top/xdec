// structureFunction: diamonds, loops, and switches inline; the rest is
// honest labels and gotos.
#include <catch2/catch_test_macros.hpp>

#include "il/il_test_support.h"
#include "xdec/analysis/dominators.h"
#include "xdec/analysis/loops.h"
#include "xdec/emit/structure.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::Dominators;
using xdec::analysis::NaturalLoop;
using xdec::analysis::PostDominators;
using xdec::emit::Stmt;
using xdec::emit::StmtKind;
using xdec::emit::StructuredFunction;
using xdec::emit::structureFunction;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::Type;

namespace {

struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {
    entry = function.createBlock(0x1000);
    function.setEntryBlock(entry);
  }

  ExprId i64(uint64_t value) { return function.constant(Type::integer(64), value); }
  ExprId cond() {
    return function.binary(ExprOp::CmpNe, function.entryReg(function.registers().find("x0")),
                           i64(0));
  }
  BlockId block(uint64_t va) { return function.createBlock(va); }

  Function function;
  BlockId entry;
};

StructuredFunction run(Function& function) {
  const Dominators dominators = Dominators::compute(function);
  const PostDominators postDominators = PostDominators::compute(function);
  const std::vector<NaturalLoop> loops = naturalLoops(function, dominators);
  return structureFunction(function, dominators, postDominators, loops);
}

/// Counts Block statements and collects the structured kinds, flattening the
/// tree for assertions.
struct Walk {
  void visit(const std::unique_ptr<Stmt>& stmt) {
    if (stmt->kind == StmtKind::Block) {
      blocks.push_back(stmt->block);
    }
    kinds.push_back(stmt->kind);
    for (const auto& item : stmt->items) {
      visit(item);
    }
    if (stmt->thenArm) visit(stmt->thenArm);
    if (stmt->elseArm) visit(stmt->elseArm);
    if (stmt->body) visit(stmt->body);
    for (const auto& body : stmt->caseBodies) {
      if (body) visit(body);
    }
    if (stmt->defaultBody) visit(stmt->defaultBody);
  }
  std::vector<BlockId> blocks;
  std::vector<StmtKind> kinds;
};

TEST_CASE("a reconverging diamond inlines as if-else", "[emit][structure]") {
  Fixture f;
  const BlockId left = f.block(0x2000);
  const BlockId right = f.block(0x3000);
  const BlockId merge = f.block(0x4000);
  f.function.appendCondBranch(f.entry, 0x1000, f.cond(), left, right);
  f.function.appendBranch(left, 0x2000, merge);
  f.function.appendBranch(right, 0x3000, merge);
  f.function.appendReturn(merge, 0x4000);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);
  REQUIRE(walk.blocks.size() == 4);
  const auto ifCount =
      std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::If);
  CHECK(ifCount == 1);
  // No control flow escaped: no gotos needed.
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Goto) == 0);
}

TEST_CASE("a one-sided diamond inlines as if with an inverted condition",
          "[emit][structure]") {
  Fixture f;
  const BlockId then = f.block(0x2000);
  const BlockId merge = f.block(0x4000);
  // Taken arm falls straight to the merge: if (!cond) { then }.
  f.function.appendCondBranch(f.entry, 0x1000, f.cond(), merge, then);
  f.function.appendBranch(then, 0x2000, merge);
  f.function.appendReturn(merge, 0x4000);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);
  CHECK(walk.blocks.size() == 3);
  REQUIRE(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::If) == 1);
  const Stmt* seq = result.root.get();
  const Stmt* ifStmt = nullptr;
  for (const auto& item : seq->items) {
    if (item->kind == StmtKind::If) {
      ifStmt = item.get();
    }
  }
  REQUIRE(ifStmt != nullptr);
  CHECK(ifStmt->invertCond);
  CHECK(ifStmt->thenArm != nullptr);
  CHECK(ifStmt->elseArm == nullptr);
}

TEST_CASE("a bare conditional header becomes a while loop", "[emit][structure]") {
  Fixture f;
  const BlockId header = f.block(0x2000);
  const BlockId body = f.block(0x3000);
  const BlockId exit = f.block(0x4000);
  f.function.appendBranch(f.entry, 0x1000, header);
  f.function.appendCondBranch(header, 0x2000, f.cond(), body, exit);
  f.function.appendBranch(body, 0x3000, header);
  f.function.appendReturn(exit, 0x4000);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);
  // Entry, body, exit carry Block statements; the header is the while's
  // condition and contributes none.
  CHECK(walk.blocks.size() == 3);
  REQUIRE(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::While) == 1);
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Goto) == 0);
}

TEST_CASE("a self-loop latch becomes a do-while", "[emit][structure]") {
  Fixture f;
  const BlockId exit = f.block(0x4000);
  // do { entry } while (cond): the header is its own latch.
  f.function.appendStore(f.entry, 0x1000, Type::integer(64), f.i64(0x9000),
                         f.i64(1));
  f.function.appendCondBranch(f.entry, 0x1004, f.cond(), f.entry, exit);
  f.function.appendReturn(exit, 0x4000);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);
  REQUIRE(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::DoWhile) ==
          1);
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Goto) == 0);
}

TEST_CASE("a loop header that can also exit before the body becomes a "
          "guarded do-while",
          "[emit][structure]") {
  Fixture f;
  // Spinlock-retry shape: the header has real ops of its own (so it is not
  // a bare while-form test) and can leave the loop directly, and the latch
  // can leave through that very same exit block instead of only ever
  // continuing back to the header.
  const BlockId body = f.block(0x2000);
  const BlockId exit = f.block(0x3000);
  f.function.appendStore(f.entry, 0x1000, Type::integer(64), f.i64(0x9000), f.i64(1));
  f.function.appendCondBranch(f.entry, 0x1004, f.cond(), body, exit);
  f.function.appendStore(body, 0x2000, Type::integer(64), f.i64(0x9004), f.i64(2));
  f.function.appendCondBranch(body, 0x2004, f.cond(), f.entry, exit);
  f.function.appendReturn(exit, 0x3000);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);
  REQUIRE(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::DoWhile) == 1);
  // One if for the header's own early exit; the loop's own condition lives
  // on the DoWhile node, not as a second If.
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::If) == 1);
  // The early exit is a labelled goto (a real jump out of the loop's
  // middle); nothing else needed one.
  REQUIRE(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Goto) == 1);
  CHECK(result.isLabeled(exit));
}

TEST_CASE("a do-while latch's real exit block is not lost behind a nested inner loop",
          "[emit][structure]") {
  Fixture f;
  // The exact shape a folded spinlock retry leaves behind: an outer retry
  // loop (entry/latch) wrapping an inner retry loop (innerHead/innerBody) of
  // its own, where the *inner* loop's exit doubles as the *outer* loop's
  // latch, and that latch's non-continuing arm lands on a block with real
  // content rather than a bare return.
  const BlockId innerHead = f.block(0x2000);
  const BlockId innerBody = f.block(0x3000);
  const BlockId latch = f.block(0x4000);
  const BlockId exit = f.block(0x5000);
  f.function.appendBranch(f.entry, 0x1000, innerHead);
  f.function.appendCondBranch(innerHead, 0x2000, f.cond(), latch, innerBody);
  f.function.appendCondBranch(innerBody, 0x3000, f.cond(), innerHead, latch);
  f.function.appendCondBranch(latch, 0x4000, f.cond(), f.entry, exit);
  f.function.appendStore(exit, 0x5000, Type::integer(64), f.i64(0x9000), f.i64(1));
  f.function.appendReturn(exit, 0x5004);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);
  // Every block's content must appear exactly once — most importantly the
  // exit block's store, which a dropped continuation would silently omit
  // (it would still print *somewhere* as an unreachable leftover, not
  // vanish outright, but it must not vanish from the loop's own flow).
  CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), exit) == 1);
  REQUIRE(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::DoWhile) >= 1);
  // The exit block must be reachable from the loop without a goto: either
  // it is the loop's own textual continuation, or a labelled goto reaches
  // it. Either is fine; disappearing is not.
  const bool reachableByGoto =
      result.isLabeled(exit) &&
      std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Goto) > 0;
  const bool reachableByFallthrough = result.root->items.size() > 0 &&
                                       [&] {
                                         for (const auto& item : result.root->items) {
                                           if (item->kind == StmtKind::Block && item->block == exit) {
                                             return true;
                                           }
                                         }
                                         return false;
                                       }();
  CHECK((reachableByGoto || reachableByFallthrough));
}

TEST_CASE("a do-while header with two independent latches keeps both exits",
          "[emit][structure]") {
  Fixture f;
  // Same nested-loop shape as above, but the header also has a second,
  // wholly unrelated back edge (its own version of the real sample's
  // b301 -> b302 -> b92: a block reached from elsewhere in the function
  // that also folds down to a direct branch back to the loop header). Two
  // latches for one header is exactly the case a single chosen latch's
  // do-while form cannot single-handedly account for.
  const BlockId loopHeader = f.block(0x2000);
  const BlockId innerHead = f.block(0x3000);
  const BlockId innerBody = f.block(0x4000);
  const BlockId latch = f.block(0x5000);
  const BlockId exit = f.block(0x6000);
  const BlockId otherPred = f.block(0x7000);
  f.function.appendCondBranch(f.entry, 0x1000, f.cond(), loopHeader, otherPred);
  f.function.appendBranch(otherPred, 0x7000, loopHeader);
  f.function.appendBranch(loopHeader, 0x2000, innerHead);
  f.function.appendCondBranch(innerHead, 0x3000, f.cond(), latch, innerBody);
  f.function.appendCondBranch(innerBody, 0x4000, f.cond(), innerHead, latch);
  f.function.appendCondBranch(latch, 0x5000, f.cond(), loopHeader, exit);
  f.function.appendStore(exit, 0x6000, Type::integer(64), f.i64(0x9000), f.i64(1));
  f.function.appendReturn(exit, 0x6004);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);
  // The two-latch header is exactly the case this test exists to guard: the
  // exit block's own store must show up somewhere, not vanish because the
  // structurer picked the "wrong" latch to key the loop on.
  CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), exit) == 1);
  CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), loopHeader) == 1);
  CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), otherPred) == 1);
}

TEST_CASE("a resolved computed branch over a table becomes a switch",
          "[emit][structure]") {
  Fixture f;
  const BlockId target0 = f.block(0x2000);
  const BlockId target1 = f.block(0x3000);
  const BlockId target2 = f.block(0x4000);
  // switch (load64(table + index*8)): the table shape from jump_table tests.
  const il::ValueId loaded = f.function.appendLoad(
      f.entry, 0x1000, Type::integer(64),
      f.function.binary(ExprOp::Add, f.i64(0x30b7f0),
                        f.function.binary(ExprOp::Shl,
                                          f.function.entryReg(
                                              f.function.registers().find("x0")),
                                          f.i64(3))));
  const il::OpId brind = f.function.appendIndirectBranch(
      f.entry, 0x1004, f.function.valueRef(loaded));
  f.function.setTargets(brind, std::vector<BlockId>{target0, target1, target2});
  f.function.appendReturn(target0, 0x2000);
  f.function.appendReturn(target1, 0x3000);
  f.function.appendReturn(target2, 0x4000);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);
  REQUIRE(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Switch) ==
          1);
  const Stmt* switchStmt = nullptr;
  for (const auto& item : result.root->items) {
    if (item->kind == StmtKind::Switch) {
      switchStmt = item.get();
    }
  }
  REQUIRE(switchStmt != nullptr);
  CHECK(switchStmt->tableMode);
  REQUIRE(switchStmt->cases.size() == 3);
  CHECK(switchStmt->cases[0] == target0);
  CHECK(switchStmt->cases[1] == target1);
  CHECK(switchStmt->cases[2] == target2);
  // Table mode selects on the raw index: entry(x0).
  const il::Expr& selector = f.function.expr(switchStmt->cond);
  CHECK(selector.op == ExprOp::EntryReg);
}

TEST_CASE("a flattened dispatcher chain becomes a switch over the state",
          "[emit][structure]") {
  Fixture f;
  // Range guard, then the equality run; orientations mixed on purpose.
  const BlockId c1 = f.block(0x2000);
  const BlockId c2 = f.block(0x2100);
  const BlockId c3 = f.block(0x2200);
  const BlockId handler1 = f.block(0x3000);
  const BlockId handler2 = f.block(0x3100);
  const BlockId handler3 = f.block(0x3200);
  const BlockId tail = f.block(0x4000);
  const ExprId spine = f.function.entryReg(f.function.registers().find("x0"));
  const ExprId guard = f.function.binary(ExprOp::CmpLtU, spine, f.i64(4));
  f.function.appendCondBranch(f.entry, 0x1000, guard, c1, tail);
  f.function.appendCondBranch(c1, 0x2000,
                              f.function.binary(ExprOp::CmpEq, spine, f.i64(1)),
                              handler1, c2);
  f.function.appendCondBranch(c2, 0x2100,
                              f.function.binary(ExprOp::CmpEq, spine, f.i64(2)),
                              handler2, c3);
  f.function.appendCondBranch(c3, 0x2200,
                              f.function.binary(ExprOp::CmpNe, spine, f.i64(3)),
                              tail, handler3);
  f.function.appendReturn(handler1, 0x3000);
  f.function.appendReturn(handler2, 0x3100);
  f.function.appendReturn(handler3, 0x3200);
  f.function.appendReturn(tail, 0x4000);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);
  REQUIRE(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Switch) == 1);
  // The range guard is part of the dispatch, not something wrapped around it:
  // "values under four, and among those, one/two/three" is one switch, and the
  // guard's own out-of-range arm goes where the run's fall-through goes.
  const Stmt* switchStmt = nullptr;
  for (const auto& item : result.root->items) {
    if (item->kind == StmtKind::Switch) {
      switchStmt = item.get();
    }
  }
  REQUIRE(switchStmt != nullptr);
  CHECK(!switchStmt->tableMode);
  CHECK(switchStmt->block == f.entry);
  REQUIRE(switchStmt->caseValues.size() == 3);
  CHECK(switchStmt->caseValues[0] == 1);
  CHECK(switchStmt->caseValues[1] == 2);
  CHECK(switchStmt->caseValues[2] == 3);
  REQUIRE(switchStmt->cases.size() == 3);
  CHECK(switchStmt->cases[0] == handler1);
  CHECK(switchStmt->cases[1] == handler2);
  CHECK(switchStmt->cases[2] == handler3);
  CHECK(switchStmt->defaultCase == tail);
  // Two tests fall to the default, so it keeps its label and its own block.
  CHECK(result.isLabeled(tail));
  // The tests the switch replaced print no labels: nothing jumps to them.
  CHECK(!result.isLabeled(c1));
  CHECK(!result.isLabeled(c2));
  CHECK(!result.isLabeled(c3));
}

TEST_CASE("a chain starting at the region entry absorbs its head block",
          "[emit][structure]") {
  Fixture f;
  const BlockId c2 = f.block(0x2000);
  const BlockId c3 = f.block(0x2100);
  const BlockId handler1 = f.block(0x3000);
  const BlockId handler2 = f.block(0x3100);
  const BlockId handler3 = f.block(0x3200);
  const BlockId tail = f.block(0x4000);
  const ExprId spine = f.function.entryReg(f.function.registers().find("x0"));
  f.function.appendCondBranch(f.entry, 0x1000,
                              f.function.binary(ExprOp::CmpEq, spine, f.i64(7)),
                              handler1, c2);
  f.function.appendCondBranch(c2, 0x2000,
                              f.function.binary(ExprOp::CmpEq, spine, f.i64(8)),
                              handler2, c3);
  f.function.appendCondBranch(c3, 0x2100,
                              f.function.binary(ExprOp::CmpEq, spine, f.i64(9)),
                              handler3, tail);
  f.function.appendReturn(handler1, 0x3000);
  f.function.appendReturn(handler2, 0x3100);
  f.function.appendReturn(handler3, 0x3200);
  f.function.appendReturn(tail, 0x4000);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  // The switch is the whole region: every handler and the fall-through are
  // reached only through it, so each is written in the arm that reaches it.
  REQUIRE(result.root->items.size() == 1);
  const Stmt& switchStmt = *result.root->items.front();
  CHECK(switchStmt.kind == StmtKind::Switch);
  CHECK(switchStmt.block == f.entry);
  REQUIRE(switchStmt.caseValues.size() == 3);
  CHECK(switchStmt.caseValues[0] == 7);
  CHECK(switchStmt.caseValues[2] == 9);
  CHECK(switchStmt.defaultCase == tail);
  REQUIRE(switchStmt.caseBodies.size() == 3);
  for (std::size_t index = 0; index < 3; ++index) {
    REQUIRE(switchStmt.caseBodies[index] != nullptr);
    CHECK(switchStmt.caseBodies[index]->kind == StmtKind::Sequence);
  }
  REQUIRE(switchStmt.defaultBody != nullptr);
  CHECK_FALSE(result.isLabeled(handler1));
  CHECK_FALSE(result.isLabeled(tail));
  // Every handler still appears exactly once, now inside its case.
  Walk walk;
  walk.visit(result.root);
  for (const BlockId block : {handler1, handler2, handler3, tail}) {
    CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), block) == 1);
  }
}

TEST_CASE("a handler several cases share keeps its label", "[emit][structure]") {
  Fixture f;
  // Two values doing the same thing is one block with two predecessors. Writing
  // it inside the first case would leave the second jumping into the middle of a
  // switch, so both keep the jump and the block keeps its name.
  const BlockId c2 = f.block(0x2000);
  const BlockId c3 = f.block(0x2100);
  const BlockId shared = f.block(0x3000);
  const BlockId own = f.block(0x3200);
  const BlockId tail = f.block(0x4000);
  const ExprId spine = f.function.entryReg(f.function.registers().find("x0"));
  f.function.appendCondBranch(f.entry, 0x1000,
                              f.function.binary(ExprOp::CmpEq, spine, f.i64(7)),
                              shared, c2);
  f.function.appendCondBranch(c2, 0x2000,
                              f.function.binary(ExprOp::CmpEq, spine, f.i64(8)),
                              shared, c3);
  f.function.appendCondBranch(c3, 0x2100,
                              f.function.binary(ExprOp::CmpEq, spine, f.i64(9)),
                              own, tail);
  f.function.appendReturn(shared, 0x3000);
  f.function.appendReturn(own, 0x3200);
  f.function.appendReturn(tail, 0x4000);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  const Stmt& switchStmt = *result.root->items.front();
  REQUIRE(switchStmt.kind == StmtKind::Switch);
  REQUIRE(switchStmt.caseBodies.size() == 3);
  CHECK(switchStmt.caseBodies[0] == nullptr);
  CHECK(switchStmt.caseBodies[1] == nullptr);
  CHECK(switchStmt.caseBodies[2] != nullptr);
  CHECK(result.isLabeled(shared));
  CHECK_FALSE(result.isLabeled(own));
}

TEST_CASE("a dispatch its own cases come back to becomes a loop",
          "[emit][structure]") {
  Fixture f;
  // A state machine: the tests read a state the cases assign, and each case
  // branches back to the first test. The switch is the body of a loop, and the
  // back edges are what `continue` means.
  const BlockId head = f.block(0x2000);
  const BlockId c2 = f.block(0x2100);
  const BlockId c3 = f.block(0x2200);
  const BlockId case1 = f.block(0x3000);
  const BlockId case2 = f.block(0x3100);
  const BlockId case3 = f.block(0x3200);
  const BlockId done = f.block(0x4000);
  f.function.appendBranch(f.entry, 0x1000, head);
  const ExprId state = f.function.entryReg(f.function.registers().find("x0"));
  f.function.appendCondBranch(head, 0x2000,
                              f.function.binary(ExprOp::CmpEq, state, f.i64(1)),
                              case1, c2);
  f.function.appendCondBranch(c2, 0x2100,
                              f.function.binary(ExprOp::CmpEq, state, f.i64(2)),
                              case2, c3);
  f.function.appendCondBranch(c3, 0x2200,
                              f.function.binary(ExprOp::CmpEq, state, f.i64(3)),
                              case3, done);
  f.function.appendBranch(case1, 0x3000, head);
  f.function.appendBranch(case2, 0x3100, head);
  f.function.appendBranch(case3, 0x3200, head);
  f.function.appendReturn(done, 0x4000);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);
  REQUIRE(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::While) == 1);
  const Stmt* loop = nullptr;
  for (const auto& item : result.root->items) {
    if (item->kind == StmtKind::While) {
      loop = item.get();
    }
  }
  REQUIRE(loop != nullptr);
  // No condition: nothing leaves this loop by failing a test.
  CHECK_FALSE(loop->cond.valid());
  REQUIRE(loop->body != nullptr);
  CHECK(loop->body->kind == StmtKind::Switch);
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Continue) == 3);
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Goto) == 0);
  // Nothing jumps to the header any more, so it needs no label.
  CHECK_FALSE(result.isLabeled(head));
}

TEST_CASE("an unmatched computed branch degrades to a compare chain",
          "[emit][structure]") {
  Fixture f;
  const BlockId target0 = f.block(0x2000);
  const BlockId target1 = f.block(0x3000);
  const ExprId opaque = f.function.valueRef(f.function.appendLoad(
      f.entry, 0x1000, Type::integer(64), f.function.entryReg(f.function.registers().find("x0"))));
  const il::OpId brind = f.function.appendIndirectBranch(f.entry, 0x1004, opaque);
  f.function.setTargets(brind, std::vector<BlockId>{target0, target1});
  f.function.appendReturn(target0, 0x2000);
  f.function.appendReturn(target1, 0x3000);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);
  REQUIRE(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Switch) ==
          1);
  for (const auto& item : result.root->items) {
    if (item->kind == StmtKind::Switch) {
      CHECK(!item->tableMode);
    }
  }
}

TEST_CASE("irreducible cross jumps fall back to labels and gotos",
          "[emit][structure]") {
  Fixture f;
  const BlockId other = f.block(0x2000);
  // a -> b, b -> a or exit: a loop, but with entry into the middle of it
  // from two sides once the entry also targets the middle.
  const BlockId mid = f.block(0x3000);
  const BlockId exit = f.block(0x4000);
  f.function.appendCondBranch(f.entry, 0x1000, f.cond(), other, mid);
  f.function.appendBranch(other, 0x2000, mid);
  f.function.appendCondBranch(mid, 0x3000, f.cond(), other, exit);
  f.function.appendReturn(exit, 0x4000);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);
  CHECK(walk.blocks.size() == 4);
  // Every block is emitted exactly once, and somebody needed a goto.
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Goto) >= 1);
  CHECK(!result.labeled.empty());
}

TEST_CASE("a goto that only restates the fallthrough is dropped",
          "[emit][structure]") {
  Fixture f;
  // Two blocks, each reachable from two different predecessors, so neither
  // can be inlined into the other's if/else without duplicating code:
  // exactly the shape `gotoChain` falls back to raw `goto`s on both arms
  // for. `p` reaches both directly; `s` (the block of interest) also
  // branches to both. Once the whole function is laid out, one of `s`'s two
  // targets ends up textually right after `s`'s own if/else anyway — the
  // goto to it is then pure noise.
  const BlockId p = f.block(0x2000);
  const BlockId s = f.block(0x3000);
  const BlockId a = f.block(0x4000);
  const BlockId b = f.block(0x5000);
  const BlockId ra = f.block(0x6000);
  const BlockId rb = f.block(0x7000);
  f.function.appendCondBranch(f.entry, 0x1000, f.cond(), p, s);
  f.function.appendCondBranch(p, 0x2000, f.cond(), a, b);
  f.function.appendCondBranch(s, 0x3000, f.cond(), a, b);
  // `a` has its own irreducible branch (to blocks nobody else reaches), so
  // it defers to the second sweep just like `entry`/`p`/`s` do, instead of
  // resolving trivially in the first sweep the way a bare `return` would —
  // that is what lets it land immediately after `s`'s if/else once both are
  // finally placed, the same coincidence of layout the real sample hit.
  f.function.appendCondBranch(a, 0x4000, f.cond(), ra, rb);
  f.function.appendReturn(b, 0x5000);
  f.function.appendReturn(ra, 0x6000);
  f.function.appendReturn(rb, 0x7000);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);
  // Every block still appears exactly once — dropping a goto must never
  // drop the block it pointed at.
  for (const BlockId block : {p, s, a, b, ra, rb}) {
    CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), block) == 1);
  }
  // `s`'s branch is genuinely irreducible (both targets are shared merge
  // points), so it still needs exactly one explicit goto, not the two a
  // naive gotoChain would print — the other target has to be reachable by
  // plain fallthrough once the tree is fully laid out.
  // At least one of the two irreducible branches (p's or s's, both of which
  // choose between the same shared pair `a`/`b`) must have had a goto
  // elided: one of its two arms is null even though both of its targets are
  // genuine multi-predecessor merge points that a gotoChain, left
  // unoptimized, would always print explicit gotos for.
  std::vector<const Stmt*> stack{result.root.get()};
  int gotoChainIfs = 0;
  int oneArmElided = 0;
  while (!stack.empty()) {
    const Stmt* stmt = stack.back();
    stack.pop_back();
    if (stmt->kind == StmtKind::If && stmt->cond.valid()) {
      const bool thenIsBareGoto = stmt->thenArm && stmt->thenArm->kind == StmtKind::Goto;
      const bool elseIsBareGoto = stmt->elseArm && stmt->elseArm->kind == StmtKind::Goto;
      const bool onlyOneArm = (stmt->thenArm == nullptr) != (stmt->elseArm == nullptr);
      if (thenIsBareGoto || elseIsBareGoto || onlyOneArm) {
        ++gotoChainIfs;
        if (onlyOneArm && (thenIsBareGoto || elseIsBareGoto)) {
          ++oneArmElided;
        }
      }
    }
    for (const auto& item : stmt->items) stack.push_back(item.get());
    if (stmt->thenArm) stack.push_back(stmt->thenArm.get());
    if (stmt->elseArm) stack.push_back(stmt->elseArm.get());
    if (stmt->body) stack.push_back(stmt->body.get());
  }
  REQUIRE(gotoChainIfs >= 1);
  CHECK(oneArmElided >= 1);
}

TEST_CASE("an if left with only its else arm inverts instead of printing empty braces",
          "[emit][structure]") {
  Fixture f;
  // Same shape as the elision test above, so one of the two irreducible
  // branches over the shared `a`/`b` pair loses an arm to the elision pass. Which
  // arm it loses depends on where the surviving blocks land, and that is not
  // something a test should pin: what has to hold either way is that a
  // one-armed if keeps its arm in `then`. An if whose `then` is empty and whose
  // `else` carries the work prints as `if (c) {} else { ... }`, which reads as
  // though the `else` were the exceptional path, when it is the only path.
  const BlockId p = f.block(0x2000);
  const BlockId s = f.block(0x3000);
  const BlockId a = f.block(0x4000);
  const BlockId b = f.block(0x5000);
  const BlockId ra = f.block(0x6000);
  const BlockId rb = f.block(0x7000);
  f.function.appendCondBranch(f.entry, 0x1000, f.cond(), p, s);
  f.function.appendCondBranch(p, 0x2000, f.cond(), a, b);
  f.function.appendCondBranch(s, 0x3000, f.cond(), a, b);
  f.function.appendCondBranch(a, 0x4000, f.cond(), ra, rb);
  f.function.appendReturn(b, 0x5000);
  f.function.appendReturn(ra, 0x6000);
  f.function.appendReturn(rb, 0x7000);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  std::vector<const Stmt*> stack{result.root.get()};
  int oneArmedGotoIfs = 0;
  while (!stack.empty()) {
    const Stmt* stmt = stack.back();
    stack.pop_back();
    if (stmt->kind == StmtKind::If) {
      // No arm is left as a non-null, empty Sequence: every one that lost its
      // only goto either has real content or is null.
      for (const Stmt* arm : {stmt->thenArm.get(), stmt->elseArm.get()}) {
        if (arm != nullptr) {
          const bool emptySequenceArm =
              (arm->kind == StmtKind::Sequence) && arm->items.empty();
          CHECK_FALSE(emptySequenceArm);
        }
      }
      // The rule under test: nothing is left holding its work in `else` alone.
      const bool elseOnly = !stmt->thenArm && stmt->elseArm;
      CHECK_FALSE(elseOnly);
      if (stmt->thenArm && stmt->thenArm->kind == StmtKind::Goto && !stmt->elseArm) {
        ++oneArmedGotoIfs;
      }
    }
    for (const auto& item : stmt->items) stack.push_back(item.get());
    if (stmt->thenArm) stack.push_back(stmt->thenArm.get());
    if (stmt->elseArm) stack.push_back(stmt->elseArm.get());
    if (stmt->body) stack.push_back(stmt->body.get());
  }
  // One of the two shared-target branches really did lose an arm, so the check
  // above ran against the shape it is there for rather than passing vacuously.
  CHECK(oneArmedGotoIfs >= 1);
}

TEST_CASE("a chain of early-return guards nests as ifs with the entry first",
          "[emit][structure]") {
  Fixture f;
  // `if (a) return x; if (b) return y; return z;` — the shape almost every
  // function with argument validation has. Each guard's arm leaves the function,
  // so none of them competes with a diamond for its block, and the guards must
  // come out as nested ifs rather than as gotos to returns parked elsewhere.
  const BlockId firstExit = f.block(0x2000);
  const BlockId second = f.block(0x3000);
  const BlockId secondExit = f.block(0x4000);
  const BlockId tail = f.block(0x5000);
  f.function.appendCondBranch(f.entry, 0x1000, f.cond(), firstExit, second);
  f.function.appendReturn(firstExit, 0x2000);
  f.function.appendCondBranch(second, 0x3000, f.cond(), secondExit, tail);
  f.function.appendReturn(secondExit, 0x4000);
  f.function.appendReturn(tail, 0x5000);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Goto) == 0);
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::If) == 2);
  // The entry leads. A guard's exit block reads as a standalone island if the
  // structurizer's search order is allowed to decide the layout.
  REQUIRE_FALSE(walk.blocks.empty());
  CHECK(walk.blocks.front() == f.entry);
  for (const BlockId block : {firstExit, second, secondExit, tail}) {
    CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), block) == 1);
  }
  // Nothing is named, because nothing has to be jumped to.
  CHECK(result.labeled.empty());
}

TEST_CASE("a block reached only by falling into it gets no label", "[emit][structure]") {
  Fixture f;
  // Two arms of a diamond meeting at a merge: the merge has two predecessors, but
  // both reach it in the text without a jump, so a label on it would be one
  // nothing goes to.
  const BlockId left = f.block(0x2000);
  const BlockId right = f.block(0x3000);
  const BlockId merge = f.block(0x4000);
  f.function.appendCondBranch(f.entry, 0x1000, f.cond(), left, right);
  f.function.appendBranch(left, 0x2000, merge);
  f.function.appendBranch(right, 0x3000, merge);
  f.function.appendReturn(merge, 0x4000);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  CHECK_FALSE(result.isLabeled(merge));
  CHECK(result.labeled.empty());
}

TEST_CASE("a while loop with a genuinely empty body still structures, not crashes",
          "[emit][structure]") {
  Fixture f;
  // `while (cond);`: a busy-wait with nothing between iterations. The
  // header is its own body — `emitRegion` walks zero blocks before hitting
  // its own stop condition — so the While's body is a Sequence with no
  // items at all, the one shape the goto-elision pass must leave as an
  // empty Sequence rather than collapsing to a null body (every printer
  // that owns a loop body dereferences it unconditionally).
  const BlockId header = f.block(0x2000);
  const BlockId exit = f.block(0x3000);
  f.function.appendBranch(f.entry, 0x1000, header);
  f.function.appendCondBranch(header, 0x2000, f.cond(), header, exit);
  f.function.appendReturn(exit, 0x3000);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);  // must not dereference a null body
  REQUIRE(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::While) == 1);
  const Stmt* whileStmt = nullptr;
  for (const auto& item : result.root->items) {
    if (item->kind == StmtKind::While) {
      whileStmt = item.get();
    }
  }
  REQUIRE(whileStmt != nullptr);
  REQUIRE(whileStmt->body != nullptr);
  CHECK(whileStmt->body->items.empty());
}

}  // namespace
