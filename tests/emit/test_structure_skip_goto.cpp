// Phase 2 (docs/architecture-optimization-eval-prompt.md §6.3's own J2e-if
// gap): switchFor's if/else collapse fallback hands each arm to
// claimCaseBody, an unbounded walk with no stop block of its own. When one
// arm's target is a block the *other* arm's own private tail falls straight
// into, that unbounded walk swallows it whole -- so instead of a diamond
// that reconverges after the `if`, one arm ends with a bare `goto` to
// content already sitting, unlabelled, inside the other arm. These tests
// exercise Structurizer::restructureSkipGotos, the post-pass that notices
// the shape and reshapes it into a plain `if (!cond) { WORK; }` followed by
// the target's own content run inline, no `goto` involved.
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

TEST_CASE("a hub one arm reaches directly and the other arm's own private "
          "tail falls into prints once, inline, with neither arm ending in "
          "a goto",
          "[emit][structure][skip-goto]") {
  // `hub`'s two predecessors -- `entry` directly (one of the dispatch's own
  // two targets) and `work`'s plain fallthrough -- disqualify both
  // claimCaseBody (not a private single-predecessor handler) and
  // claimOrCloneSharedCaseBody (one predecessor, `work`, is not itself a
  // resolved two-target dispatch) from claiming `hub` on its own arm, so
  // without this pass that arm is left with a bare `goto hub;` while
  // `work`'s own unbounded walk has already swallowed `hub` whole.
  Fixture f;
  const BlockId work = f.block(0x8000);
  const BlockId hub = f.block(0x9000);
  f.twoWaySite(f.entry, 0x30b7f0, 0x10, 0x20, hub, work);
  f.function.appendBranch(work, 0x8000, hub);
  f.function.appendReturn(hub, 0x9000);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::If) == 1);
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Goto) == 0);
  // `hub` still prints exactly once -- moved out from inside the other
  // arm's own body, not duplicated.
  CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), hub) == 1);
  CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), work) == 1);
}

TEST_CASE("a chain of three skip-goto sites unwinds all the way through, "
          "each hub landing after its own if instead of nested inside the "
          "next site's arm",
          "[emit][structure][skip-goto]") {
  // Same shape as the single-site test above, chained: `work`'s own tail
  // falls into `hub`, which is itself another two-way dispatch whose
  // "direct" target is `hub2`, and so on -- exactly the repeating pattern
  // sub_b7000's own null-check chain collapses into. Each level's promoted
  // tail has to itself be re-examined for the same shape, which is what
  // restructureSkipGotos's own re-scan of a Sequence after splicing exists
  // for.
  Fixture f;
  const BlockId work = f.block(0x8000);
  const BlockId hub = f.block(0x9000);
  const BlockId work2 = f.block(0xa000);
  const BlockId hub2 = f.block(0xb000);
  f.twoWaySite(f.entry, 0x30b7f0, 0x10, 0x20, hub, work);
  f.function.appendBranch(work, 0x8000, hub);
  f.twoWaySite(hub, 0x30c800, 0x10, 0x20, hub2, work2);
  f.function.appendBranch(work2, 0xa000, hub2);
  f.function.appendReturn(hub2, 0xb000);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::If) == 2);
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Goto) == 0);
  CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), hub) == 1);
  CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), hub2) == 1);
}
