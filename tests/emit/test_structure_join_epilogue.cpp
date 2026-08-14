// J2e (docs/architecture-optimization-eval-prompt.md §3, Phase 2): a merge
// hub fed by two or more of a dispatch region's own private handler tails --
// analysis::findDispatchJoins's own shape -- is exactly what switchFor's
// matchDispatcherShape epilogue already exists for, except pooled across
// sites rather than voted from one dispatch block's own targets (see
// matchDispatcherShape's `targets.size() < 3` floor, which a scatter-
// dispatcher's typical two-way site never clears on its own). These tests
// exercise Structurizer::joinHubByTail's wiring into switchFor directly: the
// first site whose target names a registered tail claims the hub as its own
// epilogue and every qualifying tail across the region prints as a `Break`
// into it instead of a `Goto` to a label none of them actually needed.
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

  /// A single two-way table dispatch at `dispatch`, same shape as
  /// test_structure_dispatch_region.cpp's own `twoWaySite`: `state = cond ?
  /// smaller : larger`; `brind load(tableBase + state*8)`; resolved so the
  /// select's true (smaller) value reaches `smallerTarget` and its false
  /// (larger) value reaches `largerTarget`.
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

  Function function;
  BlockId entry;
};

StructuredFunction run(Function& function, const StructureOptions& options = {}) {
  return xdec::testing::structureFunction(function, options);
}

/// The first Switch in the tree, so a test can inspect what switchFor
/// attached to it rather than only which statement kinds came out.
const Stmt* firstSwitch(const std::unique_ptr<Stmt>& stmt) {
  if (stmt->kind == StmtKind::Switch) {
    return stmt.get();
  }
  for (const auto& item : stmt->items) {
    if (const Stmt* found = firstSwitch(item)) {
      return found;
    }
  }
  return nullptr;
}

/// Flattens the structured tree into its statement kinds and the Block
/// statements it visits, same walker test_structure_dispatch_region.cpp
/// already uses.
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
    if (stmt->epilogue) visit(stmt->epilogue);
  }
  std::vector<BlockId> blocks;
  std::vector<StmtKind> kinds;
};

}  // namespace

TEST_CASE("three deferred region sites whose private tails all fall into "
          "one hub print it once as a shared epilogue",
          "[emit][structure][join-epilogue]") {
  // site0 (entry) -> tail0 or site1; site1 -> tail1 or site2; site2 -> tail2
  // or afterLast. tail0/tail1/tail2 are each a private, single-op-free jump
  // straight into `hub` -- exactly analysis::findDispatchJoins's own tail
  // shape -- and `hub` has no predecessor besides those three, so it
  // qualifies as a join. deferRegionCollapse keeps every site as a
  // table-mode switch (3 sites is below the organic minRegionSites floor)
  // so switchFor's join-hub branch, not the if/else collapse, is what's
  // under test.
  Fixture f;
  const BlockId site1 = f.block(0x8000);
  const BlockId site2 = f.block(0x8100);
  const BlockId tail0 = f.block(0x9000);
  const BlockId tail1 = f.block(0x9010);
  const BlockId tail2 = f.block(0x9020);
  const BlockId hub = f.block(0xa000);
  const BlockId afterLast = f.block(0xb000);
  constexpr uint64_t tableBase = 0x30b7f0;

  f.twoWaySite(f.entry, tableBase, 0x10, 0x20, tail0, site1);
  f.twoWaySite(site1, tableBase, 0x30, 0x40, tail1, site2);
  f.twoWaySite(site2, tableBase, 0x50, 0x60, tail2, afterLast);
  f.function.appendBranch(tail0, 0x9000, hub);
  f.function.appendBranch(tail1, 0x9010, hub);
  f.function.appendBranch(tail2, 0x9020, hub);
  f.function.appendReturn(hub, 0xa000);
  f.function.appendReturn(afterLast, 0xb000);
  f.function.rebuildEdges();

  StructureOptions options;
  options.deferRegionCollapse = true;
  const StructuredFunction result = run(f.function, options);
  Walk walk;
  walk.visit(result.root);

  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Switch) == 3);
  // hub prints exactly once (as site0's switch's epilogue, the first to
  // reach it), never once per tail the way an unclaimed merge hub would
  // otherwise need its own label for every predecessor.
  CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), hub) == 1);
  // Every tail is still inlined into its own case (none of them needed
  // addGotoTarget's fallback, unlike before J2e): tail0's case is walked up
  // to, not including, hub and closed with a `Break` into the shared
  // epilogue; tail1/tail2's own switches are processed after hub is already
  // claimed, so their cases fall back to the ordinary claimCaseBody path --
  // still inlined, just closed with a `goto hub` since the epilogue itself
  // belongs to site0's switch alone.
  CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), tail0) == 1);
  CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), tail1) == 1);
  CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), tail2) == 1);
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Goto) == 2);
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Break) == 1);
}

TEST_CASE("a join hub that relays a register to the block below it carries a frame",
          "[emit][structure][join-epilogue]") {
  // The same three-tail join, except `hub` falls through to `landing` instead
  // of returning, and both of them merge x5 -- the two-phi relay
  // analysis::LiveRegisterFrame recognises. A voted dispatcher shape gets its
  // frame from its own `hub`; this one has no such block of its own, and the
  // block below the pooled hub is it.
  Fixture f;
  const BlockId site1 = f.block(0x8000);
  const BlockId site2 = f.block(0x8100);
  const BlockId tail0 = f.block(0x9000);
  const BlockId tail1 = f.block(0x9010);
  const BlockId tail2 = f.block(0x9020);
  const BlockId hub = f.block(0xa000);
  const BlockId landing = f.block(0xa100);
  const BlockId afterLast = f.block(0xb000);
  constexpr uint64_t tableBase = 0x30b7f0;

  f.twoWaySite(f.entry, tableBase, 0x10, 0x20, tail0, site1);
  f.twoWaySite(site1, tableBase, 0x30, 0x40, tail1, site2);
  f.twoWaySite(site2, tableBase, 0x50, 0x60, tail2, afterLast);
  f.function.appendBranch(tail0, 0x9000, hub);
  f.function.appendBranch(tail1, 0x9010, hub);
  f.function.appendBranch(tail2, 0x9020, hub);

  const ExprId fromEntry = f.function.entryReg(f.function.registers().find("x5"));
  const ExprId incoming[] = {fromEntry, fromEntry, fromEntry};
  const il::OpId hubPhi = f.function.appendPhi(hub, 0xa000, Type::integer(64), incoming);
  f.function.annotate(hubPhi, "reg:x5");
  f.function.appendBranch(hub, 0xa000, landing);
  const ExprId landingIncoming[] = {f.function.valueRef(f.function.op(hubPhi).result)};
  const il::OpId landingPhi =
      f.function.appendPhi(landing, 0xa100, Type::integer(64), landingIncoming);
  f.function.annotate(landingPhi, "reg:x5");
  f.function.appendReturn(landing, 0xa100);
  f.function.appendReturn(afterLast, 0xb000);
  f.function.rebuildEdges();

  StructureOptions options;
  options.deferRegionCollapse = true;
  const StructuredFunction result = run(f.function, options);
  const Stmt* claimed = firstSwitch(result.root);
  REQUIRE(claimed != nullptr);
  REQUIRE(claimed->mergeBlock == hub);
  REQUIRE(claimed->frame.has_value());
  REQUIRE(claimed->frame->slots.size() == 1);
  CHECK(claimed->frame->slots.front().shadowPhiAtMerge == hubPhi);
  CHECK(claimed->frame->slots.front().livePhiAtHub == landingPhi);
}

TEST_CASE("a hub with a fourth, non-region predecessor is left to goto "
          "instead of claimed as a join epilogue",
          "[emit][structure][join-epilogue]") {
  // Same three-tail shape as above, but `hub` also has `extra`, a plain
  // unconditional predecessor with no dispatch shape at all, reaching it --
  // analysis::findDispatchJoins's own negative case (every real predecessor
  // must be one of the counted tails). switchFor must fall back to its
  // pre-J2e behaviour exactly: no epilogue, every tail prints under its own
  // label with a goto to hub, and hub itself keeps its normal predecessors.
  Fixture f;
  const BlockId site1 = f.block(0x8000);
  const BlockId site2 = f.block(0x8100);
  const BlockId tail0 = f.block(0x9000);
  const BlockId tail1 = f.block(0x9010);
  const BlockId tail2 = f.block(0x9020);
  const BlockId hub = f.block(0xa000);
  const BlockId extra = f.block(0xc000);
  constexpr uint64_t tableBase = 0x30b7f0;

  f.twoWaySite(f.entry, tableBase, 0x10, 0x20, tail0, site1);
  f.twoWaySite(site1, tableBase, 0x30, 0x40, tail1, site2);
  f.twoWaySite(site2, tableBase, 0x50, 0x60, tail2, extra);
  f.function.appendBranch(tail0, 0x9000, hub);
  f.function.appendBranch(tail1, 0x9010, hub);
  f.function.appendBranch(tail2, 0x9020, hub);
  f.function.appendBranch(extra, 0xc000, hub);
  f.function.appendReturn(hub, 0xa000);
  f.function.rebuildEdges();

  StructureOptions options;
  options.deferRegionCollapse = true;
  const StructuredFunction result = run(f.function, options);
  Walk walk;
  walk.visit(result.root);

  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Switch) == 3);
  // hub is not claimed as any switch's epilogue -- it still prints exactly
  // once, but reached only via its own label/goto, not inlined as a Block
  // under a Switch's `epilogue` field.
  CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), hub) == 1);
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Goto) >= 1);
}
