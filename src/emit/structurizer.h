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
#include <vector>

#include "xdec/emit/structure.h"

namespace xdec::emit {

constexpr unsigned kMaxDepth = 32;

class Structurizer {
 public:
  Structurizer(const il::Function& function, const analysis::Dominators& dominators,
               const analysis::PostDominators& postDominators,
               std::span<const analysis::NaturalLoop> loops);

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

