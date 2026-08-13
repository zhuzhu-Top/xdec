// J2f (docs/architecture-optimization-eval-prompt.md §6.5): a natural loop
// whose header terminates in a resolved dispatch, not a `CondBranch`, never
// matches any of tryLoop's or tryDispatcherLoop's shapes (both key off
// `CondBranch` specifically), and wrapAsLoop only ever looks for a back edge
// inside the one switch it just built for that exact header -- not one
// arriving from a wholly separate top-level group reached through several
// other blocks first. `Structurizer::collapseLabeledNaturalLoops` is the
// last-chance sweep for exactly that shape: every one of the loop's blocks
// left as its own untouched, labelled remnant (because none of the ordinary
// patterns claimed it) is proof enough that they collectively *are* the
// loop's body, and folds them back into one `while (true)`.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
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
using xdec::il::Function;
using xdec::il::Type;

namespace {

struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {
    entry = function.createBlock(0x1000);
    function.setEntryBlock(entry);
  }

  BlockId block(uint64_t va) { return function.createBlock(va); }

  /// An unresolved computed branch at `at` with exactly `targets.size()`
  /// static successors -- chain-mode is all this needs (it prints the same
  /// `Switch::cases`/`caseBodies` fields collapseLabeledNaturalLoops reads,
  /// same as a resolved table dispatch would).
  void dispatch(BlockId at, std::vector<BlockId> targets) {
    const uint64_t va = function.block(at).va;
    const il::OpId brind =
        function.appendIndirectBranch(at, va, function.undefined(Type::integer(64)));
    function.setTargets(brind, std::move(targets));
  }

  Function function;
  BlockId entry;
};

StructuredFunction run(Function& function, const StructureOptions& options = {}) {
  return xdec::testing::structureFunction(function, options);
}

/// Flattens the structured tree into its statement kinds and the Block
/// statements it visits, same walker every other structure test uses.
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
  std::vector<StmtKind> kinds;
  std::vector<BlockId> blocks;
};

}  // namespace

TEST_CASE("a dispatch-headed loop whose two case arms both merge into one "
          "unclaimed tail folds into a single while(true)",
          "[emit][structure][labeled-loop]") {
  // `head` dispatches to `a`/`b`/`exit`; `a` and `b` each fall straight into
  // `tail`, which jumps back to `head` -- the back edge. `tail` has two real
  // predecessors neither of which is `head` itself, so claimCaseBody rejects
  // both `a` and `b` (regionClosed fails: `tail`'s *other* predecessor is
  // outside whichever one claimed it) and `head`'s own switch never sees the
  // back edge at all. `a`, `b` and `tail` each end up their own untouched
  // top-level group -- exactly collapseLabeledNaturalLoops' own target shape.
  Fixture f;
  const BlockId head = f.block(0x2000);
  const BlockId a = f.block(0x3000);
  const BlockId b = f.block(0x3100);
  const BlockId tail = f.block(0x4000);
  const BlockId exit = f.block(0x5000);
  f.function.appendBranch(f.entry, 0x1000, head);
  f.dispatch(head, {a, b, exit});
  f.function.appendBranch(a, 0x3000, tail);
  f.function.appendBranch(b, 0x3100, tail);
  f.function.appendBranch(tail, 0x4000, head);
  f.function.appendReturn(exit, 0x5000);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);

  // Every block the loop owns prints exactly once, whether merged into the
  // while body or (had the merge been rejected) left as its own goto target
  // -- nothing here duplicates or drops a block either way.
  for (const BlockId member : {head, a, b, tail, exit}) {
    CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), member) == 1);
  }
  REQUIRE(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::While) == 1);
  const Stmt* loop = nullptr;
  for (const auto& item : result.root->items) {
    if (item->kind == StmtKind::While) {
      loop = item.get();
    }
  }
  REQUIRE(loop != nullptr);
  CHECK_FALSE(loop->cond.valid());  // while (true): nothing tests an exit here
  CHECK(loop->block == head);
  // tail's own back edge to head reads as `continue`, not a labelled goto.
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::Continue) == 1);
  CHECK_FALSE(result.isLabeled(head));
}

TEST_CASE("a dispatch-headed loop is left alone when one of its members "
          "already holds a loop of its own",
          "[emit][structure][labeled-loop]") {
  // Same shape as above, but `a` is itself a self-looping dispatch (its own
  // `back`/`exit` targets it) -- wrapAsLoop already turns `a`'s own group
  // into a `while (true)` before collapseLabeledNaturalLoops ever runs.
  // Folding that into an outer loop would need to tell `a`'s own `continue`
  // apart from one meant for `head`'s loop, which nothing here attempts, so
  // the whole merge for `head` is left alone -- `a`'s own loop stays exactly
  // where it already was, `head`'s switch is unaffected, and no while forms
  // for `head` at all.
  Fixture f;
  const BlockId head = f.block(0x2000);
  const BlockId a = f.block(0x3000);
  const BlockId back = f.block(0x3010);
  const BlockId b = f.block(0x3100);
  const BlockId tail = f.block(0x4000);
  const BlockId exit = f.block(0x5000);
  f.function.appendBranch(f.entry, 0x1000, head);
  f.dispatch(head, {a, b, exit});
  f.dispatch(a, {back, tail});
  f.function.appendBranch(back, 0x3010, a);
  f.function.appendBranch(b, 0x3100, tail);
  f.function.appendBranch(tail, 0x4000, head);
  f.function.appendReturn(exit, 0x5000);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);

  for (const BlockId member : {head, a, back, b, tail, exit}) {
    CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), member) == 1);
  }
  // `a`'s own self-loop still forms; `head`'s does not.
  CHECK(std::count(walk.kinds.begin(), walk.kinds.end(), StmtKind::While) == 1);
}
