// The structurizer's shared state.
//
// Split by concern: the region walk and the two textbook patterns (diamonds and
// natural loops) live in structure.cpp, the flattening-dispatcher recognition in
// structure_dispatch.cpp. They share one object because they share one claim
// set — a pattern may only take blocks no other pattern has taken, and any
// pattern that fails must give its blocks back.
#pragma once

#include <map>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <vector>

#include "xdec/analysis/dispatch_region.h"
#include "xdec/emit/structure.h"

namespace xdec::emit {

constexpr unsigned kMaxDepth = 32;

class Structurizer {
 public:
  Structurizer(const il::Function& function, const analysis::Dominators& dominators,
               const analysis::PostDominators& postDominators,
               std::span<const analysis::NaturalLoop> loops,
               const StructureOptions& options = {});

  StructuredFunction run();

 private:
  // -- region walking (structure.cpp) -----------------------------------------

  /// Emits blocks from `cur` until `stop`, an already-claimed block, or a dead
  /// end. Records where the walk stopped in regionEnd_.
  StmtPtr emitRegion(il::BlockId cur, il::BlockId stop, unsigned depth);

  /// Diamond speculation: both arms walk to the immediate post-dominator and
  /// no block inside has a predecessor from outside the region.
  StmtPtr tryDiamond(il::BlockId head, il::ExprId cond, il::BlockId taken,
                     il::BlockId untaken, unsigned depth);

  /// Diamond's fallback: no shared post-dominator is required. One arm is
  /// walked as a self-contained region (own predecessors only); if it turns
  /// out to flow into the other arm, that's a plain `if` with the other arm
  /// as fallthrough; if it dead-ends instead (return, unreachable, or its own
  /// unresolved branch), that's a one-sided `if` guarding the rest of the
  /// function. Handles the common "early exit" shape that diamond's strict
  /// dominance-based merge cannot: an arm and its continuation don't share an
  /// immediate post-dominator once one side leaves through a different exit.
  StmtPtr tryOneSided(il::BlockId head, il::ExprId cond, il::BlockId taken,
                      il::BlockId untaken, unsigned depth);

  /// Two nested guards whose failure arms share one fallback body (see
  /// analysis::GuardCascadeShape) -- the shape a diamond cannot represent,
  /// since a diamond's arms each claim their own blocks and a fallback with
  /// two predecessors is not that. Builds the inner `if` directly rather
  /// than recursing through `emitRegion` for it, since the fallback must be
  /// claimed (walked, marked) exactly once and then *printed* twice, once
  /// per guard's failure arm -- a plain recursive walk would instead try to
  /// claim it a second time and fail the same way tryDiamond already does.
  StmtPtr tryGuardCascade(il::BlockId head, unsigned depth);

  /// Walks and marks `fallback` up to (not including) `merge`, exactly once,
  /// for `tryGuardCascade` to embed under both guards' failure arms -- the
  /// second embedding is a deep copy of the statement tree this returns, not
  /// a second walk, so the blocks themselves are still claimed only once.
  StmtPtr claimSharedFallbackBody(il::BlockId fallback, il::BlockId merge, unsigned depth);

  /// Loop speculation: a while loop when the header is a bare conditional, a
  /// do-while when the latch is.
  StmtPtr tryLoop(const analysis::NaturalLoop& loop, unsigned depth);

  /// Loop form for the OLLVM state-machine shape: a guard header, a resolved
  /// switch whose cases mostly fall through one shared tail (see
  /// analysis::DispatcherShape), and that tail's own unconditional jump back
  /// to the header. Built as one region in a single pass -- the guard, the
  /// switch and the tail all have to become one region's blocks together or
  /// not at all, since `regionClosed` demands every predecessor of the tail
  /// be accounted for either way.
  StmtPtr tryDispatcherLoop(const analysis::NaturalLoop& loop, unsigned depth);

  /// J2f (docs/architecture-optimization-eval-prompt.md §6.5): folds a
  /// natural loop's own remaining top-level `groups` entries -- the header's
  /// and every other member's, each left as its own untouched, labelled
  /// remnant because the header's terminator (almost always a resolved
  /// dispatch, not a `CondBranch`) never matched tryLoop's or
  /// tryDispatcherLoop's shapes and wrapAsLoop only ever looks inside the one
  /// switch it just built, not at a member reached through several other
  /// handlers first -- into one `while (true)` whose jumps back to the
  /// header print as `continue`. Only fires when every one of the loop's
  /// blocks is still exactly that: its own untouched top-level entry, none
  /// of them holding a loop of their own (a `goto header` inside one would
  /// belong to *that* loop's own `continue`, not this one's). Mutates
  /// `groups` in place, erasing every entry this absorbs; the caller's own
  /// rank-based sort still places the surviving, now-merged entry correctly
  /// since it keeps the header's original `BlockId`.
  void collapseLabeledNaturalLoops(std::vector<std::pair<il::BlockId, StmtPtr>>& groups);

  /// The function's dispatch regions (see analysis::findDispatchRegions),
  /// computed at most once per Structurizer instance regardless of how many
  /// of the two callers below end up asking for it: `tryDispatcherLoop`
  /// reaches for it only once its own single-block vote
  /// (analysis::matchDispatcherShape) has already failed, but `switchFor`'s
  /// `isMemberOfLargeDispatchRegion` asks on every resolved two-target table
  /// dispatch site -- still one scan of the function, not one per site.
  const std::vector<analysis::DispatchRegion>& dispatchRegions();

  /// J1's own collapse gate (see StructureOptions, structure.cpp's switchFor):
  /// whether `block` -- a resolved two-target table dispatch's own site --
  /// is a member of a dispatch region large enough that collapsing it to
  /// `if`/`else` would lose the table identity `dispatchRegions()` already
  /// recovered. `options_.deferRegionCollapse` widens "large enough" to any
  /// region at all, for a fixture too small to reach `minRegionSites`
  /// organically.
  [[nodiscard]] bool isMemberOfLargeDispatchRegion(il::BlockId block);

  /// tryDispatcherLoop's fallback for a `dispatch` whose own target count
  /// never clears matchDispatcherShape's floor: looks for a region `dispatch`
  /// belongs to and asks analysis::confirmDispatcherShapeFromRegion to
  /// verify `dispatch`'s own targets against that region's shared tail.
  std::optional<analysis::DispatcherShape> matchRegionConfirmedShape(
      il::BlockId dispatch, std::span<const il::BlockId> targets);

  /// J2e (docs/architecture-optimization-eval-prompt.md §6.3): every join
  /// hub across every one of `dispatchRegions()`'s own regions, computed at
  /// most once per Structurizer instance and indexed by tail block for
  /// `switchFor`'s per-target lookup -- the same one-scan-not-one-per-site
  /// discipline `dispatchRegions()` itself already holds to.
  const std::map<il::BlockId, il::BlockId>& joinHubByTail();

  /// J2 (docs/architecture-optimization-eval-prompt.md §3 Phase 3,
  /// StructureOptions::regionStructuring): flattens `stmt` -- a table-mode
  /// Switch `switchFor` just finished building for `dispatch` -- against any
  /// of its own case bodies that turn out to be exactly one more resolved
  /// dispatch site of the same `region`, reached privately and reading the
  /// identical already-evaluated discriminant (`Stmt::cond`, not merely a
  /// structurally similar one). See structure_dispatch_region.cpp's own
  /// comment for why that exact-`ExprId` requirement is load-bearing: two
  /// different reads of "the same logical state variable" can (and in a
  /// flattening dispatcher's own state-transition handlers routinely do)
  /// hold different values by the second read, and folding those into one
  /// `switch (cond)` evaluated exactly once would misdescribe control flow
  /// that really re-reads and re-branches. Mutates `stmt` in place;
  /// recurses into whatever it just spliced in, so a chain three or more
  /// sites deep flattens in one call. Returns how many case slots were
  /// absorbed this way (0 when nothing in `stmt` qualified).
  std::size_t collapseRegionDispatchTree(Stmt& stmt, const analysis::DispatchRegion& region);

  StmtPtr gotoChain(il::ExprId cond, il::BlockId taken, il::BlockId untaken);
  StmtPtr gotoStmt(il::BlockId target);

  /// A resolved computed branch: a switch over the table index when the shape
  /// matched, a compare chain over the target otherwise.
  StmtPtr switchFor(il::BlockId block, const il::Op& op, unsigned depth);

  /// Structures a case's handler into the case itself, or nothing when the
  /// handler is shared and so has to keep its own label.
  StmtPtr claimCaseBody(il::BlockId dispatcher, il::BlockId handler, unsigned depth);

  /// Like `claimCaseBody`, but for a handler that flows into a dispatcher
  /// shape's shared tail `merge` (see analysis::DispatcherShape) rather than
  /// leaving on its own: the body is walked only up to `merge`, not into it,
  /// and (when `appendBreak`) closed off with a `Break` instead of requiring
  /// the handler itself to end in a return/goto/switch. `appendBreak` is
  /// false for `tryDispatcherLoop`'s three-way merge case: there, the
  /// caller's own `if`/`else` reaches `merge` structurally (as the shared
  /// code following both arms), so nothing needs to say so explicitly, and a
  /// `Break` outside of any switch would wrongly leave the enclosing loop
  /// instead.
  StmtPtr claimDispatcherCaseBody(il::BlockId dispatcher, il::BlockId handler,
                                  il::BlockId merge, unsigned depth,
                                  bool appendBreak = true);

  /// `claimCaseBody`'s same-target-duplication counterpart, for a resolved
  /// binary dispatch's handler that more than one such dispatch reaches
  /// (see switchFor's collapse to `If`). Every predecessor of `handler` must
  /// itself be a resolved two-target dispatch this collapse would
  /// recognise -- not merely more than one predecessor -- so this only ever
  /// fires inside the shape it was designed for. Claimed once (walked and
  /// size-capped exactly like `claimCaseBody`), then handed out as
  /// independent `cloneStmt` copies to every caller, first included, so no
  /// caller's tree shares nodes with another's.
  StmtPtr claimOrCloneSharedCaseBody(il::BlockId dispatcher, il::BlockId handler,
                                     unsigned depth);

  /// claimOrCloneSharedCaseBody's cheap, read-only guard: whether a resolved
  /// table dispatch is reachable from `start` within `budget` successor
  /// edges, checked by walking `il::Function::block(...).successors`
  /// directly rather than through `emitRegion` -- no `mark`/`trail_`/
  /// `gotoTargets_` touched, so there is nothing to `rollback` even when
  /// this returns true. Exists because that cost matters here specifically:
  /// a scatter-dispatcher's own handler routinely does a couple of ops and
  /// dispatches again through the very same table, so `emitRegion` would
  /// have to walk (and speculatively claim, and print-check via
  /// containsSwitch, and only then roll back) that whole further switch
  /// before finding out this handler was never going to qualify -- and,
  /// worse, that walk's own nested pattern attempts leave `budget_` spent
  /// with nothing to show for it even after the rollback, unlike this
  /// check's cost, which is the same on every call.
  [[nodiscard]] bool reachesFurtherDispatch(il::BlockId start, unsigned budget) const;

  /// Whether control cannot reach the bottom of a statement and carry on past it.
  /// Conservative: unsure counts as "it can".
  [[nodiscard]] bool alwaysLeaves(const Stmt* node) const;

  // -- dispatcher trees (structure_dispatch.cpp) ------------------------------

  /// One test of a switch decision tree: a compare of the discriminant against
  /// a constant. An equality test names one case — `taken` is its handler —
  /// and leaves every other value to `next`. An ordered test splits the values
  /// in two and both arms carry on testing.
  struct ValueTest {
    il::ExprId spine{};
    bool equality = false;
    uint64_t value = 0;   // equality tests only
    il::BlockId taken{};  // equality: the handler; ordered: the true arm
    il::BlockId next{};   // equality: every other value; ordered: the false arm
  };

  /// Matches a block whose only real op is a two-way compare of one value
  /// against a constant. `allowPhis` holds for the block the tree is rooted
  /// at: its phis are still emitted by its predecessors, while a phi further
  /// down would lose its copies when the switch replaces the block.
  [[nodiscard]] std::optional<ValueTest> matchValueTest(il::BlockId blockId,
                                                       bool allowPhis) const;

  /// What a decision tree resolves to: the case values with their handlers and
  /// the test each one branched from, plus the single block every value no case
  /// names ends up at.
  struct ValueTree {
    il::ExprId spine{};
    std::vector<uint64_t> values;
    std::vector<il::BlockId> handlers;
    std::vector<il::BlockId> preds;
    il::BlockId defaultTarget{};
    std::vector<il::BlockId> defaultPreds;
    /// The test blocks the switch replaces, `head` first.
    std::vector<il::BlockId> absorbed;
  };

  /// Walks the tests reachable from `head`. False when they do not form one
  /// switch; nothing is claimed either way.
  [[nodiscard]] bool collectTree(il::BlockId head, ValueTree& tree) const;

  /// Recognises a decision tree over one value rooted at `head` and appends it
  /// to `seq` as one switch. False leaves everything untouched.
  bool tryDispatchTree(Stmt* seq, il::BlockId head, unsigned depth);

  /// Wraps a dispatch `stmt` in `while (true)` when `head` is a loop header the
  /// switch's own cases branch back to, rewriting those back edges as
  /// `continue`. Returns `stmt` unchanged when it is not that shape.
  StmtPtr wrapAsLoop(StmtPtr stmt, il::BlockId head);

  /// Turns every jump back to `header` inside `node` into a `continue`, and
  /// says whether it found any; stops at a nested loop rather than stealing
  /// its own back edge's `continue`. Defined in structure_dispatch.cpp
  /// (wrapAsLoop's original use); shared here so structure.cpp's
  /// collapseLabeledNaturalLoops (J2f) does not need its own copy.
  static bool continueAtBackEdges(Stmt* node, il::BlockId header);

  // -- helpers ----------------------------------------------------------------

  StmtPtr emitBlock(il::BlockId blockId);
  [[nodiscard]] const il::Op* terminatorOf(il::BlockId blockId) const;
  [[nodiscard]] bool hasBodyOps(il::BlockId blockId) const;

  /// Whether control leaves the function here rather than going on somewhere.
  [[nodiscard]] bool exits(il::BlockId blockId) const;

  /// Whether structuring this block could discover anything: a loop header, or a
  /// branch that might turn out to be a diamond, guard, or dispatcher. A block
  /// that only runs and falls through holds nothing to find, so starting a fresh
  /// walk at it can only take it away from whatever branch leads to it.
  [[nodiscard]] bool holdsAPattern(il::BlockId blockId) const;

  [[nodiscard]] bool hasPhis(il::BlockId blockId) const;
  void mark(il::BlockId blockId);

  /// Records a block as a goto/case target, tracked so a later rollback can
  /// take it back: a failed speculative walk must leave no trace, including
  /// the labels it would have forced on blocks it only visited in passing.
  void addGotoTarget(il::BlockId block);

  /// Undoes every `mark`/`addGotoTarget` since the matching snapshots — both
  /// must be taken together, right before the speculative work they guard.
  void rollback(std::size_t trailSnapshot, std::size_t gotoSnapshot);

  /// Every block claimed since `snapshot` must have all its predecessors
  /// inside the claimed set (the region head is the legitimate exception).
  [[nodiscard]] bool regionClosed(std::size_t snapshot, il::BlockId head) const;

  const il::Function& function_;
  const analysis::Dominators& dominators_;
  const analysis::PostDominators& postDominators_;
  std::map<il::BlockId, const analysis::NaturalLoop*> loopByHeader_;
  std::set<il::BlockId> emitted_;
  std::set<il::BlockId> gotoTargets_;
  std::set<il::BlockId> inProgressHeaders_;
  std::vector<il::BlockId> trail_;
  std::vector<il::BlockId> gotoTrail_;
  il::BlockId regionEnd_{};

  /// Canonical bodies claimed by `claimOrCloneSharedCaseBody`, keyed by
  /// handler: populated once per handler, on the first dispatch site that
  /// reaches it, and cloned for every call after (including that first one).
  std::map<il::BlockId, StmtPtr> sharedCaseBodyCache_;

  /// One entry per `sharedCaseBodyCache_` insertion, in insertion order:
  /// the claim's own pre-walk `trail_.size()` (its local `snapshot`) paired
  /// with the handler it cached. `rollback` needs this because a cache
  /// insertion can happen nested inside another pattern's speculative
  /// `emitRegion` walk (any of tryDiamond/tryOneSided/tryLoop/claimCaseBody/
  /// etc. can recurse through emitBlock/switchFor into this cache) -- if
  /// that *enclosing* attempt fails and rolls its own trail back, the
  /// nested claim's blocks stop being `emitted_` again, but without this,
  /// the cache entry itself would survive and keep handing out a clone of a
  /// body whose blocks the rest of the function no longer considers
  /// claimed, printing it both inline (stale, from the clone) and again at
  /// its own natural position (since nothing marked it emitted after the
  /// rollback) -- see rollback's own comment for the actual purge.
  std::vector<std::pair<std::size_t, il::BlockId>> sharedCaseBodyInsertions_;

  /// See dispatchRegions(). Absent until the first call.
  std::optional<std::vector<analysis::DispatchRegion>> dispatchRegions_;

  /// See joinHubByTail(). Absent until the first call.
  std::optional<std::map<il::BlockId, il::BlockId>> joinHubByTail_;

  /// See StructureOptions. Copied in at construction; never mutated after.
  StructureOptions options_;

  /// See `StructuredFunction::matchedPatterns`: one entry per `CondBranch`
  /// site actually claimed, appended right where `emitRegion` commits to
  /// that pattern -- never speculatively, so a rolled-back attempt leaves no
  /// trace here either, the same discipline `trail_`/`gotoTrail_` already
  /// keep. Moved into the result once in `run()`.
  std::vector<std::string_view> matchedPatterns_;

  // One-sided ifs require no shared merge point, so they are happy to claim
  // a block a real (postdominator-backed) diamond somewhere else would have
  // wanted for its arm or its merge — and once claimed, that diamond can
  // never form. `run` disables this pattern for a full pass over the
  // function so every ordinary diamond and dispatch chain gets first pick,
  // then enables it for a second pass over whatever is still unclaimed.
  bool oneSidedEnabled_ = false;

  // Bounds total speculative work. A failed diamond/one-sided attempt rolls
  // its walked blocks back so an ancestor can try something else with them —
  // exactly what makes a heavily-branching, flattened CFG cascade: the same
  // large region gets walked and discarded again by every enclosing attempt
  // that tries a different arm. Each block a speculative walk visits spends
  // from this budget; once it is gone, tryDiamond/tryOneSided stop attempting
  // new patterns (raw emission still always terminates), trading a bit of
  // structuring quality on pathological functions for a bounded running time.
  std::size_t budget_;
};

}  // namespace xdec::emit

