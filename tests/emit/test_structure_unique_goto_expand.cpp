// J2g (docs/architecture-optimization-eval-prompt.md §6.6, "Unique Goto
// Expansion"): a switch case (or default arm) `claimCaseBody`/
// `claimDispatcherCaseBody` never managed to inline prints as a bare `goto`
// to its handler, which then keeps its own separate top-level `groups` entry
// and label purely to hold the few lines that `goto` was always going to run
// next. `Structurizer::expandUniqueCaseGotos` is the last-chance sweep for
// exactly that shape: when nothing else in the whole finished tree still
// names that handler, its group folds whole into the case body and
// disappears instead of keeping a label nothing else needs.
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

  BlockId block(uint64_t va) { return function.createBlock(va); }

  /// An unresolved computed branch at `at` with exactly `targets.size()`
  /// static successors -- chain-mode is all this needs (it prints the same
  /// `Switch::cases`/`caseBodies` fields expandUniqueCaseGotos reads).
  void dispatch(BlockId at, std::vector<BlockId> targets) {
    const uint64_t va = function.block(at).va;
    const il::OpId brind =
        function.appendIndirectBranch(at, va, function.undefined(Type::integer(64)));
    function.setTargets(brind, std::move(targets));
  }

  ExprId i64(uint64_t value) { return function.constant(Type::integer(64), value); }
  ExprId cond() {
    return function.binary(ExprOp::CmpNe, function.entryReg(function.registers().find("x0")),
                           i64(0));
  }

  /// A single two-way *resolved* table dispatch at `at` -- same shape as
  /// test_structure_join_epilogue.cpp's own `twoWaySite` -- so switchFor's
  /// 2-way collapse (structure.cpp) turns it into a plain `If` instead of
  /// the unresolved `dispatch()` helper's table-mode `Switch`.
  void twoWaySite(BlockId at, uint64_t tableBase, uint64_t smaller, uint64_t larger,
                   BlockId smallerTarget, BlockId largerTarget) {
    const uint64_t va = function.block(at).va;
    const ExprId state = function.select(cond(), i64(smaller), i64(larger));
    const il::ValueId loaded = function.appendLoad(
        at, va, Type::integer(64),
        function.binary(ExprOp::Add, i64(tableBase), function.binary(ExprOp::Shl, state, i64(3))));
    const il::OpId brind = function.appendIndirectBranch(at, va + 4, function.valueRef(loaded));
    function.setTargets(brind, std::vector<BlockId>{smallerTarget, largerTarget});
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

/// The `Switch` statement built from `dispatcher`, wherever it ended up in
/// the tree -- nested inside another switch's own case body counts, since a
/// claimed case's content is exactly where a further dispatch chain lives.
const Stmt* findSwitch(const Stmt* node, BlockId dispatcher) {
  if (node == nullptr) {
    return nullptr;
  }
  if (node->kind == StmtKind::Switch && node->block == dispatcher) {
    return node;
  }
  for (const auto& item : node->items) {
    if (const Stmt* found = findSwitch(item.get(), dispatcher)) return found;
  }
  if (const Stmt* found = findSwitch(node->thenArm.get(), dispatcher)) return found;
  if (const Stmt* found = findSwitch(node->elseArm.get(), dispatcher)) return found;
  if (const Stmt* found = findSwitch(node->body.get(), dispatcher)) return found;
  for (const auto& body : node->caseBodies) {
    if (const Stmt* found = findSwitch(body.get(), dispatcher)) return found;
  }
  if (const Stmt* found = findSwitch(node->defaultBody.get(), dispatcher)) return found;
  if (const Stmt* found = findSwitch(node->epilogue.get(), dispatcher)) return found;
  return nullptr;
}

/// The `If` statement built from `dispatcher`, wherever it ended up in the
/// tree -- same traversal shape as `findSwitch`.
const Stmt* findIf(const Stmt* node, BlockId dispatcher) {
  if (node == nullptr) {
    return nullptr;
  }
  if (node->kind == StmtKind::If && node->block == dispatcher) {
    return node;
  }
  for (const auto& item : node->items) {
    if (const Stmt* found = findIf(item.get(), dispatcher)) return found;
  }
  if (const Stmt* found = findIf(node->thenArm.get(), dispatcher)) return found;
  if (const Stmt* found = findIf(node->elseArm.get(), dispatcher)) return found;
  if (const Stmt* found = findIf(node->body.get(), dispatcher)) return found;
  for (const auto& body : node->caseBodies) {
    if (const Stmt* found = findIf(body.get(), dispatcher)) return found;
  }
  if (const Stmt* found = findIf(node->defaultBody.get(), dispatcher)) return found;
  if (const Stmt* found = findIf(node->epilogue.get(), dispatcher)) return found;
  return nullptr;
}

/// The first `While`/`DoWhile` anywhere in `node`'s own tree, same traversal
/// shape as `findSwitch`.
const Stmt* findLoop(const Stmt* node) {
  if (node == nullptr) {
    return nullptr;
  }
  if (node->kind == StmtKind::While || node->kind == StmtKind::DoWhile) {
    return node;
  }
  for (const auto& item : node->items) {
    if (const Stmt* found = findLoop(item.get())) return found;
  }
  if (const Stmt* found = findLoop(node->thenArm.get())) return found;
  if (const Stmt* found = findLoop(node->elseArm.get())) return found;
  if (const Stmt* found = findLoop(node->body.get())) return found;
  for (const auto& body : node->caseBodies) {
    if (const Stmt* found = findLoop(body.get())) return found;
  }
  if (const Stmt* found = findLoop(node->defaultBody.get())) return found;
  if (const Stmt* found = findLoop(node->epilogue.get())) return found;
  return nullptr;
}

/// Whether `node`'s own tree still names `target` with a plain `Goto` --
/// same shape as `collectReferences`, but a predicate rather than a
/// collector, since these tests only ever need to ask about one block at a
/// time.
bool hasGotoTo(const Stmt* node, BlockId target) {
  if (node == nullptr) {
    return false;
  }
  if (node->kind == StmtKind::Goto && node->block == target) {
    return true;
  }
  for (const auto& item : node->items) {
    if (hasGotoTo(item.get(), target)) return true;
  }
  if (hasGotoTo(node->thenArm.get(), target)) return true;
  if (hasGotoTo(node->elseArm.get(), target)) return true;
  if (hasGotoTo(node->body.get(), target)) return true;
  for (const auto& body : node->caseBodies) {
    if (hasGotoTo(body.get(), target)) return true;
  }
  if (hasGotoTo(node->defaultBody.get(), target)) return true;
  return hasGotoTo(node->epilogue.get(), target);
}

/// Whether `node`'s own tree holds a `Continue` naming `header` -- the back
/// edge a loop's own body converts a matching `Goto` into, wherever in the
/// (possibly cloned, possibly reshuffled by which case ended up owning
/// which fallthrough neighbor) tree it ended up.
bool hasContinueTo(const Stmt* node, BlockId header) {
  if (node == nullptr) {
    return false;
  }
  if (node->kind == StmtKind::Continue && node->block == header) {
    return true;
  }
  for (const auto& item : node->items) {
    if (hasContinueTo(item.get(), header)) return true;
  }
  if (hasContinueTo(node->thenArm.get(), header)) return true;
  if (hasContinueTo(node->elseArm.get(), header)) return true;
  if (hasContinueTo(node->body.get(), header)) return true;
  for (const auto& body : node->caseBodies) {
    if (hasContinueTo(body.get(), header)) return true;
  }
  if (hasContinueTo(node->defaultBody.get(), header)) return true;
  return hasContinueTo(node->epilogue.get(), header);
}

}  // namespace

TEST_CASE("a switch case whose sole unclaimed target is an otherwise-orphaned "
          "single-reference handler folds it in as the case body",
          "[emit][structure][unique-goto-expand]") {
  // `dispatcher` dispatches to `handler`/`exit`. `handler` falls straight
  // into `relay`, which falls into `sink`; `exit` falls straight into `sink`
  // too. `sink` therefore has two real predecessors neither of which is
  // `dispatcher` itself, so claimCaseBody's own regionClosed check rejects
  // both `handler`'s and `exit`'s claim attempts (each one's speculative
  // walk absorbs `sink`, only to discover the other's edge into it) --
  // exactly the shape that leaves each as its own untouched, labelled
  // top-level group for the RPO sweep to pick up, and expandUniqueCaseGotos
  // to fold back into the case that is now its only reference. The extra
  // `relay` hop keeps `handler` from being a plain single-branch vote for
  // `sink` alongside `exit` -- two such votes would make
  // analysis::matchDispatcherShape claim both as a shared-epilogue dispatch
  // before either ever became an orphan.
  Fixture f;
  const BlockId dispatcher = f.block(0x2000);
  const BlockId handler = f.block(0x3000);
  const BlockId relay = f.block(0x3010);
  const BlockId exit = f.block(0x3100);
  const BlockId sink = f.block(0x4000);
  f.function.appendBranch(f.entry, 0x1000, dispatcher);
  f.dispatch(dispatcher, {handler, exit});
  f.function.appendBranch(handler, 0x3000, relay);
  f.function.appendBranch(relay, 0x3010, sink);
  f.function.appendBranch(exit, 0x3100, sink);
  f.function.appendReturn(sink, 0x4000);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);

  // Every block still prints exactly once -- folding a group in is moving
  // it, never copying or dropping it.
  for (const BlockId member : {dispatcher, handler, relay, exit, sink}) {
    CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), member) == 1);
  }
  const Stmt* switchStmt = findSwitch(result.root.get(), dispatcher);
  REQUIRE(switchStmt != nullptr);
  REQUIRE(switchStmt->cases.size() == 2);
  REQUIRE(switchStmt->caseBodies.size() == 2);
  const std::size_t handlerIndex = switchStmt->cases[0] == handler ? 0 : 1;
  const std::size_t exitIndex = 1 - handlerIndex;
  // Both cases folded in: neither is a bare goto slot any more.
  CHECK(switchStmt->caseBodies[handlerIndex] != nullptr);
  CHECK(switchStmt->caseBodies[exitIndex] != nullptr);
  CHECK_FALSE(result.isLabeled(handler));
  CHECK_FALSE(result.isLabeled(exit));
  // `sink` is folded whole into `handler`'s own case as plain fallthrough
  // content, but `exit`'s folded case still reaches it with an explicit
  // `goto` (its own group never absorbed `sink`, since `handler`'s free walk
  // got there first) -- it keeps its label.
  CHECK(result.isLabeled(sink));
}

TEST_CASE("a handler reached from two separate dispatch sites is never "
          "folded into either one's case",
          "[emit][structure][unique-goto-expand]") {
  // `shared`'s own IL predecessors are `outer` and `inner` both -- a real
  // second reference, not one this pass' own tree scan happens to miss --
  // so expandUniqueCaseGotos' IL-level predecessor check has to refuse it
  // regardless of anything else. `inner` itself is claimed inline as
  // `outer`'s other case (its own only predecessor is `outer`, and nothing
  // it absorbs escapes that claim), so `shared`'s two references end up one
  // inside `inner`'s nested switch and one in `outer`'s own -- the second
  // reference this test exists to prove still blocks the fold even though
  // it is not sitting next to the first one in the tree.
  Fixture f;
  const BlockId outer = f.block(0x2000);
  const BlockId inner = f.block(0x2100);
  const BlockId shared = f.block(0x3000);
  const BlockId innerExit = f.block(0x3100);
  f.function.appendBranch(f.entry, 0x1000, outer);
  f.dispatch(outer, {shared, inner});
  f.dispatch(inner, {shared, innerExit});
  f.function.appendReturn(shared, 0x3000);
  f.function.appendReturn(innerExit, 0x3100);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);
  CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), shared) == 1);
  CHECK(result.isLabeled(shared));

  const Stmt* outerSwitch = findSwitch(result.root.get(), outer);
  REQUIRE(outerSwitch != nullptr);
  const std::size_t outerSharedIndex = outerSwitch->cases[0] == shared ? 0 : 1;
  CHECK(outerSwitch->caseBodies[outerSharedIndex] == nullptr);

  const Stmt* innerSwitch = findSwitch(result.root.get(), inner);
  REQUIRE(innerSwitch != nullptr);
  const std::size_t innerSharedIndex = innerSwitch->cases[0] == shared ? 0 : 1;
  CHECK(innerSwitch->caseBodies[innerSharedIndex] == nullptr);
}

TEST_CASE("a handler whose block has no terminator of its own is never "
          "folded in",
          "[emit][structure][unique-goto-expand]") {
  // `handler` is created but never given any ops at all, so
  // `Structurizer::terminatorOf` finds nothing and `emitRegion`'s walk stops
  // right there with a bare `Block` -- `alwaysLeaves` reads that as falling
  // off the end rather than leaving, which has to disqualify the fold
  // regardless of `handler`'s single predecessor and its total absence of a
  // second reference.
  Fixture f;
  const BlockId dispatcher = f.block(0x2000);
  const BlockId handler = f.block(0x3000);
  const BlockId exit = f.block(0x3100);
  f.function.appendBranch(f.entry, 0x1000, dispatcher);
  f.dispatch(dispatcher, {handler, exit});
  f.function.appendReturn(exit, 0x3100);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  CHECK(result.isLabeled(handler));

  const Stmt* switchStmt = findSwitch(result.root.get(), dispatcher);
  REQUIRE(switchStmt != nullptr);
  const std::size_t handlerIndex = switchStmt->cases[0] == handler ? 0 : 1;
  CHECK(switchStmt->caseBodies[handlerIndex] == nullptr);
}

TEST_CASE("a handler that is itself a further dispatch is never folded in",
          "[emit][structure][unique-goto-expand]") {
  // `handler` is itself a second dispatch, `a`/`b` both falling into
  // `tail`, which also has `exit` (this switch's other case) as a
  // predecessor -- the same regionClosed conflict as the positive case
  // above, so `handler` fails to claim at the outer switch and ends up its
  // own top-level group once the RPO sweep gets to it, that group's last
  // statement is the nested switch it holds, ending in a real `return`
  // through its own shared epilogue (`alwaysLeaves` is true). Only
  // `containsSwitch` stands between it and folding in regardless -- proving
  // that check does its own, independent job rather than merely restating
  // what `alwaysLeaves` already refused.
  Fixture f;
  const BlockId dispatcher = f.block(0x2000);
  const BlockId handler = f.block(0x3000);
  const BlockId a = f.block(0x3010);
  const BlockId b = f.block(0x3020);
  const BlockId tail = f.block(0x3030);
  const BlockId exit = f.block(0x3100);
  f.function.appendBranch(f.entry, 0x1000, dispatcher);
  f.dispatch(dispatcher, {handler, exit});
  f.dispatch(handler, {a, b});
  f.function.appendBranch(a, 0x3010, tail);
  f.function.appendBranch(b, 0x3020, tail);
  f.function.appendBranch(exit, 0x3100, tail);
  f.function.appendReturn(tail, 0x3030);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  CHECK(result.isLabeled(handler));

  const Stmt* outerSwitch = findSwitch(result.root.get(), dispatcher);
  REQUIRE(outerSwitch != nullptr);
  const std::size_t handlerIndex = outerSwitch->cases[0] == handler ? 0 : 1;
  CHECK(outerSwitch->caseBodies[handlerIndex] == nullptr);

  // `handler`'s own nested switch still exists somewhere in the tree,
  // proving it really did leave cleanly through its own epilogue -- it was
  // `containsSwitch` alone that refused the fold above.
  const Stmt* innerSwitch = findSwitch(result.root.get(), handler);
  REQUIRE(innerSwitch != nullptr);
}

TEST_CASE("both cases of a dispatch fold into the switch before J2f gets a "
          "chance to merge them into the loop body they also belong to",
          "[emit][structure][unique-goto-expand]") {
  // `dispatcher` dispatches to `handlerA`/`handlerB`. `handlerA` falls
  // through `relayA` into `merge`; `handlerB` also falls straight into
  // `merge`, so -- same trick as the very first test above -- `merge`'s two
  // real predecessors make claimCaseBody's speculative walk fail for
  // *both* cases (whichever it tries first finds the other's edge still
  // outside its own trail) and each ends up its own labelled top-level
  // group once the RPO sweep gets to it. `merge` itself then closes a back
  // edge straight to `dispatcher`, so `dispatcher` heads a natural loop
  // whose blocks are every one of {merge, relayA, handlerB, handlerA} --
  // this is exactly the scatter-dispatcher shape `docs/
  // architecture-optimization-eval-prompt.md` §6.8 found in
  // sample_libscplugin: collapseLabeledNaturalLoops (J2f) merging a
  // case's own single-reference remnant whole into the loop body it also
  // happens to sit in, before expandUniqueCaseGotos (J2g) ever got a
  // chance to fold it into the case that was its only real reference. J2g
  // now runs once *before* J2f gets to decide anything, so both `handlerA`
  // and `handlerB` fold into their own cases first; nothing is left over
  // for J2f to merge into a loop body at all, yet the back edge `merge`
  // closes with -- now sitting inside `handlerA`'s cloned case body rather
  // than a standalone remnant -- still needs to end up a `continue` inside
  // a real `while (true)`, which only J2f's own second pass (run again
  // after J2f, see Structurizer::run) can still deliver once the loop
  // header's own group holds the whole switch outright.
  Fixture f;
  const BlockId dispatcher = f.block(0x2000);
  const BlockId handlerA = f.block(0x3000);
  const BlockId relayA = f.block(0x3010);
  const BlockId handlerB = f.block(0x3100);
  const BlockId merge = f.block(0x4000);
  f.function.appendBranch(f.entry, 0x1000, dispatcher);
  f.dispatch(dispatcher, {handlerA, handlerB});
  f.function.appendBranch(handlerA, 0x3000, relayA);
  f.function.appendBranch(relayA, 0x3010, merge);
  f.function.appendBranch(handlerB, 0x3100, merge);
  f.function.appendBranch(merge, 0x4000, dispatcher);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);

  // Every block still prints exactly once: J2g's fold moved each group, and
  // J2f's later loop merge (of what J2g leaves it) moves the rest -- neither
  // step is a copy.
  for (const BlockId member : {dispatcher, handlerA, relayA, handlerB, merge}) {
    CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), member) == 1);
  }

  const Stmt* switchStmt = findSwitch(result.root.get(), dispatcher);
  REQUIRE(switchStmt != nullptr);
  REQUIRE(switchStmt->cases.size() == 2);
  REQUIRE(switchStmt->caseBodies.size() == 2);
  const std::size_t handlerAIndex = switchStmt->cases[0] == handlerA ? 0 : 1;
  const std::size_t handlerBIndex = 1 - handlerAIndex;
  // Both cases folded in: J2g claimed each before J2f ever saw them as loop
  // members to merge instead.
  CHECK(switchStmt->caseBodies[handlerAIndex] != nullptr);
  CHECK(switchStmt->caseBodies[handlerBIndex] != nullptr);
  CHECK_FALSE(result.isLabeled(handlerA));
  CHECK_FALSE(result.isLabeled(handlerB));
  // `merge` is folded whole into `handlerA`'s own case as plain fallthrough
  // content (mirroring `sink` in the very first test), but `handlerB`'s
  // folded case still reaches it with an explicit `goto` -- it keeps its
  // label.
  CHECK(result.isLabeled(merge));

  // The loop J2f exists to find is still there: `dispatcher`'s own group
  // still becomes a real `while (true)`, its body still the switch, even
  // though every one of the loop's other blocks got claimed away by J2g
  // first and none were left standing for J2f's own merge step to do
  // anything with.
  const Stmt* loop = findLoop(result.root.get());
  REQUIRE(loop != nullptr);
  CHECK(loop->block == dispatcher);
  const Stmt* loopSwitch = findSwitch(loop->body.get(), dispatcher);
  CHECK(loopSwitch == switchStmt);

  // The back edge `merge` used to close with a plain `goto dispatcher` --
  // now sitting inside whichever of the two folded case bodies' own claim
  // walk happened to reach `merge` first (that ordering is RPO's to decide,
  // not this test's) -- reads as `continue` once it is inside that
  // `while (true)`, exactly as it would have if `merge` had kept its own
  // standalone remnant group for J2f to convert directly, and nothing
  // anywhere in the tree is still a bare `goto dispatcher` left unconverted.
  CHECK(hasContinueTo(switchStmt, dispatcher));
  CHECK_FALSE(hasGotoTo(switchStmt, dispatcher));
}

TEST_CASE("an If arm that fell back to a bare goto folds its single-reference "
          "orphan target in exactly as a switch case would",
          "[emit][structure][unique-goto-expand]") {
  // Phase 5 (goto-elimination plan §Phase5, J2g extension): `dispatcher` is
  // a resolved two-way table dispatch (see `twoWaySite`), so switchFor's own
  // 2-way collapse (structure.cpp) turns it into a plain `If` rather than a
  // `Switch`. There is only ever this one site, so `joinHubByTail` finds no
  // pooled hub for either arm and the `If` gets no epilogue of its own.
  // `handler`'s only real predecessor is `dispatcher` itself, so
  // claimCaseBody's speculative walk into handler->relay->sink fails only
  // because `sink` also has `exit` as a real predecessor (regionClosed) --
  // the exact same conflict the very first switch test above relies on --
  // and claimOrCloneSharedCaseBody refuses too, since that path only ever
  // fires for a handler with 2+ predecessors of its own. Both arms fall
  // back to switchFor's last-resort bare `gotoStmt`, leaving `handler` and
  // `exit` each their own labelled top-level remnant for this phase's own
  // extension of `expandGotoTargets` (structure.cpp) to fold straight back
  // into the `If`'s arms.
  Fixture f;
  const BlockId dispatcher = f.entry;
  const BlockId handler = f.block(0x3000);
  const BlockId relay = f.block(0x3010);
  const BlockId exit = f.block(0x3100);
  const BlockId sink = f.block(0x4000);
  constexpr uint64_t tableBase = 0x30b7f0;
  f.twoWaySite(dispatcher, tableBase, 0x10, 0x20, handler, exit);
  f.function.appendBranch(handler, 0x3000, relay);
  f.function.appendBranch(relay, 0x3010, sink);
  f.function.appendBranch(exit, 0x3100, sink);
  f.function.appendReturn(sink, 0x4000);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);

  // Every block still prints exactly once: folding an arm's goto target in
  // moves its group, never copies or drops it.
  for (const BlockId member : {dispatcher, handler, relay, exit, sink}) {
    CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), member) == 1);
  }
  const Stmt* ifStmt = findIf(result.root.get(), dispatcher);
  REQUIRE(ifStmt != nullptr);
  // Neither arm is still a bare `goto` naming its target -- both folded in.
  REQUIRE(ifStmt->thenArm != nullptr);
  REQUIRE(ifStmt->elseArm != nullptr);
  CHECK(ifStmt->thenArm->kind != StmtKind::Goto);
  CHECK(ifStmt->elseArm->kind != StmtKind::Goto);
  CHECK_FALSE(result.isLabeled(handler));
  CHECK_FALSE(result.isLabeled(exit));
  // `sink` is folded whole into `handler`'s own arm as plain fallthrough
  // content, but `exit`'s folded arm still reaches it with an explicit
  // `goto` (its own group never absorbed `sink`, since `handler`'s free
  // walk got there first) -- it keeps its label.
  CHECK(result.isLabeled(sink));
}

TEST_CASE("an If arm's bare goto is left alone when its target is reached "
          "from two separate dispatch sites",
          "[emit][structure][unique-goto-expand]") {
  // Same trick as "a handler reached from two separate dispatch sites..."
  // above, but `outer` is a resolved two-way table dispatch (see
  // `twoWaySite`), so it collapses to a plain `If` rather than a `Switch`.
  // `inner` stays the plain unresolved `dispatch()` from that earlier test
  // (a real second reference this pass' own tree scan cannot miss, but not
  // itself a resolved two-way site -- so claimOrCloneSharedCaseBody's own
  // "every predecessor must be a matching resolved dispatch" check refuses
  // `shared`, unlike the sibling test above where both sides qualified and
  // it legitimately cloned instead of leaving a goto). `shared` therefore
  // has to stay labelled, and `outer`'s own `If` arm naming it has to stay
  // a bare `goto` rather than fold in.
  Fixture f;
  const BlockId outer = f.entry;
  const BlockId inner = f.block(0x2100);
  const BlockId shared = f.block(0x3000);
  const BlockId innerExit = f.block(0x3100);
  constexpr uint64_t outerTable = 0x30b7f0;
  f.twoWaySite(outer, outerTable, 0x10, 0x20, shared, inner);
  f.dispatch(inner, {shared, innerExit});
  f.function.appendReturn(shared, 0x3000);
  f.function.appendReturn(innerExit, 0x3100);
  f.function.rebuildEdges();

  const StructuredFunction result = run(f.function);
  Walk walk;
  walk.visit(result.root);
  CHECK(std::count(walk.blocks.begin(), walk.blocks.end(), shared) == 1);
  CHECK(result.isLabeled(shared));

  const Stmt* outerIf = findIf(result.root.get(), outer);
  REQUIRE(outerIf != nullptr);
  const bool outerThenIsSharedGoto = outerIf->thenArm && outerIf->thenArm->kind == StmtKind::Goto &&
                                      outerIf->thenArm->block == shared;
  const bool outerElseIsSharedGoto = outerIf->elseArm && outerIf->elseArm->kind == StmtKind::Goto &&
                                      outerIf->elseArm->block == shared;
  CHECK((outerThenIsSharedGoto || outerElseIsSharedGoto));

  const Stmt* innerSwitch = findSwitch(result.root.get(), inner);
  REQUIRE(innerSwitch != nullptr);
  const std::size_t innerSharedIndex = innerSwitch->cases[0] == shared ? 0 : 1;
  CHECK(innerSwitch->caseBodies[innerSharedIndex] == nullptr);
}
