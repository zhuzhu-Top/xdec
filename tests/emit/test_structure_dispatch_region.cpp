// J1 (docs/18-architecture-optimization-plan.md §5.2): switchFor's two-way
// table dispatch collapse is right for an isolated site, but wrong for one
// of many sites all reading through the same physical table (see
// analysis::DispatchRegion) -- collapsing every one away loses the table
// identity tying them together. These tests exercise the collapse-defer
// gate (StructureOptions::minRegionSites/deferRegionCollapse) directly: a
// region large enough keeps table-mode switches, a region too small (or no
// region at all) collapses exactly as before, and the two ways of reaching
// "defer" -- an organically large region, and the diagnostic override --
// each work on their own.
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "il/il_test_support.h"
#include "xdec/emit/structure.h"
#include "xdec/il/function.h"

#include "../fixture/pipeline_fixture.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::emit::Stmt;
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

  /// A single two-way table dispatch at `dispatch`: `state = cond ? smaller :
  /// larger`; `brind load(tableBase + state*8)`; resolved so the select's
  /// true (smaller) value reaches `smallerTarget` and its false (larger)
  /// value reaches `largerTarget` -- switchFor's own if/else-collapse shape
  /// (see test_structure.cpp's "a resolved two-target table dispatch
  /// collapses to if/else...").
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

  /// Builds `count` chained two-way table dispatch sites reading through one
  /// shared table base, so analysis::findDispatchRegions clusters them into
  /// one analysis::DispatchRegion with `count` sites. Each site's "continue"
  /// (larger-value) arm chains into the next site's own dispatch block, or
  /// into `afterLast` for the final site; each site's other arm is a
  /// private leaf that returns outright. `first` is site 0's own dispatch
  /// block (typically the function's entry, so the whole chain is reachable
  /// and structureFunction actually walks every site).
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

  Function function;
  BlockId entry;
};

StructuredFunction run(Function& function, const StructureOptions& options = {}) {
  return xdec::testing::structureFunction(function, options);
}

/// Counts Block statements and collects the structured kinds, flattening the
/// tree for assertions -- same walker as test_structure.cpp's.
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

/// Every Switch node reachable from `root`, found by walking every arm a
/// Stmt can hold (mirrors Walk, but returns the nodes themselves rather than
/// just their kind, since a couple of these tests need to inspect
/// `tableMode`/`caseValues` on switches that end up nested inside another
/// switch's own case body).
std::vector<const Stmt*> allSwitches(const Stmt* root) {
  std::vector<const Stmt*> found;
  std::vector<const Stmt*> stack{root};
  while (!stack.empty()) {
    const Stmt* stmt = stack.back();
    stack.pop_back();
    if (stmt->kind == StmtKind::Switch) {
      found.push_back(stmt);
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
  return found;
}

}  // namespace

TEST_CASE("a chained two-way table dispatch region below the collapse-defer "
          "floor still collapses to if/else",
          "[emit][structure][dispatch-region]") {
  // 3 sites is well under the default floor (8, aligned with
  // analysis::ObfuscationProfile::likelyFlattened's own dispatcherFanIn
  // threshold) -- confirms the gate leaves small, ordinary two-way table
  // dispatches exactly as switchFor already prints them (L1 unchanged).
  Fixture f;
  const BlockId tail = f.block(0x9100);
  f.chainedTwoWaySites(f.entry, 3, 0x30b7f0, tail);
  f.function.appendReturn(tail, 0x9100);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::If) == 3);
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Switch) == 0);
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Goto) == 0);
}

TEST_CASE("a chained two-way table dispatch region at or above the "
          "collapse-defer floor keeps table-mode switches instead",
          "[emit][structure][dispatch-region]") {
  // 8 sites organically crosses the default floor with no diagnostic
  // override at all -- the real production gate (StructureOptions::
  // minRegionSites), not just the test-only deferRegionCollapse switch.
  Fixture f;
  const BlockId tail = f.block(0x9100);
  f.chainedTwoWaySites(f.entry, 8, 0x30b7f0, tail);
  f.function.appendReturn(tail, 0x9100);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::If) == 0);
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Switch) == 8);
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Goto) == 0);
  for (const Stmt* switchStmt : allSwitches(result.root.get())) {
    CHECK(switchStmt->tableMode);
    REQUIRE(switchStmt->caseValues.size() == 2);
  }
}

TEST_CASE("lowering minRegionSites defers a small region's sites while an "
          "isolated site through a different table still collapses",
          "[emit][structure][dispatch-region]") {
  // Two independent clusters in one function: a 3-site region through
  // tableBase A (deferred once the floor is lowered to 3) and one isolated
  // site through a wholly different tableBase B (its own 1-site region,
  // still well under the lowered floor). Selectivity, not a blanket switch:
  // membership plus size is what the gate actually reads, not merely
  // whether *some* region exists in the function.
  Fixture f;
  const BlockId isolatedDispatch = f.block(0x7000);
  f.chainedTwoWaySites(f.entry, 3, 0x30b7f0, isolatedDispatch);
  const BlockId isolatedSmaller = f.block(0xa000);
  const BlockId isolatedLarger = f.block(0xa100);
  f.twoWaySite(isolatedDispatch, 0x2a2428, 0x10, 0x20, isolatedSmaller, isolatedLarger);
  f.function.appendReturn(isolatedSmaller, 0xa000);
  f.function.appendReturn(isolatedLarger, 0xa100);
  f.function.rebuildEdges();

  StructureOptions options;
  options.minRegionSites = 3;
  const StructuredFunction result = run(f.function, options);
  Walk walk;
  walk.visit(result.root);
  // The 3-site region (tableBase A) is at the lowered floor: its sites keep
  // their table-mode switches.
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Switch) == 3);
  // The isolated site (tableBase B, region size 1) is nowhere near the
  // lowered floor either: it still collapses to a plain if/else.
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::If) == 1);
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Goto) == 0);
  bool foundIsolatedIf = false;
  std::vector<const Stmt*> stack{result.root.get()};
  while (!stack.empty()) {
    const Stmt* stmt = stack.back();
    stack.pop_back();
    if (stmt->kind == StmtKind::If) {
      Walk ifWalk;
      if (stmt->thenArm) ifWalk.visit(stmt->thenArm);
      if (stmt->elseArm) ifWalk.visit(stmt->elseArm);
      if (std::count(ifWalk.blocks.begin(), ifWalk.blocks.end(), isolatedSmaller) == 1 ||
          std::count(ifWalk.blocks.begin(), ifWalk.blocks.end(), isolatedLarger) == 1) {
        foundIsolatedIf = true;
      }
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
  CHECK(foundIsolatedIf);
}

TEST_CASE("a handler two deferred region sites both fall into is cloned into "
          "each case instead of left as a goto",
          "[emit][structure][dispatch-region]") {
  // J2d (docs/18-architecture-optimization-plan.md §5.3's handler-clone
  // extension, wired into switchFor's table-mode case loop): once J1 defers
  // a region site's if/else collapse, its cases are claimed by the very
  // same table-mode switch loop tryDispatcherLoop's shared-tail cases use --
  // which, before this test's fix, never tried claimOrCloneSharedCaseBody at
  // all, so a handler more than one deferred site fell into always kept a
  // label and printed a goto on every side, no matter how small it was.
  // Two sites through one table, deferred via the diagnostic switch so both
  // print as table-mode switches; both sites' "smaller" arm reaches the
  // exact same shared handler, the shape claimOrCloneSharedCaseBody exists
  // for (every predecessor a resolved two-target table dispatch).
  Fixture f;
  const BlockId site1 = f.block(0x8000);
  const BlockId sharedHandler = f.block(0x9000);
  const BlockId tail = f.block(0xa000);
  f.twoWaySite(f.entry, 0x30b7f0, 0x10, 0x20, sharedHandler, site1);
  f.twoWaySite(site1, 0x30b7f0, 0x30, 0x40, sharedHandler, tail);
  f.function.appendReturn(sharedHandler, 0x9000);
  f.function.appendReturn(tail, 0xa000);
  f.function.rebuildEdges();

  StructureOptions options;
  options.deferRegionCollapse = true;
  const StructuredFunction result = run(f.function, options);
  Walk walk;
  walk.visit(result.root);
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Switch) == 2);
  // Cloned once per site that falls into it, not printed once behind a label
  // (an unclaimed case prints its `goto` straight from a null caseBodies
  // slot -- see the negative test below -- so there is no `Goto` node this
  // count could check instead).
  CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), sharedHandler) == 2);
}

TEST_CASE("an organically large region still clones a handler two of its "
          "own non-adjacent sites share",
          "[emit][structure][dispatch-region]") {
  // Same shape as above, but reached through minRegionSites alone (no
  // deferRegionCollapse override), with 8 chained sites (site indices 0..7)
  // so the region organically crosses the default floor, and the two
  // sharing sites (2 and 5) two apart in the chain rather than adjacent --
  // confirms the clone fallback fires regardless of which two of a
  // region's many sites happen to share a handler, not just a hand-picked
  // pair at the start. Built by hand rather than through
  // Fixture::chainedTwoWaySites, which only hands back the region's first
  // (entry) dispatch block -- this needs every site's own BlockId to
  // retarget two of them at the same shared handler.
  Fixture f;
  constexpr std::size_t kCount = 8;
  constexpr uint64_t tableBase = 0x30b7f0;
  const BlockId sharedHandler = f.block(0x9200);
  const BlockId tail = f.block(0x9100);
  std::vector<BlockId> dispatches(kCount);
  dispatches[0] = f.entry;
  for (std::size_t i = 1; i < kCount; ++i) {
    dispatches[i] = f.block(0x8000 + 0x100 * static_cast<uint64_t>(i));
  }
  for (std::size_t i = 0; i < kCount; ++i) {
    const BlockId next = (i + 1 < kCount) ? dispatches[i + 1] : tail;
    const uint64_t smaller = 0x10 + 0x100 * static_cast<uint64_t>(i);
    const uint64_t larger = 0x20 + 0x100 * static_cast<uint64_t>(i);
    if (i == 2 || i == 5) {
      // Both sharing sites' "smaller" arm reaches the same shared handler
      // instead of a private leaf -- still a resolved two-target table
      // dispatch through the region's one table either way, which is all
      // claimOrCloneSharedCaseBody's own precondition requires.
      f.twoWaySite(dispatches[i], tableBase, smaller, larger, sharedHandler, next);
    } else {
      const BlockId leaf = f.block(0x9000 + 0x10 * static_cast<uint64_t>(i));
      f.twoWaySite(dispatches[i], tableBase, smaller, larger, leaf, next);
      f.function.appendReturn(leaf, f.function.block(leaf).va);
    }
  }
  f.function.appendReturn(sharedHandler, 0x9200);
  f.function.appendReturn(tail, 0x9100);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Switch) == 8);
  CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), sharedHandler) == 2);
}

TEST_CASE("a handler shared with a non-table predecessor still falls back "
          "to goto in table-mode, not just in the if/else collapse",
          "[emit][structure][dispatch-region]") {
  // The negative twin of the two tests above: claimOrCloneSharedCaseBody
  // itself declines whenever any one predecessor of the shared handler is
  // not a resolved two-target table dispatch -- switchFor's table-mode loop
  // must still fall back to addGotoTarget exactly as it already did before
  // J2d, not crash or claim something unproven. `extra`, a plain
  // unconditional predecessor with no dispatch shape at all, is that one
  // disqualifying predecessor.
  Fixture f;
  const BlockId site1 = f.block(0x8000);
  const BlockId sharedHandler = f.block(0x9000);
  const BlockId tail = f.block(0xa000);
  const BlockId extra = f.block(0xb000);
  f.twoWaySite(f.entry, 0x30b7f0, 0x10, 0x20, sharedHandler, site1);
  f.twoWaySite(site1, 0x30b7f0, 0x30, 0x40, sharedHandler, tail);
  f.function.appendBranch(extra, 0xb000, sharedHandler);
  f.function.appendReturn(sharedHandler, 0x9000);
  f.function.appendReturn(tail, 0xa000);
  f.function.rebuildEdges();

  StructureOptions options;
  options.deferRegionCollapse = true;
  const StructuredFunction result = run(f.function, options);
  Walk walk;
  walk.visit(result.root);
  const std::vector<const Stmt*> switches = allSwitches(result.root.get());
  REQUIRE(switches.size() == 2);
  // Declined clone: neither switch's case for `sharedHandler` claimed a
  // body (an unclaimed slot, not a `Goto` node -- switchFor never builds
  // one for an unclaimed case, see gotoStmt's callers vs. addGotoTarget's;
  // the printer writes the `goto` text straight from the null slot itself).
  for (const Stmt* switchStmt : switches) {
    for (std::size_t index = 0; index < switchStmt->cases.size(); ++index) {
      if (switchStmt->cases[index] == sharedHandler) {
        CHECK(switchStmt->caseBodies[index] == nullptr);
      }
    }
  }
  // The handler itself still prints exactly once, under its own label, for
  // every predecessor (both switches and `extra`) to reach through a goto.
  CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), sharedHandler) == 1);
}

TEST_CASE("the deferRegionCollapse diagnostic switch defers a lone two-way "
          "table dispatch with no region size involved at all",
          "[emit][structure][dispatch-region]") {
  // A single site is its own 1-site region -- nowhere near even a lowered
  // floor -- so this is only reachable through the independent
  // `deferRegionCollapse` switch, not through `minRegionSites` at all.
  Fixture f;
  const BlockId smallerTarget = f.block(0x2000);
  const BlockId largerTarget = f.block(0x3000);
  f.twoWaySite(f.entry, 0x30b7f0, 0x10, 0x20, smallerTarget, largerTarget);
  f.function.appendReturn(smallerTarget, 0x2000);
  f.function.appendReturn(largerTarget, 0x3000);
  f.function.rebuildEdges();

  {
    // Baseline: default options collapse it, exactly as switchFor always
    // has for an isolated site.
    const StructuredFunction result = run(f.function);
    Walk walk;
    walk.visit(result.root);
    CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::If) == 1);
    CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Switch) == 0);
  }
  {
    StructureOptions options;
    options.deferRegionCollapse = true;
    const StructuredFunction result = run(f.function, options);
    Walk walk;
    walk.visit(result.root);
    CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::If) == 0);
    REQUIRE(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Switch) == 1);
    CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Goto) == 0);
    const std::vector<const Stmt*> switches = allSwitches(result.root.get());
    REQUIRE(switches.size() == 1);
    CHECK(switches.front()->tableMode);
  }
}
