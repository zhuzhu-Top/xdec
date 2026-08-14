// J2 (docs/architecture-optimization-eval-prompt.md §3 Phase 3):
// Structurizer::collapseRegionDispatchTree runs unconditionally once
// switchFor finishes building a table-mode switch belonging to some
// analysis::DispatchRegion. This file's Fixture builds the plan's own
// reference shape -- two-way table dispatch sites chained linearly, all
// reading through one physical table -- to exercise that pass directly:
// a chain of sites that each recompute their own, different discriminant
// declines the fold (each site's own state read is a different ExprId), a
// relay reading the identical already-evaluated discriminant as its own
// dispatcher gets flattened into the outer switch's cases, and a relay that
// recomputes a fresh discriminant stays nested.
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "il/il_test_support.h"
#include "xdec/emit/structure.h"
#include "xdec/il/function.h"

#include "../fixture/pipeline_fixture.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::emit::StmtKind;
using xdec::emit::StructuredFunction;
using xdec::emit::StructureOptions;
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

  /// A single two-way table dispatch, same shape
  /// test_structure_dispatch_region.cpp's own `twoWaySite` already uses:
  /// `state = cond ? smaller : larger`; `brind load(tableBase + state*8)`;
  /// resolved so the select's true (smaller) value reaches `smallerTarget`
  /// and its false (larger) value reaches `largerTarget`.
  void twoWaySite(BlockId dispatch, uint64_t tableBase, uint64_t smaller, uint64_t larger,
                  BlockId smallerTarget, BlockId largerTarget) {
    const uint64_t va = function.block(dispatch).va;
    const ExprId state = function.select(cond(), i64(smaller), i64(larger));
    const il::ValueId loaded = function.appendLoad(
        dispatch, va, Type::integer(64),
        function.binary(ExprOp::Add, i64(tableBase), function.binary(ExprOp::Shl, state, i64(3))));
    const il::OpId brind = function.appendIndirectBranch(dispatch, va + 4, function.valueRef(loaded));
    function.setTargets(brind, std::vector<BlockId>{smallerTarget, largerTarget});
  }

  /// The plan's own reference shape (§3.4): `count` two-way table dispatch
  /// sites chained linearly through one shared table, each site's "smaller"
  /// arm a private leaf that returns outright and its "larger" arm chaining
  /// into the next site (or `afterLast` for the final one) -- every site's
  /// caseValues statically recoverable from its own select, so a future
  /// collapseRegionDispatchTree has nothing standing in the way of naming
  /// every case. `first` is site 0's own dispatch block.
  void chainedTwoWaySites(BlockId first, std::size_t count, uint64_t tableBase, BlockId afterLast) {
    std::vector<BlockId> dispatches(count);
    dispatches[0] = first;
    for (std::size_t i = 1; i < count; ++i) {
      dispatches[i] = block(0x8000 + 0x100 * static_cast<uint64_t>(i));
    }
    for (std::size_t i = 0; i < count; ++i) {
      const BlockId leaf = block(0x9000 + 0x10 * static_cast<uint64_t>(i));
      const BlockId next = (i + 1 < count) ? dispatches[i + 1] : afterLast;
      const uint64_t smaller = 0x10 + 0x100 * static_cast<uint64_t>(i);
      const uint64_t larger = 0x20 + 0x100 * static_cast<uint64_t>(i);
      twoWaySite(dispatches[i], tableBase, smaller, larger, leaf, next);
      function.appendReturn(leaf, function.block(leaf).va);
    }
  }

  /// A two-way table dispatch whose index is exactly `index` -- not a fresh
  /// `select()` of this call's own, but literally the same already-computed
  /// `ExprId` a caller passes in. Reusing one site's own index this way is
  /// what makes two dispatch blocks' `matchJumpTable` results provably the
  /// same discriminant (collapseRegionDispatchTree's own soundness
  /// requirement): `index` here carries no recoverable Select structure, so
  /// `matchDispatchValues` reports nothing for either site and both switches
  /// print ordinal case labels -- irrelevant to the shape under test, which
  /// is purely about how many `Switch` nodes the tree has, not their labels.
  void bareIndexSite(BlockId dispatch, ExprId index, uint64_t tableBase, BlockId first,
                     BlockId second) {
    const uint64_t va = function.block(dispatch).va;
    const il::ValueId loaded = function.appendLoad(
        dispatch, va, Type::integer(64),
        function.binary(ExprOp::Add, i64(tableBase), function.binary(ExprOp::Shl, index, i64(3))));
    const il::OpId brind = function.appendIndirectBranch(dispatch, va + 4, function.valueRef(loaded));
    function.setTargets(brind, std::vector<BlockId>{first, second});
  }

  Function function;
  BlockId entry;
};

StructuredFunction run(Function& function, const StructureOptions& options = {}) {
  return xdec::testing::structureFunction(function, options);
}

struct Walk {
  void visit(const std::unique_ptr<xdec::emit::Stmt>& stmt) {
    kinds.push_back(stmt->kind);
    for (const auto& item : stmt->items) visit(item);
    if (stmt->thenArm) visit(stmt->thenArm);
    if (stmt->elseArm) visit(stmt->elseArm);
    if (stmt->body) visit(stmt->body);
    for (const auto& body : stmt->caseBodies) {
      if (body) visit(body);
    }
    if (stmt->defaultBody) visit(stmt->defaultBody);
    if (stmt->epilogue) visit(stmt->epilogue);
  }
  std::vector<StmtKind> kinds;
};

}  // namespace

TEST_CASE("collapseRegionDispatchTree declines a chained region whose sites "
          "each recompute their own, different discriminant",
          "[emit][structure][region-switch]") {
  // 7 sites organically crosses the default minRegionSites floor (8 is the
  // floor, but the diagnostic override below reaches the same deferred
  // shape without relying on that), so switchFor already keeps every site
  // as its own table-mode switch instead of collapsing to if/else. Each
  // site's own state read is a fresh `select()` -- a different ExprId, not
  // a shared discriminant -- so collapseRegionDispatchTree's exact-`ExprId`
  // soundness check declines every one of them: running it must not move a
  // single one of those switches.
  Fixture f;
  const BlockId tail = f.block(0x9100);
  f.chainedTwoWaySites(f.entry, 7, 0x30b7f0, tail);
  f.function.appendReturn(tail, 0x9100);
  f.function.rebuildEdges();

  StructureOptions options;
  options.deferRegionCollapse = true;
  Walk walk;
  walk.visit(run(f.function, options).root);

  REQUIRE(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Switch) == 7);
}

TEST_CASE("collapseRegionDispatchTree flattens a private relay dispatch "
          "reading the identical discriminant into the outer switch's own "
          "cases",
          "[emit][structure][region-switch]") {
  // entry dispatches on `index` to {tail0, relay}; relay -- reached only
  // from entry's own second target -- reads that exact same `index` again
  // (not a fresh recomputation) and dispatches to {leafA, leafB}. Sound to
  // flatten (see collapseRegionDispatchTree's own comment): both switches
  // are provably testing the one already-evaluated expression, so entry's
  // own switch can carry relay's two cases directly instead of nesting
  // relay's whole switch inside its own second case.
  Fixture f;
  const ExprId index = f.function.entryReg(f.function.registers().find("x0"));
  const BlockId relay = f.block(0x8000);
  const BlockId tail0 = f.block(0x9000);
  const BlockId leafA = f.block(0x9010);
  const BlockId leafB = f.block(0x9020);
  constexpr uint64_t tableBase = 0x30b7f0;

  f.bareIndexSite(f.entry, index, tableBase, tail0, relay);
  f.bareIndexSite(relay, index, tableBase, leafA, leafB);
  f.function.appendReturn(tail0, 0x9000);
  f.function.appendReturn(leafA, 0x9010);
  f.function.appendReturn(leafB, 0x9020);
  f.function.rebuildEdges();

  StructureOptions options;
  options.deferRegionCollapse = true;
  const StructuredFunction result = run(f.function, options);
  Walk walk;
  walk.visit(result.root);

  REQUIRE(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Switch) == 1);
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Goto) == 0);

  const xdec::emit::Stmt* switchStmt = nullptr;
  std::vector<const xdec::emit::Stmt*> stack{result.root.get()};
  while (!stack.empty()) {
    const xdec::emit::Stmt* stmt = stack.back();
    stack.pop_back();
    if (stmt->kind == StmtKind::Switch) {
      switchStmt = stmt;
    }
    for (const auto& item : stmt->items) stack.push_back(item.get());
    if (stmt->thenArm) stack.push_back(stmt->thenArm.get());
    if (stmt->elseArm) stack.push_back(stmt->elseArm.get());
    if (stmt->body) stack.push_back(stmt->body.get());
    for (const auto& body : stmt->caseBodies) {
      if (body) stack.push_back(body.get());
    }
    if (stmt->defaultBody) stack.push_back(stmt->defaultBody.get());
  }
  REQUIRE(switchStmt != nullptr);
  REQUIRE(switchStmt->cases.size() == 3);
  CHECK(switchStmt->cases[0] == tail0);
  CHECK(switchStmt->cases[1] == leafA);
  CHECK(switchStmt->cases[2] == leafB);
}

TEST_CASE("collapseRegionDispatchTree does not flatten a relay dispatch "
          "that recomputes its own, different discriminant",
          "[emit][structure][region-switch]") {
  // Same shape as above, but `relay` reads a *fresh* select rather than
  // entry's own `index` -- the ordinary scatter-dispatcher shape, where a
  // handler's own next-state computation is not provably the value entry
  // already tested. Must stay nested: folding it into one flat switch would
  // print case labels for values entry's own expression never takes.
  Fixture f;
  const ExprId index = f.function.entryReg(f.function.registers().find("x0"));
  const BlockId relay = f.block(0x8000);
  const BlockId tail0 = f.block(0x9000);
  const BlockId leafA = f.block(0x9010);
  const BlockId leafB = f.block(0x9020);
  constexpr uint64_t tableBase = 0x30b7f0;

  f.bareIndexSite(f.entry, index, tableBase, tail0, relay);
  const ExprId relayIndex = f.function.binary(ExprOp::Add, index, f.i64(1));
  f.bareIndexSite(relay, relayIndex, tableBase, leafA, leafB);
  f.function.appendReturn(tail0, 0x9000);
  f.function.appendReturn(leafA, 0x9010);
  f.function.appendReturn(leafB, 0x9020);
  f.function.rebuildEdges();

  StructureOptions options;
  options.deferRegionCollapse = true;
  const StructuredFunction result = run(f.function, options);
  Walk walk;
  walk.visit(result.root);

  REQUIRE(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Switch) == 2);
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Goto) == 0);
}
