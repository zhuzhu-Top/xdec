// The region walk and the two textbook patterns: reconverging diamonds and
// natural loops. Dispatcher chains live in structure_dispatch.cpp; the header
// explains why they share one object.
#include "structurizer.h"

#include <algorithm>
#include <limits>

#include "xdec/analysis/dispatcher_shape.h"
#include "xdec/analysis/guard_cascade.h"
#include "xdec/analysis/jump_table.h"

namespace xdec::emit {

namespace {

/// RAII marker for a block currently being speculated on. Without this, a
/// pattern's recursive `emitRegion` calls can walk back around to `head`
/// itself — not yet in `emitted_`, since only a successful pattern marks it —
/// and try to speculate on it all over again, and again inside that, without
/// bound: exactly the back-edges-to-head that flattened dispatchers are full
/// of. Membership just says "don't reopen this as a fresh pattern site right
/// now"; a nested walk that reaches it still emits it plainly and moves on.
class ScopedHeader {
 public:
  ScopedHeader(std::set<il::BlockId>& set, il::BlockId block) : set_(set), block_(block) {
    set_.insert(block_);
  }
  ~ScopedHeader() { set_.erase(block_); }
  ScopedHeader(const ScopedHeader&) = delete;
  ScopedHeader& operator=(const ScopedHeader&) = delete;

 private:
  std::set<il::BlockId>& set_;
  il::BlockId block_;
};

}  // namespace

namespace {
// Generous enough that ordinary functions never feel it (a clean diamond
// visits each of its blocks once), but bounded so a function whose branches
// mostly fail to structure cannot cascade into unbounded backtracking.
constexpr std::size_t kBudgetPerBlock = 200;
}  // namespace

namespace {

/// A full, independent copy of `node`'s statement tree -- the way
/// tryGuardCascade prints a shared fallback body under two different guards'
/// failure arms without claiming its blocks a second time (see
/// claimSharedFallbackBody). Safe regardless of how deep or structured the
/// original walk found the body to be: nothing here re-inspects the CFG or
/// touches `emitted_`/`trail_`, it only duplicates the already-built nodes,
/// so whatever those blocks are is exactly what each copy prints.
StmtPtr cloneStmt(const Stmt* node) {
  if (node == nullptr) {
    return nullptr;
  }
  StmtPtr copy = Stmt::make(node->kind);
  copy->block = node->block;
  copy->cond = node->cond;
  copy->invertCond = node->invertCond;
  copy->tableMode = node->tableMode;
  copy->items.reserve(node->items.size());
  for (const StmtPtr& item : node->items) {
    copy->items.push_back(cloneStmt(item.get()));
  }
  copy->thenArm = cloneStmt(node->thenArm.get());
  copy->elseArm = cloneStmt(node->elseArm.get());
  copy->body = cloneStmt(node->body.get());
  copy->cases = node->cases;
  copy->caseBodies.reserve(node->caseBodies.size());
  for (const StmtPtr& body : node->caseBodies) {
    copy->caseBodies.push_back(cloneStmt(body.get()));
  }
  copy->caseValues = node->caseValues;
  copy->casePreds = node->casePreds;
  copy->defaultCase = node->defaultCase;
  copy->defaultPred = node->defaultPred;
  copy->defaultBody = cloneStmt(node->defaultBody.get());
  copy->epilogue = cloneStmt(node->epilogue.get());
  copy->mergeBlock = node->mergeBlock;
  copy->frame = node->frame;
  return copy;
}

/// The block `stmt` runs first when control falls into it with no jump of
/// its own — the same question `goto`-elision needs answered for whatever
/// sits right after a candidate goto, restricted to shapes that really do
/// have one deterministic entry. A `Goto` is deliberately excluded even
/// though it "leads to" a block: it is a jump, not a fallthrough, so letting
/// it count here would make the elision pass think a goto two hops away is
/// already redundant.
il::BlockId firstBlock(const Stmt* stmt) {
  if (stmt == nullptr) {
    return {};
  }
  switch (stmt->kind) {
    case StmtKind::Block:
      return stmt->block;
    case StmtKind::Sequence:
      return stmt->items.empty() ? il::BlockId{} : firstBlock(stmt->items.front().get());
    case StmtKind::DoWhile:
      // A do-while always runs its body first, header included.
      return firstBlock(stmt->body.get());
    default:
      return {};
  }
}

/// Drops a `goto target;` wherever `target` is exactly the block that runs
/// immediately afterwards anyway — the diamond/dispatch-chain fallback
/// (`gotoChain`) prints an explicit goto on both arms because it cannot tell
/// at the point either arm is built whether one of them will land on
/// whatever the surrounding region happens to walk into next; only once the
/// whole tree is assembled is "next" known. This is the same shape
/// `tryOneSided` already collapses, applied uniformly after the fact instead
/// of only where the structurizer's own claim order happened to leave an
/// opening for it.
///
/// `next` is the block that runs immediately after `node` finishes, if
/// control can reach that point without having already jumped elsewhere; an
/// invalid id means nothing is known to follow (a loop body's tail loops or
/// exits through its own condition, never a plain fallthrough).
void elideFallthroughGotos(StmtPtr& node, il::BlockId next) {
  if (!node) {
    return;
  }
  switch (node->kind) {
    case StmtKind::Goto:
      if (next.valid() && node->block == next) {
        // Nulling the arm — rather than leaving an empty Sequence in its
        // place — matters: printIf only reaches for its edge-copy fallback
        // (the same one an already-absent one-sided arm uses) when the arm
        // pointer itself is null.
        node.reset();
      }
      return;
    case StmtKind::Sequence: {
      std::vector<StmtPtr>& items = node->items;
      for (std::size_t index = 0; index < items.size(); ++index) {
        const il::BlockId childNext = index + 1 < items.size()
                                           ? firstBlock(items[index + 1].get())
                                           : next;
        elideFallthroughGotos(items[index], childNext);
      }
      items.erase(std::remove_if(items.begin(), items.end(),
                                  [](const StmtPtr& item) { return item == nullptr; }),
                  items.end());
      // Deliberately left as an empty (not null) Sequence: every other
      // printer that owns one — printWhile, printDoWhile, the root itself —
      // already treats "nothing to print" as the normal, expected shape of
      // an empty Sequence and prints it unconditionally. Only `If` arms give
      // null a second meaning (its own edge-copy fallback), so that
      // collapse happens there instead, right below.
      return;
    }
    case StmtKind::If: {
      // Either arm, once it finishes, reaches whatever follows the whole
      // `if` — the same `next` passed in here.
      elideFallthroughGotos(node->thenArm, next);
      elideFallthroughGotos(node->elseArm, next);
      // An arm left as an empty Sequence (its one goto just got elided, and
      // it had nothing else in it) means exactly what an already-absent arm
      // means: fall through to whichever edge nothing else claimed. printIf
      // only takes that path when the arm pointer itself is null, so an
      // empty one has to collapse here to actually get there.
      const auto isEmpty = [](const StmtPtr& arm) {
        return !arm || (arm->kind == StmtKind::Sequence && arm->items.empty());
      };
      if (isEmpty(node->thenArm)) {
        node->thenArm.reset();
      }
      if (isEmpty(node->elseArm)) {
        node->elseArm.reset();
      }
      // Both arms gone dark is a plain "if (cond) {}" with no need for
      // anyone to read it a second way; only worth reshaping when exactly
      // one side survived. An absent `then` with real content sitting in
      // `else` would otherwise print as empty braces followed by an
      // `else { ... }`, which reads as if the `else` were the exceptional
      // path — swapping the arm into `then` and negating `cond` says the
      // same thing as a single ordinary if.
      if (!node->thenArm && node->elseArm) {
        std::swap(node->thenArm, node->elseArm);
        node->invertCond = !node->invertCond;
      }
      return;
    }
    case StmtKind::While:
    case StmtKind::DoWhile:
      // The body's own tail never falls through to `next`: a while rechecks
      // its condition, a do-while rechecks its latch condition. Internal
      // fallthrough opportunities inside the body are still worth taking,
      // just relative to nothing outside it.
      elideFallthroughGotos(node->body, il::BlockId{});
      return;
    case StmtKind::Switch:
      // Nothing follows a case: the next thing in the text is the next case,
      // which is never where the case's own code goes. Only a fallthrough
      // wholly inside one case body is a fallthrough at all. The epilogue is
      // different -- it is what runs right after the switch as a whole, so it
      // inherits this switch's own `next`.
      for (StmtPtr& body : node->caseBodies) {
        elideFallthroughGotos(body, il::BlockId{});
      }
      elideFallthroughGotos(node->defaultBody, il::BlockId{});
      elideFallthroughGotos(node->epilogue, next);
      return;
    case StmtKind::Block:
    case StmtKind::Continue:
    case StmtKind::Break:
      // Neither names a block anything falls into, so there is no
      // fallthrough here to elide it against.
      return;
  }
}

/// Turns a `Goto` to `exit` into a `Break`, wherever it sits in a loop's own
/// body without crossing into a nested loop's or switch's control -- a plain
/// `break;` there would leave that inner construct rather than reach `exit`,
/// so those are left as the `goto`/label pair they already are. Applied right
/// after a loop pattern closes with `exit` as its proven, single successor
/// (see Structurizer::tryLoop): a body branch that already reaches it is not
/// telling the reader anything a `break` wouldn't, and every predecessor of
/// `exit` besides the loop itself already keeps its own path there uneffected
/// -- this only ever rewrites gotos this exact loop's body owns.
///
/// `block` is left set on the resulting `Break`, unlike a dispatcher case's
/// (see Stmt::epilogue): the loop's `exit` still has copies to print for
/// whichever edge just took it, exactly as its `Goto` would have, and
/// `StmtPrinter` reads it right back off for that (see printStmt's `Break`
/// case).
void rewriteLoopExitToBreak(StmtPtr& node, il::BlockId exit) {
  if (!node || !exit.valid()) {
    return;
  }
  switch (node->kind) {
    case StmtKind::Goto:
      if (node->block == exit) {
        node->kind = StmtKind::Break;
      }
      return;
    case StmtKind::Sequence:
      for (StmtPtr& item : node->items) {
        rewriteLoopExitToBreak(item, exit);
      }
      return;
    case StmtKind::If:
      rewriteLoopExitToBreak(node->thenArm, exit);
      rewriteLoopExitToBreak(node->elseArm, exit);
      return;
    default:
      // A nested While/DoWhile has its own exit, and a nested Switch's cases
      // end with their own Break bound to their own epilogue -- neither is
      // this loop's to rewrite.
      return;
  }
}

/// Every block the finished tree still names, as opposed to reaching by falling
/// into it. Only a `goto` and a `switch` case name one, so this is the whole of
/// what needs a label — a block with several predecessors that all fall into it
/// in the text needs none, and printing one anyway leaves a label nothing jumps
/// to, which reads like a missing jump rather than an absent one.
void collectReferences(const Stmt* node, std::set<il::BlockId>& out) {
  if (node == nullptr) {
    return;
  }
  if (node->kind == StmtKind::Goto) {
    out.insert(node->block);
  }
  if (node->kind == StmtKind::Switch) {
    for (std::size_t index = 0; index < node->cases.size(); ++index) {
      // A case that carries its handler reaches it by containing it.
      if (index < node->caseBodies.size() && node->caseBodies[index]) {
        collectReferences(node->caseBodies[index].get(), out);
        continue;
      }
      out.insert(node->cases[index]);
    }
    if (node->defaultBody) {
      collectReferences(node->defaultBody.get(), out);
    } else if (node->defaultCase.valid()) {
      out.insert(node->defaultCase);
    }
    collectReferences(node->epilogue.get(), out);
  }
  for (const StmtPtr& item : node->items) {
    collectReferences(item.get(), out);
  }
  collectReferences(node->thenArm.get(), out);
  collectReferences(node->elseArm.get(), out);
  collectReferences(node->body.get(), out);
}

}  // namespace

Structurizer::Structurizer(const il::Function& function,
                           const analysis::Dominators& dominators,
                           const analysis::PostDominators& postDominators,
                           std::span<const analysis::NaturalLoop> loops)
    : function_(function),
      dominators_(dominators),
      postDominators_(postDominators),
      budget_(static_cast<std::size_t>(function.blockCount()) * kBudgetPerBlock) {
  for (const analysis::NaturalLoop& loop : loops) {
    loopByHeader_[loop.header] = &loop;
  }
}

StructuredFunction Structurizer::run() {
  StructuredFunction result;
  // Each top-level walk is kept with the block it started from, and they are put
  // in reading order only once all of them exist. Appending them as they are
  // claimed instead orders the function by the structurizer's search, which has
  // nothing to do with how control flows: a guard's exit block, claimed early
  // because it happened to be reachable on its own, lands ahead of the entry, and
  // the function reads as a row of returns followed by the code that leads to
  // them. Reverse post-order is the reading order — it is the order the entry
  // reaches things — and it puts the entry first by construction.
  std::vector<std::pair<il::BlockId, StmtPtr>> groups;
  groups.emplace_back(function_.entryBlock(),
                      emitRegion(function_.entryBlock(), il::BlockId{}, 0));
  // The top-level walk stops the moment it meets a branch it cannot turn
  // into a diamond/loop/chain (both arms become goto targets and nothing
  // forces a single `cur` to keep going). Whatever is left in reverse
  // post-order gets its own fresh top-level walk — most of a flattened
  // function's blocks are reached this way, so without it almost nothing
  // downstream of the very first unstructured branch would ever get a
  // chance at forming a diamond or loop, and the whole body degrades into
  // one block-and-goto per block.
  //
  // Restricted here to blocks that hold something to find. This sweep exists to
  // reach patterns the entry walk could not, and claiming a block that holds no
  // pattern serves that not at all while permanently costing the branch above it
  // the chance to absorb the block into a guard or an arm.
  for (const il::BlockId blockId : dominators_.rpo()) {
    if (emitted_.contains(blockId) || !holdsAPattern(blockId)) {
      continue;
    }
    groups.emplace_back(blockId, emitRegion(blockId, il::BlockId{}, 0));
  }
  // Second sweep, one-sided ifs now allowed: every real diamond and dispatch
  // chain already had first pick of the CFG, so this can only claim blocks
  // nothing else wanted.
  oneSidedEnabled_ = true;
  for (const il::BlockId blockId : dominators_.rpo()) {
    if (emitted_.contains(blockId)) {
      continue;
    }
    groups.emplace_back(blockId, emitRegion(blockId, il::BlockId{}, 0));
  }

  std::map<il::BlockId, std::size_t> rank;
  for (const il::BlockId blockId : dominators_.rpo()) {
    rank.emplace(blockId, rank.size());
  }
  // A block the entry cannot reach has no place in that order, so it trails
  // everything that does — which is also where an unreachable remnant belongs.
  const auto rankOf = [&rank](il::BlockId block) {
    const auto found = rank.find(block);
    return found == rank.end() ? std::numeric_limits<std::size_t>::max()
                               : found->second;
  };
  std::stable_sort(groups.begin(), groups.end(),
                   [&rankOf](const auto& lhs, const auto& rhs) {
                     return rankOf(lhs.first) < rankOf(rhs.first);
                   });
  result.root = Stmt::make(StmtKind::Sequence);
  for (auto& [head, region] : groups) {
    for (StmtPtr& item : region->items) {
      result.root->items.push_back(std::move(item));
    }
  }

  // A goto-chain fallback (or a guard arm) always prints its target
  // explicitly because it is built before the region walk knows what ends
  // up next to it; now that `result.root`'s order is final, drop every one
  // that turned out to just restate the fallthrough.
  elideFallthroughGotos(result.root, il::BlockId{});

  std::set<il::BlockId> labeled;
  collectReferences(result.root.get(), labeled);
  result.labeled.assign(labeled.begin(), labeled.end());
  result.matchedPatterns = std::move(matchedPatterns_);
  return result;
}

// ---------------------------------------------------------------------------
// Region walking
// ---------------------------------------------------------------------------

StmtPtr Structurizer::emitRegion(il::BlockId cur, il::BlockId stop,
                                 unsigned depth) {
  auto seq = Stmt::make(StmtKind::Sequence);
  while (cur.valid() && cur != stop) {
    if (emitted_.contains(cur)) {
      // Where this region goes next is a block some other region already owns.
      // The edge is real and has to be said out loud: stopping the sequence here
      // silently makes control fall into whatever the final layout puts next,
      // which is another region's first block and not where this one goes. An
      // empty sequence is left alone — nothing was committed, so nothing is
      // owed, and the caller reads `regionEnd_` to decide what happened.
      if (!seq->items.empty()) {
        seq->items.push_back(gotoStmt(cur));
      }
      break;
    }
    if (depth < kMaxDepth && !inProgressHeaders_.contains(cur)) {
      if (const auto found = loopByHeader_.find(cur); found != loopByHeader_.end()) {
        if (StmtPtr loop = tryLoop(*found->second, depth)) {
          seq->items.push_back(std::move(loop));
          cur = regionEnd_;
          continue;
        }
      }
    }
    const il::Op* terminator = terminatorOf(cur);
    if (terminator == nullptr) {
      seq->items.push_back(emitBlock(cur));
      mark(cur);
      break;
    }
    switch (terminator->code) {
      case il::OpCode::Branch: {
        seq->items.push_back(emitBlock(cur));
        mark(cur);
        cur = function_.targets(*terminator)[0];
        break;
      }
      case il::OpCode::CondBranch: {
        const auto targets = function_.targets(*terminator);
        const auto operands = function_.operands(*terminator);
        if (depth < kMaxDepth && targets[0] != targets[1] &&
            !inProgressHeaders_.contains(cur)) {
          if (StmtPtr diamond = tryDiamond(cur, operands[0], targets[0], targets[1],
                                           depth)) {
            seq->items.push_back(emitBlock(cur));
            mark(cur);
            seq->items.push_back(std::move(diamond));
            matchedPatterns_.push_back(kCondBranchPatterns[0].name);
            cur = regionEnd_;
            continue;
          }
          if (StmtPtr cascade = tryGuardCascade(cur, depth)) {
            seq->items.push_back(emitBlock(cur));
            mark(cur);
            seq->items.push_back(std::move(cascade));
            matchedPatterns_.push_back(kCondBranchPatterns[1].name);
            cur = regionEnd_;
            continue;
          }
          if (tryDispatchTree(seq.get(), cur, depth)) {
            matchedPatterns_.push_back(kCondBranchPatterns[2].name);
            break;  // the switch consumed its tests; the region ends here
          }
          if (StmtPtr oneSided =
                  tryOneSided(cur, operands[0], targets[0], targets[1], depth)) {
            seq->items.push_back(emitBlock(cur));
            mark(cur);
            seq->items.push_back(std::move(oneSided));
            matchedPatterns_.push_back(kCondBranchPatterns[3].name);
            cur = regionEnd_;
            continue;
          }
        }
        if (!oneSidedEnabled_ && seq->items.empty()) {
          // First sweep, and nothing committed to `seq` yet: leave this site
          // unclaimed rather than settle for the raw goto-chain form. One-
          // sided ifs are weak enough to happily claim a block a real diamond
          // elsewhere still needs, so every diamond and dispatch chain in the
          // function gets to make its claims — here included, once the
          // second sweep revisits it — before anything falls back to plain
          // gotos. Once `seq` already holds something, though — a loop or
          // diamond this very call just closed, walking on into `cur` as its
          // committed continuation — deferring is not a free retry, it is
          // amnesia: nothing else remembers that this exact point in the
          // sequence owes a way to reach `cur`, and the second sweep, when it
          // eventually gets to `cur` on its own, prints it as an unconnected
          // island instead. Once something upstream is committed, `cur` gets
          // an honest goto now rather than a silently dropped edge later.
          break;
        }
        seq->items.push_back(emitBlock(cur));
        mark(cur);
        seq->items.push_back(gotoChain(operands[0], targets[0], targets[1]));
        matchedPatterns_.push_back("goto-chain");
        break;  // region ends: both arms belong to other regions
      }
      case il::OpCode::IndirectBranch: {
        seq->items.push_back(emitBlock(cur));
        mark(cur);
        // wrapAsLoop is a no-op unless `cur` is itself a loop header whose
        // cases branch back to it -- the same flattened-dispatcher shape
        // tryDispatchTree already wraps when the dispatch is a compare
        // chain instead of a resolved table.
        seq->items.push_back(wrapAsLoop(switchFor(cur, *terminator, depth), cur));
        break;
      }
      default:
        // Return/Unreachable and anything exotic: the block prints it.
        seq->items.push_back(emitBlock(cur));
        mark(cur);
        break;
    }
    if (terminator->code != il::OpCode::Branch) {
      break;
    }
  }
  regionEnd_ = cur;
  return seq;
}

// ---------------------------------------------------------------------------
// Diamonds
// ---------------------------------------------------------------------------

StmtPtr Structurizer::tryDiamond(il::BlockId head, il::ExprId cond,
                                 il::BlockId taken, il::BlockId untaken,
                                 unsigned depth) {
  if (budget_ == 0) {
    return nullptr;
  }
  const il::BlockId merge = postDominators_.ipdom(head);
  if (!merge.valid() || merge == head || emitted_.contains(merge)) {
    return nullptr;
  }
  const ScopedHeader inProgress(inProgressHeaders_, head);
  const std::size_t snapshot = trail_.size();
  const std::size_t gotoSnapshot = gotoTrail_.size();
  StmtPtr thenArm;
  StmtPtr elseArm;
  bool invert = false;
  if (taken == merge) {
    invert = true;
  } else {
    thenArm = emitRegion(taken, merge, depth + 1);
    if (regionEnd_ != merge) {
      budget_ -= std::min(budget_, trail_.size() - snapshot);
      rollback(snapshot, gotoSnapshot);
      return nullptr;
    }
  }
  if (untaken != merge) {
    elseArm = emitRegion(untaken, merge, depth + 1);
    if (regionEnd_ != merge) {
      budget_ -= std::min(budget_, trail_.size() - snapshot);
      rollback(snapshot, gotoSnapshot);
      return nullptr;
    }
  }
  if (emitted_.contains(merge) || !regionClosed(snapshot, head)) {
    budget_ -= std::min(budget_, trail_.size() - snapshot);
    rollback(snapshot, gotoSnapshot);
    return nullptr;
  }
  auto stmt = Stmt::make(StmtKind::If);
  stmt->cond = cond;
  stmt->invertCond = invert;
  if (invert) {
    stmt->thenArm = std::move(elseArm);
  } else {
    stmt->thenArm = std::move(thenArm);
    stmt->elseArm = std::move(elseArm);
  }
  regionEnd_ = merge;
  return stmt;
}

// ---------------------------------------------------------------------------
// Diamond's fallback: one-sided ifs
// ---------------------------------------------------------------------------

StmtPtr Structurizer::tryOneSided(il::BlockId head, il::ExprId cond,
                                  il::BlockId taken, il::BlockId untaken,
                                  unsigned depth) {
  if (budget_ == 0) {
    return nullptr;
  }
  for (const bool invert : {false, true}) {
    const il::BlockId body = invert ? untaken : taken;
    const il::BlockId rest = invert ? taken : untaken;
    if (body == head || body == rest) {
      continue;
    }
    // Cheap necessary condition, checked before the expensive walk: if
    // `head` doesn't dominate `body`, some other path already reaches it,
    // so the region can never come out closed. Skips most doomed attempts
    // before they spend any budget.
    if (!dominators_.dominates(head, body)) {
      continue;
    }
    const ScopedHeader inProgress(inProgressHeaders_, head);
    const std::size_t snapshot = trail_.size();
    const std::size_t gotoSnapshot = gotoTrail_.size();
    // No shared post-dominator required: walk `body` as its own region,
    // stopping early if it happens to flow straight into `rest` (a plain if
    // with implicit fallthrough), otherwise however it naturally ends —
    // return, unreachable, or a branch of its own that neither arm shares.
    StmtPtr arm = emitRegion(body, rest, depth + 1);
    // A stop other than `rest` is only safe when it is this walk's own
    // terminal block (the last thing it marked): anything else means the
    // walk ran into a block some other region already owns, and we cannot
    // tell where control actually goes without a real merge point.
    const bool deadEnds = trail_.size() > snapshot && trail_.back() == regionEnd_;
    const bool cleanStop = regionEnd_ == rest || deadEnds;
    // A guard whose arm leaves the function is exempt from the wait the first
    // sweep imposes on one-sided ifs. The wait exists because a one-sided if
    // needs no merge point and so can claim a block that a real diamond
    // elsewhere still needs; an arm that returns rather than reconverging is
    // not a block any diamond could have used, so holding it back gains
    // nothing and costs the shape it belongs to. Early-exit guards are also
    // where the cost of waiting is highest: leave one unclaimed and the sweep
    // that follows takes its return block as a standalone island, after which
    // no guard can ever form and the function reads back with its exits
    // stranded ahead of its entry.
    const bool leavesFunction = deadEnds && exits(regionEnd_);
    if (!cleanStop || (!oneSidedEnabled_ && !leavesFunction) ||
        !regionClosed(snapshot, head)) {
      budget_ -= std::min(budget_, trail_.size() - snapshot);
      rollback(snapshot, gotoSnapshot);
      if (budget_ == 0) {
        return nullptr;
      }
      continue;
    }
    auto stmt = Stmt::make(StmtKind::If);
    stmt->cond = cond;
    stmt->invertCond = invert;
    stmt->thenArm = std::move(arm);
    regionEnd_ = rest;
    return stmt;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Guard cascades: two guards sharing one fallback
// ---------------------------------------------------------------------------

StmtPtr Structurizer::tryGuardCascade(il::BlockId head, unsigned depth) {
  if (budget_ == 0) {
    return nullptr;
  }
  const std::optional<analysis::GuardCascadeShape> shape =
      analysis::matchGuardCascade(function_, postDominators_, head);
  if (!shape.has_value() || emitted_.contains(shape->merge) ||
      emitted_.contains(shape->fallback) || emitted_.contains(shape->innerHead) ||
      inProgressHeaders_.contains(shape->innerHead)) {
    return nullptr;
  }
  const ScopedHeader inProgress(inProgressHeaders_, head);
  const std::size_t snapshot = trail_.size();
  const std::size_t gotoSnapshot = gotoTrail_.size();

  // Claimed once; tryGuardCascade's own two failure arms each get an
  // independent copy of it below (see cloneStmt and claimSharedFallbackBody).
  StmtPtr fallbackOnce = claimSharedFallbackBody(shape->fallback, shape->merge, depth + 1);
  if (!fallbackOnce) {
    budget_ -= std::min(budget_, trail_.size() - snapshot);
    rollback(snapshot, gotoSnapshot);
    return nullptr;
  }

  // The inner guard's own ops always run once the outer guard lets control
  // through, so they lead the outer `if`'s taken arm, ahead of the inner
  // `if` itself -- the same shape emitRegion's own CondBranch case builds
  // for an ordinary nested diamond, just assembled directly here instead of
  // through a recursive emitRegion call (which would try to walk the shared
  // fallback a second time and fail exactly as tryDiamond already does).
  auto innerIf = Stmt::make(StmtKind::If);
  innerIf->cond = shape->innerCond;
  // thenArm stays null: the inner guard's success arm reaches `merge`
  // directly, so there is nothing of its own to print there -- printIf's own
  // handling of an empty arm takes care of that edge's phi copies. See
  // GuardCascadeShape::innerSuccessIsTaken for the polarity this inverts.
  innerIf->invertCond = !shape->innerSuccessIsTaken;
  innerIf->elseArm = cloneStmt(fallbackOnce.get());

  auto innerSeq = Stmt::make(StmtKind::Sequence);
  innerSeq->items.push_back(emitBlock(shape->innerHead));
  mark(shape->innerHead);
  innerSeq->items.push_back(std::move(innerIf));

  const il::Op* headTerm = terminatorOf(head);
  const auto headTargets = function_.targets(*headTerm);
  const bool outerTakenIsInner = headTargets[0] == shape->innerHead;

  auto outerIf = Stmt::make(StmtKind::If);
  outerIf->cond = shape->outerCond;
  outerIf->invertCond = !outerTakenIsInner;
  outerIf->thenArm = std::move(innerSeq);
  outerIf->elseArm = std::move(fallbackOnce);

  if (!regionClosed(snapshot, head)) {
    budget_ -= std::min(budget_, trail_.size() - snapshot);
    rollback(snapshot, gotoSnapshot);
    return nullptr;
  }
  regionEnd_ = shape->merge;
  return outerIf;
}

StmtPtr Structurizer::claimSharedFallbackBody(il::BlockId fallback, il::BlockId merge,
                                              unsigned depth) {
  if (depth >= kMaxDepth || budget_ == 0 || fallback == merge || emitted_.contains(fallback) ||
      inProgressHeaders_.contains(fallback)) {
    return nullptr;
  }
  const std::size_t snapshot = trail_.size();
  const std::size_t gotoSnapshot = gotoTrail_.size();
  // Stops AT merge rather than into it: merge belongs to whatever follows
  // the whole cascade, not to the fallback's own body.
  StmtPtr body = emitRegion(fallback, merge, depth);
  // `fallback`'s own predecessors were already proven to be exactly the two
  // guards' failure arms by matchGuardCascade, so `regionClosed` is asked
  // about `fallback` itself here, not either guard -- exactly the trust
  // claimDispatcherCaseBody places in its own caller's private-handler check
  // before asking the same question about `dispatcher`.
  if (trail_.size() == snapshot || regionEnd_ != merge || !regionClosed(snapshot, fallback)) {
    budget_ -= std::min(budget_, trail_.size() - snapshot);
    rollback(snapshot, gotoSnapshot);
    return nullptr;
  }
  return body;
}

// ---------------------------------------------------------------------------
// Loops
// ---------------------------------------------------------------------------

StmtPtr Structurizer::tryLoop(const analysis::NaturalLoop& loop, unsigned depth) {
  for (const il::BlockId member : loop.blocks) {
    if (emitted_.contains(member)) {
      return nullptr;  // partially claimed loops: raw emission stays honest
    }
  }
  const il::BlockId header = loop.header;
  const il::Op* headTerm = terminatorOf(header);
  if (headTerm == nullptr) {
    return nullptr;
  }
  inProgressHeaders_.insert(header);
  StmtPtr result;
  const std::size_t snapshot = trail_.size();
  const std::size_t gotoSnapshot = gotoTrail_.size();

  // while form: the header is only a conditional, one arm inside, one out.
  if (headTerm->code == il::OpCode::CondBranch && !hasBodyOps(header)) {
    const auto targets = function_.targets(*headTerm);
    const auto operands = function_.operands(*headTerm);
    for (const int arm : {0, 1}) {
      const il::BlockId bodyStart = targets[static_cast<std::size_t>(arm)];
      const il::BlockId exit = targets[static_cast<std::size_t>(arm ^ 1)];
      if (exit.valid() && loop.blocks.contains(bodyStart) &&
          !loop.blocks.contains(exit)) {
        StmtPtr body = emitRegion(bodyStart, header, depth + 1);
        if (regionEnd_ == header && regionClosed(snapshot, header)) {
          auto stmt = Stmt::make(StmtKind::While);
          stmt->block = header;
          stmt->cond = operands[0];
          stmt->invertCond = arm == 1;
          stmt->body = std::move(body);
          mark(header);
          regionEnd_ = exit;
          rewriteLoopExitToBreak(stmt->body, exit);
          result = std::move(stmt);
        } else {
          rollback(snapshot, gotoSnapshot);
        }
        break;
      }
    }
  }

  // Infinite form: the header is the whole loop and its only exit is an
  // unconditional jump back to itself -- no compiler emits this, but an
  // obfuscator's anti-tampering trap or dead-state sentinel does. Nothing
  // here tests a condition, so `while`/`do-while` above never fit; printed
  // plainly this is the label-and-goto pair a reader has to notice loops
  // forever, where `while (true)` says so directly.
  if (result == nullptr && loop.blocks.size() == 1 &&
      headTerm->code == il::OpCode::Branch &&
      function_.targets(*headTerm)[0] == header) {
    auto body = Stmt::make(StmtKind::Sequence);
    body->items.push_back(emitBlock(header));
    mark(header);
    auto stmt = Stmt::make(StmtKind::While);
    stmt->block = header;
    stmt->body = std::move(body);
    regionEnd_ = il::BlockId{};  // no exit edge: nothing follows this loop
    result = std::move(stmt);
  }

  // do-while form: one latch whose conditional returns to the header.
  if (result == nullptr) {
    for (const il::BlockId latch : loop.latches) {
      const il::Op* latchTerm = terminatorOf(latch);
      if (latchTerm == nullptr || latchTerm->code != il::OpCode::CondBranch) {
        continue;
      }
      const auto targets = function_.targets(*latchTerm);
      const auto operands = function_.operands(*latchTerm);
      const int backArm = targets[0] == header ? 0 : targets[1] == header ? 1 : -1;
      if (backArm < 0) {
        continue;
      }
      const il::BlockId exit = targets[static_cast<std::size_t>(backArm ^ 1)];
      if (loop.blocks.contains(exit)) {
        continue;
      }
      StmtPtr body = emitRegion(header, latch, depth + 1);
      if (regionEnd_ == latch && !emitted_.contains(latch) &&
          regionClosed(snapshot, header)) {
        body->items.push_back(emitBlock(latch));
        mark(latch);
        auto stmt = Stmt::make(StmtKind::DoWhile);
        stmt->block = header;
        stmt->cond = operands[0];
        stmt->invertCond = backArm == 1;
        stmt->body = std::move(body);
        regionEnd_ = exit;
        rewriteLoopExitToBreak(stmt->body, exit);
        result = std::move(stmt);
      } else {
        rollback(snapshot, gotoSnapshot);
      }
      break;
    }
  }

  // Guarded do-while: the header itself may leave the loop before the body
  // ever runs — retry-style code (a spinlock's "is this free yet" test is
  // the textbook case) commonly tests up front and again at the latch,
  // both exits landing on the very same block. That header exit is why
  // neither form above fits: the while-form needs a header with nothing
  // but the test (this one has real ops first), and the do-while form
  // walks `emitRegion(header, latch)`, whose very first block is `header`
  // — already marked in-progress for this call, so its own branch can
  // only fall back to a bare, unlabelled goto pair instead of a nested
  // if. Handling the header's exit explicitly here, as its own `if (exit)
  // goto sharedExit;` ahead of an ordinary do-while body, sidesteps that
  // rather than teaching the general walk to tolerate re-entering its own
  // in-progress header.
  if (result == nullptr && headTerm->code == il::OpCode::CondBranch) {
    const auto headTargets = function_.targets(*headTerm);
    const auto headOperands = function_.operands(*headTerm);
    for (const int harm : {0, 1}) {
      if (result != nullptr) {
        break;
      }
      const il::BlockId continueTarget = headTargets[static_cast<std::size_t>(harm)];
      const il::BlockId headerExit = headTargets[static_cast<std::size_t>(harm ^ 1)];
      if (!headerExit.valid() || continueTarget == header ||
          !loop.blocks.contains(continueTarget) || loop.blocks.contains(headerExit)) {
        continue;
      }
      for (const il::BlockId latch : loop.latches) {
        if (latch == header) {
          continue;  // no separate body block: the bare do-while form above owns this
        }
        const il::Op* latchTerm = terminatorOf(latch);
        if (latchTerm == nullptr || latchTerm->code != il::OpCode::CondBranch) {
          continue;
        }
        const auto latchTargets = function_.targets(*latchTerm);
        const auto latchOperands = function_.operands(*latchTerm);
        const int backArm = latchTargets[0] == header ? 0 : latchTargets[1] == header ? 1 : -1;
        if (backArm < 0) {
          continue;
        }
        const il::BlockId latchExit = latchTargets[static_cast<std::size_t>(backArm ^ 1)];
        // Both exits have to be the SAME block: this only speaks the C a
        // single shared landing point gives it a plain `if (...) goto
        // there;` for. Exits to two different blocks would need a loop
        // construct that can break to either one, which nothing here
        // prints (yet).
        if (latchExit != headerExit || loop.blocks.contains(latchExit)) {
          continue;
        }
        StmtPtr body = continueTarget == latch
                          ? Stmt::make(StmtKind::Sequence)
                          : emitRegion(continueTarget, latch, depth + 1);
        if ((continueTarget != latch &&
             (regionEnd_ != latch || !regionClosed(snapshot, header))) ||
            emitted_.contains(latch)) {
          rollback(snapshot, gotoSnapshot);
          continue;
        }
        auto guard = Stmt::make(StmtKind::If);
        guard->cond = headOperands[0];
        guard->invertCond = harm == 0;
        guard->thenArm = gotoStmt(headerExit);
        auto fullBody = Stmt::make(StmtKind::Sequence);
        // The header's own ops (and the block printIf reads `last_` from
        // for the guard's edge) never got emitted above: the walk that
        // would ordinarily do that is exactly what this form avoids
        // re-entering the header through.
        fullBody->items.push_back(emitBlock(header));
        mark(header);
        fullBody->items.push_back(std::move(guard));
        for (StmtPtr& item : body->items) {
          fullBody->items.push_back(std::move(item));
        }
        fullBody->items.push_back(emitBlock(latch));
        mark(latch);
        auto stmt = Stmt::make(StmtKind::DoWhile);
        stmt->block = header;
        stmt->cond = latchOperands[0];
        stmt->invertCond = backArm == 1;
        stmt->body = std::move(fullBody);
        regionEnd_ = headerExit;
        rewriteLoopExitToBreak(stmt->body, headerExit);
        result = std::move(stmt);
        break;
      }
    }
  }

  if (result == nullptr) {
    result = tryDispatcherLoop(loop, depth);
  }

  inProgressHeaders_.erase(header);
  return result;
}

StmtPtr Structurizer::tryDispatcherLoop(const analysis::NaturalLoop& loop, unsigned depth) {
  const il::BlockId header = loop.header;
  const il::Op* headTerm = terminatorOf(header);
  if (headTerm == nullptr || headTerm->code != il::OpCode::CondBranch) {
    return nullptr;
  }
  const auto headTargets = function_.targets(*headTerm);
  const auto headOperands = function_.operands(*headTerm);
  for (const int harm : {0, 1}) {
    const il::BlockId dispatch = headTargets[static_cast<std::size_t>(harm)];
    const il::BlockId headerExit = headTargets[static_cast<std::size_t>(harm ^ 1)];
    if (!headerExit.valid() || dispatch == header || !loop.blocks.contains(dispatch) ||
        emitted_.contains(dispatch)) {
      continue;
    }
    const il::Op* dispatchTerm = terminatorOf(dispatch);
    if (dispatchTerm == nullptr || dispatchTerm->code != il::OpCode::IndirectBranch) {
      continue;
    }
    const auto dispatchTargets = function_.targets(*dispatchTerm);
    const std::optional<analysis::DispatcherShape> shape =
        analysis::matchDispatcherShape(function_, dispatch, dispatchTargets);
    if (!shape.has_value() || shape->hub != header ||
        std::find(loop.latches.begin(), loop.latches.end(), shape->merge) ==
            loop.latches.end()) {
      continue;
    }
    const std::size_t snapshot = trail_.size();
    const std::size_t gotoSnapshot = gotoTrail_.size();
    auto body = Stmt::make(StmtKind::Sequence);
    body->items.push_back(emitBlock(header));
    mark(header);
    auto guard = Stmt::make(StmtKind::If);
    guard->cond = headOperands[0];
    guard->invertCond = harm == 0;

    // Ordinarily headerExit truly leaves the loop, so the guard is a plain
    // `if (cond) goto headerExit;` and dispatch/the switch just follow it,
    // unconditionally, in the same sequence. bc_lib's own dispatcher instead
    // has the "state out of range" arm do a small bit of its own work and
    // then land on the very same `shape->merge` every handler does (see
    // analysis::DispatcherShape) -- exactly the private, merge-bound handler
    // shape `claimDispatcherCaseBody` already inlines for a switch case,
    // just reached from `header` rather than `dispatch`. That is also *why*
    // headerExit turns up inside `loop.blocks` at all here: the natural-loop
    // walk reaches it backward from the latch, which a genuine exit never
    // would. Since both arms now reach the same tail, dispatch/the switch has
    // to move into a real `else` (an out-of-range state must not also run the
    // switch on whatever garbage index it left behind), and the tail moves
    // out from under the switch to run once after the whole `if`/`else`
    // instead of once per arm.
    const bool threeWayMerge = loop.blocks.contains(headerExit);
    StmtPtr headerExitBody;
    if (threeWayMerge) {
      headerExitBody =
          claimDispatcherCaseBody(header, headerExit, shape->merge, depth + 1, /*appendBreak=*/false);
      if (!headerExitBody) {
        rollback(snapshot, gotoSnapshot);
        continue;
      }
    } else {
      guard->thenArm = gotoStmt(headerExit);
    }

    auto emitDispatchAndSwitch = [&]() -> StmtPtr {
      auto seq = Stmt::make(StmtKind::Sequence);
      seq->items.push_back(emitBlock(dispatch));
      mark(dispatch);
      StmtPtr switchStmt = switchFor(dispatch, *dispatchTerm, depth + 1);
      // The loop-back is implicit in `while (true)`'s own re-entry: strip the
      // trailing `goto header` switchFor's epilogue printed on the (correct,
      // context-free) assumption that nothing was going to wrap it in a loop.
      if (switchStmt->epilogue && !switchStmt->epilogue->items.empty() &&
          switchStmt->epilogue->items.back()->kind == StmtKind::Goto &&
          switchStmt->epilogue->items.back()->block == header) {
        switchStmt->epilogue->items.pop_back();
      }
      seq->items.push_back(std::move(switchStmt));
      return seq;
    };

    StmtPtr sharedEpilogue;
    if (threeWayMerge) {
      StmtPtr dispatchAndSwitch = emitDispatchAndSwitch();
      sharedEpilogue = std::move(dispatchAndSwitch->items.back()->epilogue);
      guard->thenArm = std::move(headerExitBody);
      guard->elseArm = std::move(dispatchAndSwitch);
      if (!regionClosed(snapshot, header)) {
        rollback(snapshot, gotoSnapshot);
        continue;
      }
      body->items.push_back(std::move(guard));
      if (sharedEpilogue) {
        body->items.push_back(std::move(sharedEpilogue));
      }
    } else {
      body->items.push_back(std::move(guard));
      StmtPtr dispatchAndSwitch = emitDispatchAndSwitch();
      if (!regionClosed(snapshot, header)) {
        rollback(snapshot, gotoSnapshot);
        continue;
      }
      for (StmtPtr& item : dispatchAndSwitch->items) {
        body->items.push_back(std::move(item));
      }
    }
    auto stmt = Stmt::make(StmtKind::While);
    stmt->block = header;
    stmt->body = std::move(body);
    // A claimed headerExit rejoins merge, and merge's only way out is back
    // through the header -- there is no path left out of this loop to name.
    regionEnd_ = threeWayMerge ? il::BlockId{} : headerExit;
    return stmt;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Raw emission
// ---------------------------------------------------------------------------

StmtPtr Structurizer::gotoChain(il::ExprId cond, il::BlockId taken,
                                il::BlockId untaken) {
  auto stmt = Stmt::make(StmtKind::If);
  stmt->cond = cond;
  stmt->thenArm = gotoStmt(taken);
  if (taken != untaken) {
    stmt->elseArm = gotoStmt(untaken);
  }
  return stmt;
}

StmtPtr Structurizer::gotoStmt(il::BlockId target) {
  addGotoTarget(target);
  auto stmt = Stmt::make(StmtKind::Goto);
  stmt->block = target;
  return stmt;
}

StmtPtr Structurizer::switchFor(il::BlockId block, const il::Op& op, unsigned depth) {
  auto stmt = Stmt::make(StmtKind::Switch);
  const auto operands = function_.operands(op);
  const auto targets = function_.targets(op);
  stmt->block = block;
  stmt->cases.assign(targets.begin(), targets.end());
  // Every case leaves from the branch's own block, which is where the phi
  // copies on those edges have to come from.
  stmt->casePreds.assign(targets.size(), block);
  stmt->caseBodies.resize(targets.size());

  // A flattening dispatcher's cases usually share one tail (see
  // analysis::DispatcherShape): most of them do their own work and then land
  // on the very same block before jumping back to the loop header.
  // Structuring that block once here, as the switch's own epilogue, is what
  // lets every handler that reaches it be written inline (see
  // claimDispatcherCaseBody) instead of `goto`-ing to a label a hundred cases
  // repeat. Skipped when `merge` is already spoken for -- some other region
  // got there first, and claiming it twice would print it twice.
  const std::optional<analysis::DispatcherShape> shape =
      analysis::matchDispatcherShape(function_, block, targets);
  StmtPtr epilogue;
  if (shape.has_value() && !emitted_.contains(shape->merge) &&
      !inProgressHeaders_.contains(shape->merge)) {
    epilogue = emitRegion(shape->merge, il::BlockId{}, depth + 1);
  }
  for (std::size_t index = 0; index < targets.size(); ++index) {
    if (epilogue) {
      stmt->caseBodies[index] =
          claimDispatcherCaseBody(block, targets[index], shape->merge, depth);
    }
    if (!stmt->caseBodies[index]) {
      stmt->caseBodies[index] = claimCaseBody(block, targets[index], depth);
    }
    if (!stmt->caseBodies[index]) {
      addGotoTarget(targets[index]);
    }
  }
  if (epilogue) {
    stmt->epilogue = std::move(epilogue);
    stmt->mergeBlock = shape->merge;
    // Whether the handlers this switch just claimed carry their live
    // registers to `epilogue` through the shadow-register protocol (see
    // analysis::LiveRegisterFrame): when they do, emission can skip printing
    // a case's save into a slot it never actually changes.
    stmt->frame = analysis::matchLiveRegisterFrame(function_, *shape);
  }
  // A table match at two targets is still a real index dispatch -- an
  // opaque-predicate pass upstream routinely leaves a flattened function's
  // three- and four-way sites down to two live values once the others are
  // proven unreachable, and a two-way `switch (state)` says exactly what
  // happened where an address compare chain (`if (t == 0x...) ... else if
  // (t == 0x...)`) makes the reader rediscover it from raw target addresses.
  // Below two there is nothing a switch adds over a plain `if`.
  if (targets.size() >= 2) {
    if (const auto table = analysis::matchJumpTable(function_, operands[0]);
        table.has_value() && table->index.valid()) {
      stmt->tableMode = true;
      stmt->cond = table->index;
      return stmt;
    }
  }
  stmt->cond = operands[0];
  return stmt;
}

StmtPtr Structurizer::claimCaseBody(il::BlockId dispatcher, il::BlockId handler,
                                    unsigned depth) {
  if (depth >= kMaxDepth || budget_ == 0 || handler == dispatcher ||
      emitted_.contains(handler) || inProgressHeaders_.contains(handler)) {
    return nullptr;
  }
  // Reached from this switch and from nowhere else. A handler with other ways in
  // needs a label for them, and writing its code inside one case would leave the
  // others jumping into the middle of a switch — which is what the label form
  // says plainly, so it stays.
  const auto& predecessors = function_.block(handler).predecessors;
  if (predecessors.size() != 1 || predecessors.front() != dispatcher) {
    return nullptr;
  }
  const ScopedHeader inProgress(inProgressHeaders_, dispatcher);
  const std::size_t snapshot = trail_.size();
  const std::size_t gotoSnapshot = gotoTrail_.size();
  StmtPtr body = emitRegion(handler, il::BlockId{}, depth + 1);
  // Control must not be able to run off the end of the case: the next thing after
  // it in the text is the next case, which is not where the handler goes.
  if (trail_.size() == snapshot || !regionClosed(snapshot, dispatcher) ||
      !alwaysLeaves(body.get())) {
    budget_ -= std::min(budget_, trail_.size() - snapshot);
    rollback(snapshot, gotoSnapshot);
    return nullptr;
  }
  return body;
}

StmtPtr Structurizer::claimDispatcherCaseBody(il::BlockId dispatcher, il::BlockId handler,
                                              il::BlockId merge, unsigned depth,
                                              bool appendBreak) {
  if (depth >= kMaxDepth || budget_ == 0 || handler == dispatcher || handler == merge ||
      emitted_.contains(handler) || inProgressHeaders_.contains(handler)) {
    return nullptr;
  }
  // Same private-handler requirement as claimCaseBody: reached from this
  // switch and from nowhere else.
  const auto& predecessors = function_.block(handler).predecessors;
  if (predecessors.size() != 1 || predecessors.front() != dispatcher) {
    return nullptr;
  }
  const ScopedHeader inProgress(inProgressHeaders_, dispatcher);
  const std::size_t snapshot = trail_.size();
  const std::size_t gotoSnapshot = gotoTrail_.size();
  // Stops AT merge rather than walking into it: merge belongs to the switch
  // as a whole (see switchFor's epilogue), not to this one case.
  StmtPtr body = emitRegion(handler, merge, depth + 1);
  if (trail_.size() == snapshot || regionEnd_ != merge || !regionClosed(snapshot, dispatcher)) {
    budget_ -= std::min(budget_, trail_.size() - snapshot);
    rollback(snapshot, gotoSnapshot);
    return nullptr;
  }
  // The handler's own code never says explicitly that it leaves for the
  // tail -- that arrival is what `regionEnd_ == merge` above already proved;
  // `Break` is what says the same thing in C, since the tail is about to be
  // printed as the switch's epilogue rather than inline in this case.
  if (appendBreak) {
    body->items.push_back(Stmt::make(StmtKind::Break));
  }
  return body;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bool Structurizer::alwaysLeaves(const Stmt* node) const {
  if (node == nullptr) {
    return false;
  }
  switch (node->kind) {
    case StmtKind::Goto:
    case StmtKind::Break:
      return true;
    case StmtKind::Block:
      return exits(node->block);
    case StmtKind::Sequence:
      return !node->items.empty() && alwaysLeaves(node->items.back().get());
    case StmtKind::If:
      return alwaysLeaves(node->thenArm.get()) && alwaysLeaves(node->elseArm.get());
    case StmtKind::Switch:
      // A dispatcher shape's epilogue is what every `Break` case actually
      // falls into, so whether the whole switch+epilogue unit leaves is the
      // epilogue's question to answer, not the switch's.
      if (node->epilogue) {
        return alwaysLeaves(node->epilogue.get());
      }
      // Every case either was claimed (claimCaseBody already required its body
      // to leave before accepting it) or prints as a `goto` to its handler,
      // which also leaves -- so the only way control falls past a Switch is an
      // unresolved compare chain, which has no default arm to catch a target
      // matching none of its tests (see printSwitch's `enumerated` check).
      return node->tableMode || !node->caseValues.empty();
    default:
      return false;
  }
}

StmtPtr Structurizer::emitBlock(il::BlockId blockId) {
  auto stmt = Stmt::make(StmtKind::Block);
  stmt->block = blockId;
  return stmt;
}

const il::Op* Structurizer::terminatorOf(il::BlockId blockId) const {
  const auto& ops = function_.block(blockId).ops;
  if (ops.empty()) {
    return nullptr;
  }
  const il::Op& last = function_.op(ops.back());
  switch (last.code) {
    case il::OpCode::Branch:
    case il::OpCode::CondBranch:
    case il::OpCode::IndirectBranch:
    case il::OpCode::Return:
    case il::OpCode::Unreachable:
      return &last;
    default:
      return nullptr;
  }
}

bool Structurizer::hasBodyOps(il::BlockId blockId) const {
  const auto& ops = function_.block(blockId).ops;
  return ops.size() > 1 || (ops.size() == 1 && terminatorOf(blockId) == nullptr);
}

bool Structurizer::exits(il::BlockId blockId) const {
  const il::Op* terminator = terminatorOf(blockId);
  return terminator != nullptr && (terminator->code == il::OpCode::Return ||
                                   terminator->code == il::OpCode::Unreachable);
}

bool Structurizer::holdsAPattern(il::BlockId blockId) const {
  if (loopByHeader_.contains(blockId)) {
    return true;
  }
  const il::Op* terminator = terminatorOf(blockId);
  return terminator != nullptr && (terminator->code == il::OpCode::CondBranch ||
                                   terminator->code == il::OpCode::IndirectBranch);
}

bool Structurizer::hasPhis(il::BlockId blockId) const {
  const auto& ops = function_.block(blockId).ops;
  return !ops.empty() && function_.op(ops.front()).code == il::OpCode::Phi;
}

void Structurizer::mark(il::BlockId blockId) {
  emitted_.insert(blockId);
  trail_.push_back(blockId);
}

void Structurizer::addGotoTarget(il::BlockId block) {
  if (gotoTargets_.insert(block).second) {
    gotoTrail_.push_back(block);
  }
}

void Structurizer::rollback(std::size_t trailSnapshot, std::size_t gotoSnapshot) {
  for (std::size_t index = trailSnapshot; index < trail_.size(); ++index) {
    emitted_.erase(trail_[index]);
  }
  trail_.resize(trailSnapshot);
  for (std::size_t index = gotoSnapshot; index < gotoTrail_.size(); ++index) {
    gotoTargets_.erase(gotoTrail_[index]);
  }
  gotoTrail_.resize(gotoSnapshot);
}

bool Structurizer::regionClosed(std::size_t snapshot, il::BlockId head) const {
  for (std::size_t index = snapshot; index < trail_.size(); ++index) {
    const il::BlockId member = trail_[index];
    if (member == head) {
      continue;
    }
    for (const il::BlockId pred : function_.block(member).predecessors) {
      bool inside = pred == head;
      for (std::size_t other = snapshot; other < trail_.size() && !inside; ++other) {
        inside = trail_[other] == pred;
      }
      if (!inside) {
        return false;
      }
    }
  }
  return true;
}

bool StructuredFunction::isLabeled(il::BlockId block) const {
  return std::binary_search(labeled.begin(), labeled.end(), block);
}

StructuredFunction structureFunction(
    const il::Function& function, const analysis::Dominators& dominators,
    const analysis::PostDominators& postDominators,
    std::span<const analysis::NaturalLoop> loops) {
  return Structurizer(function, dominators, postDominators, loops).run();
}

}  // namespace xdec::emit

