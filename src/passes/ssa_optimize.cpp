// makeSsaOptimizePass: SCCP, phi simplification, and global DCE (see the
// header for what lives here and why it stays at Ssa maturity).
#include "xdec/passes/ssa_optimize.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "xdec/analysis/call_abi.h"
#include "xdec/il/ceval.h"
#include "xdec/il/function.h"
#include "xdec/obs/log.h"

#include "algebra.h"
#include "call_names.h"
#include "transform.h"

namespace xdec::passes {

// Progress inside the pass, for when the pass itself is the thing taking too
// long. Set XDEC_LOG=optimize=debug.
XDEC_DEFINE_LOG_CATEGORY(optimizeLog, "optimize")

namespace {

/// Milliseconds since the last lap, for the sub-phase timings.
class Clock {
 public:
  [[nodiscard]] int64_t lap() {
    const auto now = std::chrono::steady_clock::now();
    const auto since = now - at_;
    at_ = now;
    return std::chrono::duration_cast<std::chrono::milliseconds>(since).count();
  }

 private:
  std::chrono::steady_clock::time_point at_ = std::chrono::steady_clock::now();
};

/// One SCCP lattice cell: Unknown below Const below Overdefined. Movement is
/// monotone, which is what makes the iteration terminate.
struct Cell {
  enum class Kind : uint8_t { Unknown, Const, Overdefined };
  Kind kind = Kind::Unknown;
  uint64_t value = 0;  // valid when kind == Const, masked to the type's width

  [[nodiscard]] static Cell constant(uint64_t value) { return {Kind::Const, value}; }
  [[nodiscard]] static Cell overdefined() { return {Kind::Overdefined, 0}; }
};

/// Wegman-Zadeck sparse conditional constant propagation over the SSA value
/// graph, specialised to the tree IR: SSA values carry cells, and whole
/// expression trees evaluate through the shared constant evaluator once their
/// leaves are known. Both worklists are explicit — a flattened function runs
/// to thousands of blocks, and a recursive CFG walk is a stack overflow
/// looking for permission to happen.
class Sccp {
 public:
  explicit Sccp(il::Function& function)
      : function_(function),
        cells_(function.valueCount()),
        evaluated_(function.exprCount()),
        overdefined_(function.exprCount(), 0),
        uses_(function.valueCount()),
        opBlocks_(function.opCount()),
        leafSeenGen_(function.exprCount(), 0),
        execBlocks_(function.blockCount(), 0) {}

  /// Runs to convergence, then rewrites: constant values become Const
  /// expressions everywhere they are read, and constant-conditioned branches
  /// become unconditional. Returns whether anything changed.
  bool run() {
    indexUses();
    markExecutable(function_.entryBlock());
    drain();
    const bool rewritten = rewriteConstants();
    return rewritten || foldBranches();
  }

 private:
  // -- iteration ------------------------------------------------------------

  void ensureExprCapacity(uint32_t idx) {
    if (idx >= overdefined_.size()) {
      const std::size_t next = std::max<std::size_t>(function_.exprCount(), idx + 1);
      overdefined_.resize(next, 0);
      evaluated_.resize(next);
    }
  }

  void indexUses() {
    for (const il::BlockId blockId : function_.blockHandles()) {
      for (const il::OpId opId : function_.block(blockId).ops) {
        opBlocks_[opId.index()] = blockId;
        for (const il::ExprId operand : function_.operands(function_.op(opId))) {
          collectValueLeaves(operand, opId);
        }
      }
    }
  }

  void collectValueLeaves(il::ExprId id, il::OpId user) {
    // Iterative, like the verifier's walker: substituted chains are deeper
    // than any call stack this pass may rely on.
    if (++leafGen_ == 0) {
      std::fill(leafSeenGen_.begin(), leafSeenGen_.end(), 0);
      leafGen_ = 1;
    }
    const uint32_t gen = leafGen_;
    leafWork_.clear();
    leafWork_.push_back(id);
    while (!leafWork_.empty()) {
      const il::ExprId at = leafWork_.back();
      leafWork_.pop_back();
      const uint32_t idx = at.index();
      if (idx >= leafSeenGen_.size()) {
        leafSeenGen_.resize(std::max<std::size_t>(function_.exprCount(), idx + 1), 0);
      }
      if (leafSeenGen_[idx] == gen) {
        continue;
      }
      leafSeenGen_[idx] = gen;
      const il::Expr& expr = function_.expr(at);
      if (expr.op == il::ExprOp::Value) {
        const il::ValueId val{static_cast<uint32_t>(expr.immediate)};
        if (val.valid() && val.index() < uses_.size()) {
          uses_[val.index()].push_back(user);
        }
        continue;
      }
      for (unsigned index = 0; index < expr.operandCount; ++index) {
        leafWork_.push_back(expr.operands[index]);
      }
    }
  }

  /// The cell of a whole expression tree. Unknown means a Value leaf has not
  /// been computed yet; the op owning this expression is on that value's use
  /// list, so the evaluation is retried when the leaf moves.
  ///
  /// Memoised for the duration of one top-level evaluation, which is what makes
  /// this affordable rather than merely correct. Expressions here are a DAG, not
  /// a tree: simplification substitutes one value into every use, so a
  /// subexpression is shared by as many parents as read it, and walking the
  /// parents independently visits it once per path through the graph — which for
  /// the MBA chains in an obfuscated function means exponentially many times, and
  /// each visit interns fresh constant nodes on the way. That is what turned this
  /// pass from four seconds into minutes when the function doubled in size.
  /// Within one evaluation no cell moves, so one answer per node is the same
  /// answer as many.
  [[nodiscard]] Cell evaluate(il::ExprId id, unsigned depth = 0) {
    // Overdefined is the top of the lattice and cells only ever move up, so an
    // expression that has reached it can never leave: every reason to return it
    // is either permanent (a type with no Const form, the depth cutoff, an
    // evaluator that declined) or itself an Overdefined child. Remembering those
    // across the whole iteration is what makes an obfuscated function tractable,
    // because in one nearly everything is Overdefined -- the work being saved is
    // proving the same unknowable subexpression unknowable once per visit.
    const uint32_t idx = id.index();
    ensureExprCapacity(idx);
    if (overdefined_[idx] != 0) {
      return Cell::overdefined();
    }
    if (depth == 0) {
      // Stamped rather than cleared. An evaluation's answers are only good for
      // that evaluation, but a map holds onto its buckets when emptied, so
      // clearing one that a deep walk grew to hundreds of thousands of entries
      // costs that many bucket writes on every root -- and a drain does tens of
      // thousands of roots, which is how a pass with only 266k node evaluations
      // in it spent sixteen seconds not doing them.
      ++generation_;
      truncated_ = false;
    } else if (evaluated_[idx].first == generation_) {
      return evaluated_[idx].second;
    }
    const bool outerTruncated = truncated_;
    truncated_ = false;
    const Cell cell = evaluateNode(id, depth);
    const bool nodeTruncated = truncated_;
    truncated_ = outerTruncated || nodeTruncated;
    ensureExprCapacity(idx);
    if (cell.kind == Cell::Kind::Overdefined) {
      // Unless the depth cutoff was involved, in which case the verdict is about
      // where this walk started rather than about the node, and a walk that
      // reaches it from closer by may do better.
      if (!nodeTruncated) {
        overdefined_[idx] = 1;
      }
      return cell;
    }
    evaluated_[idx] = std::pair{generation_, cell};
    return cell;
  }

  [[nodiscard]] Cell evaluateNode(il::ExprId id, unsigned depth) {
    if (depth > kMaxDepth) {
      truncated_ = true;
      return Cell::overdefined();
    }
    const il::Expr expr = function_.expr(id);  // by value: interning dangles
    switch (expr.op) {
      case il::ExprOp::Const:
        return Cell::constant(expr.immediate);
      case il::ExprOp::Undef:
        // An unknown machine value, not an unknown analysis result: it must
        // poison meets like any other unanalysable input.
        return Cell::overdefined();
      case il::ExprOp::Value:
        return cells_[static_cast<uint32_t>(expr.immediate)];
      default:
        break;
    }
    if (expr.operandCount == 0) {
      return Cell::overdefined();
    }
    il::Expr rebuilt = expr;
    bool allConst = true;
    for (unsigned index = 0; index < expr.operandCount; ++index) {
      const Cell child = evaluate(expr.operands[index], depth + 1);
      if (child.kind == Cell::Kind::Overdefined) {
        return Cell::overdefined();
      }
      if (child.kind == Cell::Kind::Unknown) {
        allConst = false;
        continue;
      }
      // A flags bundle can evaluate to a constant NZCV, but there is no Const
      // expression of that type to materialise: declined, not wrong.
      const il::Type childType = function_.expr(expr.operands[index]).type;
      if (!childType.isScalarInteger() && !childType.isFloat()) {
        return Cell::overdefined();
      }
      rebuilt.operands[index] = function_.constant(childType, child.value);
    }
    if (!allConst) {
      return Cell{Cell::Kind::Unknown, 0};
    }
    // Same limit at the node itself: no Const form, no constant cell.
    if (!expr.type.isScalarInteger() && !expr.type.isFloat()) {
      return Cell::overdefined();
    }
    // Every leaf is a Const: hand the node to the shared evaluator, so a fold
    // here can never disagree with the interpreter the oracle checks.
    il::ConcreteValue out;
    if (il::tryEvalConst(function_, function_.intern(rebuilt), out)) {
      return Cell::constant(out.lo);
    }
    return Cell::overdefined();  // wider than 64 bits, say: declined, not wrong
  }

  [[nodiscard]] static Cell meet(Cell lhs, Cell rhs) {
    if (lhs.kind == Cell::Kind::Unknown) {
      return rhs;
    }
    if (rhs.kind == Cell::Kind::Unknown) {
      return lhs;
    }
    if (lhs.kind == Cell::Kind::Overdefined || rhs.kind == Cell::Kind::Overdefined) {
      return Cell::overdefined();
    }
    if (lhs.value == rhs.value) {
      return lhs;
    }
    return Cell::overdefined();
  }

  void updateCell(il::ValueId value, Cell next) {
    if (!value.valid() || value.index() >= cells_.size()) {
      return;
    }
    Cell& cell = cells_[value.index()];
    if (next.kind == cell.kind && (next.kind != Cell::Kind::Const || next.value == cell.value)) {
      return;
    }
    cell = next;
    for (const il::OpId user : uses_[value.index()]) {
      ssaWork_.push_back(user);
    }
  }

  [[nodiscard]] static uint64_t edgeKey(il::BlockId from, il::BlockId to) {
    return (static_cast<uint64_t>(from.index()) << 32) | to.index();
  }

  void pushEdge(il::BlockId from, il::BlockId to) {
    if (execEdges_.insert(edgeKey(from, to)).second) {
      cfgWork_.emplace_back(from, to);
    }
  }

  void markExecutable(il::BlockId blockId) {
    if (blockId.valid() && blockId.index() < execBlocks_.size() &&
        execBlocks_[blockId.index()] == 0) {
      execBlocks_[blockId.index()] = 1;
      visitBlock(blockId);
    }
  }

  void drain() {
    while (!cfgWork_.empty() || !ssaWork_.empty()) {
      if (!cfgWork_.empty()) {
        const auto [from, to] = cfgWork_.back();
        cfgWork_.pop_back();
        if (to.valid() && to.index() < execBlocks_.size() &&
            execBlocks_[to.index()] == 0) {
          execBlocks_[to.index()] = 1;
          visitBlock(to);
        } else {
          // Already executable: only the phis see the newly executable edge.
          visitPhisOnEdge(from, to);
        }
        continue;
      }
      const il::OpId opId = ssaWork_.back();
      ssaWork_.pop_back();
      // A use in a block the analysis has not reached yet is noise: when the
      // block becomes executable its whole body is visited fresh.
      const il::BlockId blockId = opBlocks_[opId.index()];
      if (blockId.valid() && execBlocks_[blockId.index()] != 0) {
        visitOp(blockId, opId);
      }
    }
  }

  /// Meets one newly executable edge into the phis of `to`, which is the whole
  /// of what that edge tells them.
  ///
  /// A phi's cell is the meet over its executable incoming edges, so an edge
  /// becoming executable adds a term rather than changing the others: meeting it
  /// into the cell already there gives the same answer as recomputing the merge,
  /// and recomputing is what makes a flattened dispatcher unaffordable. Its head
  /// block has one predecessor per state -- 1351 of them here -- so a full merge
  /// per arriving edge is quadratic in predecessors and, since every state block
  /// branches back, all of those edges do arrive.
  void visitPhisOnEdge(il::BlockId from, il::BlockId to) {
    const auto& predecessors = function_.block(to).predecessors;
    // Once per edge rather than once per phi. Repeated entries are separate slots
    // for the one edge, so they all become executable together.
    slots_.clear();
    for (std::size_t index = 0; index < predecessors.size(); ++index) {
      if (predecessors[index] == from) {
        slots_.push_back(index);
      }
    }
    if (slots_.empty()) {
      return;
    }
    for (const il::OpId opId : function_.block(to).ops) {
      const il::Op& op = function_.op(opId);
      if (op.code != il::OpCode::Phi) {
        break;
      }
      const auto operands = function_.operands(op);
      Cell merged = cells_[op.result.index()];
      for (const std::size_t slot : slots_) {
        if (slot < operands.size()) {
          merged = meet(merged, evaluate(operands[slot]));
        }
      }
      updateCell(op.result, merged);
    }
  }

  void visitBlock(il::BlockId blockId) {
    for (const il::OpId opId : function_.block(blockId).ops) {
      visitOp(blockId, opId);
    }
  }

  void visitOp(il::BlockId blockId, il::OpId opId) {
    const il::Op& op = function_.op(opId);
    switch (op.code) {
      case il::OpCode::Phi:
        visitPhi(blockId, opId);
        break;
      case il::OpCode::Load:
      case il::OpCode::Intrinsic:
      case il::OpCode::Call:
        // Memory and the opaque: a defined value we know nothing about.
        if (op.result.valid()) {
          updateCell(op.result, Cell::overdefined());
        }
        break;
      case il::OpCode::Branch:
        pushEdge(blockId, function_.targets(op)[0]);
        break;
      case il::OpCode::CondBranch: {
        const Cell condition = evaluate(function_.operands(op)[0]);
        const auto targets = function_.targets(op);
        if (condition.kind == Cell::Kind::Const) {
          pushEdge(blockId, targets[condition.value != 0 ? 0 : 1]);
        } else if (condition.kind == Cell::Kind::Overdefined) {
          pushEdge(blockId, targets[0]);
          pushEdge(blockId, targets[1]);
        }
        break;
      }
      case il::OpCode::IndirectBranch:
        for (const il::BlockId target : function_.targets(op)) {
          pushEdge(blockId, target);
        }
        break;
      default:
        break;
    }
  }

  void visitPhi(il::BlockId blockId, il::OpId opId) {
    const il::Op& op = function_.op(opId);
    const auto operands = function_.operands(op);
    const auto& predecessors = function_.block(blockId).predecessors;
    Cell merged{Cell::Kind::Unknown, 0};
    for (std::size_t index = 0; index < operands.size() && index < predecessors.size();
         ++index) {
      if (!execEdges_.contains(edgeKey(predecessors[index], blockId))) {
        continue;  // an edge the analysis has not reached contributes nothing
      }
      merged = meet(merged, evaluate(operands[index]));
    }
    updateCell(op.result, merged);
  }

  // -- rewriting ------------------------------------------------------------

  /// Constant cells become Const expressions at every use. Only values the
  /// analysis visited carry a cell, so unreachable regions are untouched.
  bool rewriteConstants() {
    ValueSubst subst;
    std::size_t count = 0;
    for (uint32_t valueIndex = 0; valueIndex < cells_.size(); ++valueIndex) {
      const Cell cell = cells_[valueIndex];
      if (cell.kind != Cell::Kind::Const) {
        continue;
      }
      const il::ValueId value{valueIndex};
      // Belt and braces: Const cells of non-scalar type have no Const form
      // (evaluate already declines them; never invent one here).
      const il::Type valueType = function_.value(value).type;
      if (!valueType.isScalarInteger() && !valueType.isFloat()) {
        continue;
      }
      subst.set(value, function_.constant(valueType, cell.value));
      ++count;
    }
    if (count == 0) {
      return false;
    }
    bool changed = false;
    for (const il::BlockId blockId : function_.blockHandles()) {
      for (const il::OpId opId : function_.block(blockId).ops) {
        const auto operands = function_.operands(function_.op(opId));
        if (operands.empty()) {
          continue;
        }
        std::vector<il::ExprId> rewritten(operands.begin(), operands.end());
        bool touched = false;
        for (il::ExprId& operand : rewritten) {
          const il::ExprId next = subst.apply(function_, operand);
          touched |= next != operand;
          operand = next;
        }
        if (touched) {
          function_.setOperands(opId, rewritten);
          changed = true;
        }
      }
    }
    return changed;
  }

  /// A branch whose condition is a known constant keeps one arm. The dropped
  /// successor's phis lose this edge's operand slot, edges are rebuilt once at
  /// the end, and the CFG is one arm closer to the code the author wrote.
  bool foldBranches() {
    bool changed = false;
    for (const il::BlockId blockId : function_.blockHandles()) {
      if (!blockId.valid() || execBlocks_[blockId.index()] == 0) {
        continue;  // unreachable: leave its shape for the cleanup pass
      }
      const il::Block& block = function_.block(blockId);
      const il::OpId terminatorId = block.terminator();
      if (!terminatorId.valid() || function_.op(terminatorId).code != il::OpCode::CondBranch) {
        continue;
      }
      const Cell condition = evaluate(function_.operands(function_.op(terminatorId))[0]);
      if (condition.kind != Cell::Kind::Const) {
        continue;
      }
      const auto targets = function_.targets(function_.op(terminatorId));
      const il::BlockId kept = targets[condition.value != 0 ? 0 : 1];
      const il::BlockId dropped = targets[condition.value != 0 ? 1 : 0];

      il::Op& terminator = function_.op(terminatorId);
      terminator.code = il::OpCode::Branch;
      function_.setOperands(terminatorId, {});
      function_.setTargets(terminatorId, std::vector<il::BlockId>{kept});

      // The dropped edge's phi slot vanishes. Both arms to the same block is
      // fine: one slot goes, one stays, and both held the same value anyway.
      // Predecessors are still the pre-loop CFG here, so multiple dropped edges
      // into the same target prune their own indices consistently when we
      // remove slots from highest to lowest, or let rebuildEdges run once?
      // Wait: if block A and B both drop edges to `dropped`, pruning inside the
      // loop mutates `dropped`'s phi operands while `dropped.predecessors` stays
      // un-rebuilt, which keeps slot indices aligned with `predecessors`!
      //Wait: if blockId appears at slot i and another block at slot j, pruning
      // slot i shifts subsequent indices in `phi.operands` vs `predecessors`!
      // Let's handle that cleanly: track pruned slots per block and erase in
      // descending slot order before one final `rebuildEdges()`.
      recordPrunedPhiSlot(dropped, blockId);
      changed = true;
    }
    if (changed) {
      applyPrunedPhiSlots();
      function_.rebuildEdges();
    }
    return changed;
  }

  void recordPrunedPhiSlot(il::BlockId blockId, il::BlockId from) {
    const auto& predecessors = function_.block(blockId).predecessors;
    const auto at = std::find(predecessors.begin(), predecessors.end(), from);
    if (at == predecessors.end()) {
      return;
    }
    prunedSlots_[blockId].push_back(static_cast<std::size_t>(at - predecessors.begin()));
  }

  void applyPrunedPhiSlots() {
    for (auto& [blockId, slots] : prunedSlots_) {
      std::sort(slots.begin(), slots.end(), std::greater<std::size_t>{});
      for (const il::OpId opId : function_.block(blockId).ops) {
        if (function_.op(opId).code != il::OpCode::Phi) {
          break;
        }
        const auto operands = function_.operands(function_.op(opId));
        std::vector<il::ExprId> pruned(operands.begin(), operands.end());
        for (const std::size_t slot : slots) {
          if (slot < pruned.size()) {
            pruned.erase(pruned.begin() + static_cast<std::ptrdiff_t>(slot));
          }
        }
        function_.setOperands(opId, pruned);
      }
    }
    prunedSlots_.clear();
  }

  static constexpr unsigned kMaxDepth = 64;

  il::Function& function_;
  std::vector<Cell> cells_;
  /// One top-level evaluation's answers, stamped with the evaluation they belong
  /// to, and the whole iteration's settled ones (see evaluate).
  std::vector<std::pair<uint64_t, Cell>> evaluated_;
  uint64_t generation_ = 0;
  std::vector<uint8_t> overdefined_;
  /// Whether the node being evaluated hit the depth cutoff, which is what makes
  /// its verdict provisional rather than settled.
  bool truncated_ = false;
  /// Scratch for visitPhisOnEdge, a member only to keep its allocation.
  std::vector<std::size_t> slots_;
  std::vector<std::vector<il::OpId>> uses_;
  std::vector<il::BlockId> opBlocks_;
  std::vector<uint32_t> leafSeenGen_;
  uint32_t leafGen_ = 0;
  std::vector<il::ExprId> leafWork_;
  /// Executable edges, keyed by the two block indices packed into one integer.
  /// Hashed rather than ordered because nothing here wants them in order and the
  /// membership test is the hottest operation in the pass: every phi asks it once
  /// per predecessor, so at a flattened dispatcher's head it is asked tens of
  /// millions of times, and a tree of pairs answers each with a chain of cache
  /// misses.
  std::unordered_set<uint64_t> execEdges_;
  std::vector<uint8_t> execBlocks_;
  std::vector<std::pair<il::BlockId, il::BlockId>> cfgWork_;
  std::vector<il::OpId> ssaWork_;
  std::unordered_map<il::BlockId, std::vector<std::size_t>> prunedSlots_;
};

/// Collapses phis until none can be, using a worklist over phi users so a
/// chain of N collapsible phis finishes in O(Phis + Ops) instead of scanning
/// the entire function N times.
bool simplifyPhis(il::Function& function) {
  struct PhiNode {
    il::BlockId block;
    il::OpId op;
    il::ValueId result;
    il::Type type;
    bool removed = false;
  };

  std::vector<PhiNode> phis;
  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      if (op.code != il::OpCode::Phi) {
        break;
      }
      phis.push_back(PhiNode{blockId, opId, op.result, op.type, false});
    }
  }
  if (phis.empty()) {
    return false;
  }

  std::vector<std::vector<std::size_t>> phiUsers(function.valueCount());
  std::vector<uint32_t> seenGen(function.exprCount(), 0);
  uint32_t gen = 0;
  std::vector<il::ExprId> exprWork;
  std::vector<il::ValueId> collectedLeaves;

  const auto collectLeaves = [&](il::ExprId root) {
    collectedLeaves.clear();
    if (++gen == 0) {
      std::fill(seenGen.begin(), seenGen.end(), 0);
      gen = 1;
    }
    exprWork.clear();
    exprWork.push_back(root);
    while (!exprWork.empty()) {
      const il::ExprId at = exprWork.back();
      exprWork.pop_back();
      const uint32_t idx = at.index();
      if (idx >= seenGen.size()) {
        seenGen.resize(std::max<std::size_t>(function.exprCount(), idx + 1), 0);
      }
      if (seenGen[idx] == gen) {
        continue;
      }
      seenGen[idx] = gen;
      const il::Expr& expr = function.expr(at);
      if (expr.op == il::ExprOp::Value) {
        collectedLeaves.push_back(il::ValueId{static_cast<uint32_t>(expr.immediate)});
        continue;
      }
      for (unsigned i = 0; i < expr.operandCount; ++i) {
        exprWork.push_back(expr.operands[i]);
      }
    }
  };

  for (std::size_t idx = 0; idx < phis.size(); ++idx) {
    for (const il::ExprId operand : function.operands(function.op(phis[idx].op))) {
      collectLeaves(operand);
      for (const il::ValueId val : collectedLeaves) {
        if (val.valid() && val.index() < phiUsers.size()) {
          phiUsers[val.index()].push_back(idx);
        }
      }
    }
  }

  std::vector<il::ExprId> substMap(function.valueCount());
  std::vector<std::size_t> collapseOrder;
  std::vector<std::size_t> work;
  work.reserve(phis.size());
  std::vector<uint8_t> inWork(phis.size(), 1);
  for (std::size_t idx = 0; idx < phis.size(); ++idx) {
    work.push_back(idx);
  }

  // Rewrites one expression through `substMap` using a short-lived local memo
  // so newly collapsed phis are immediately visible to dependent phis.
  std::unordered_map<il::ExprId, il::ExprId> localMemo;
  auto applyLocal = [&](auto& self, il::ExprId id) -> il::ExprId {
    if (const auto found = localMemo.find(id); found != localMemo.end()) {
      return found->second;
    }
    const il::Expr expr = function.expr(id);
    il::ExprId result = id;
    if (expr.op == il::ExprOp::Value) {
      const uint32_t vIdx = static_cast<uint32_t>(expr.immediate);
      if (vIdx < substMap.size() && substMap[vIdx].valid()) {
        result = substMap[vIdx];
      }
    } else if (expr.operandCount > 0) {
      il::Expr rebuilt = expr;
      bool changed = false;
      for (unsigned i = 0; i < expr.operandCount; ++i) {
        const il::ExprId next = self(self, expr.operands[i]);
        changed |= next != expr.operands[i];
        rebuilt.operands[i] = next;
      }
      if (changed) {
        result = function.intern(rebuilt);
      }
    }
    localMemo.emplace(id, result);
    return result;
  };

  while (!work.empty()) {
    const std::size_t idx = work.back();
    work.pop_back();
    inWork[idx] = 0;
    PhiNode& phi = phis[idx];
    if (phi.removed) {
      continue;
    }

    if (!collapseOrder.empty()) {
      localMemo.clear();
      const auto operands = function.operands(function.op(phi.op));
      std::vector<il::ExprId> rewritten(operands.begin(), operands.end());
      bool touched = false;
      for (il::ExprId& operand : rewritten) {
        const il::ExprId next = applyLocal(applyLocal, operand);
        touched |= next != operand;
        operand = next;
      }
      if (touched) {
        function.setOperands(phi.op, rewritten);
      }
    }

    il::ExprId distinct;
    bool collapsed = true;
    for (const il::ExprId operand : function.operands(function.op(phi.op))) {
      const il::Expr& expr = function.expr(operand);
      if (expr.op == il::ExprOp::Value &&
          il::ValueId{static_cast<uint32_t>(expr.immediate)} == phi.result) {
        continue;  // a self loop carries no information
      }
      if (!distinct.valid()) {
        distinct = operand;
      } else if (distinct != operand) {
        collapsed = false;
        break;
      }
    }
    if (!collapsed) {
      continue;
    }
    if (!distinct.valid()) {
      distinct = function.undefined(phi.type);
    }

    phi.removed = true;
    substMap[phi.result.index()] = distinct;
    collapseOrder.push_back(idx);

    collectLeaves(distinct);
    const auto& users = phiUsers[phi.result.index()];
    for (const std::size_t uIdx : users) {
      if (phis[uIdx].removed) {
        continue;
      }
      for (const il::ValueId newLeaf : collectedLeaves) {
        if (newLeaf.valid() && newLeaf.index() < phiUsers.size() &&
            newLeaf != phi.result) {
          phiUsers[newLeaf.index()].push_back(uIdx);
        }
      }
      if (inWork[uIdx] == 0) {
        inWork[uIdx] = 1;
        work.push_back(uIdx);
      }
    }
  }

  if (collapseOrder.empty()) {
    return false;
  }

  // Resolve chained substitutions in reverse collapse order so every collapsed
  // phi maps straight to its final expression in a single whole-function pass.
  ValueSubst subst;
  for (auto it = collapseOrder.rbegin(); it != collapseOrder.rend(); ++it) {
    const il::ValueId res = phis[*it].result;
    const il::ExprId resolved = subst.apply(function, substMap[res.index()]);
    subst.set(res, resolved);
  }

  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      if (function.op(opId).code == il::OpCode::Phi) {
        continue;  // surviving phis were already rewritten in place
      }
      const auto operands = function.operands(function.op(opId));
      if (operands.empty()) {
        continue;
      }
      std::vector<il::ExprId> rewritten(operands.begin(), operands.end());
      bool touched = false;
      for (il::ExprId& operand : rewritten) {
        const il::ExprId next = subst.apply(function, operand);
        touched |= next != operand;
        operand = next;
      }
      if (touched) {
        function.setOperands(opId, rewritten);
      }
    }
  }
  for (const PhiNode& phi : phis) {
    if (phi.removed) {
      function.removeOp(phi.block, phi.op);
    }
  }
  return true;
}

/// Demand-driven global DCE. A value is needed when a side-effecting op or a
/// branch condition reads it, or when it feeds the definition of a needed
/// value; everything else — phis and loads — goes. A single backward sweep from
/// observable roots already computes the exact live closure because removing
/// dead Phis and Loads never drops any operand of a live op.
bool dce(il::Function& function) {
  std::vector<uint8_t> needed(function.valueCount(), 0);
  std::vector<uint8_t> visitedExprs(function.exprCount(), 0);
  std::vector<il::ValueId> work;
  std::vector<il::ExprId> exprWork;

  const auto mark = [&](il::ExprId root) {
    exprWork.clear();
    exprWork.push_back(root);
    while (!exprWork.empty()) {
      const il::ExprId id = exprWork.back();
      exprWork.pop_back();
      const uint32_t idx = id.index();
      if (idx >= visitedExprs.size()) {
        visitedExprs.resize(std::max<std::size_t>(function.exprCount(), idx + 1), 0);
      }
      if (visitedExprs[idx] != 0) {
        continue;
      }
      visitedExprs[idx] = 1;
      const il::Expr& expr = function.expr(id);
      if (expr.op == il::ExprOp::Value) {
        const auto value = il::ValueId{static_cast<uint32_t>(expr.immediate)};
        if (value.valid() && value.index() < needed.size() && needed[value.index()] == 0) {
          needed[value.index()] = 1;
          work.push_back(value);
        }
        continue;
      }
      for (unsigned index = 0; index < expr.operandCount; ++index) {
        exprWork.push_back(expr.operands[index]);
      }
    }
  };

  // Roots: whatever the program observes — memory writes, calls, intrinsics,
  // and the conditions that steer control flow.
  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      if (!il::hasSideEffects(op.code) && !op.isTerminator()) {
        continue;
      }
      for (const il::ExprId operand : function.operands(op)) {
        mark(operand);
      }
    }
  }
  // Propagate: a needed value's definition needs its own inputs.
  while (!work.empty()) {
    const il::ValueId value = work.back();
    work.pop_back();
    const il::ValueInfo& info = function.value(value);
    if (!info.definition.valid() || !function.hasOp(info.definition)) {
      continue;
    }
    for (const il::ExprId operand : function.operands(function.op(info.definition))) {
      mark(operand);
    }
  }
  std::vector<std::pair<il::BlockId, il::OpId>> dead;
  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      if ((op.code == il::OpCode::Phi || op.code == il::OpCode::Load) &&
          op.result.valid() && op.result.index() < needed.size() &&
          needed[op.result.index()] == 0) {
        dead.emplace_back(blockId, opId);
      }
    }
  }
  if (dead.empty()) {
    return false;
  }
  for (const auto& [blockId, opId] : dead) {
    function.removeOp(blockId, opId);
  }
  return true;
}

/// True when an untracked vector/float root (`q0..q7`, `q16..q31`, etc.) is
/// caller-saved under AAPCS64 and therefore clobbered by any ordinary Call.
[[nodiscard]] bool isCallerSavedVectorRoot(std::string_view name) noexcept {
  if (name.size() < 2 || (name[0] != 'q' && name[0] != 'v' && name[0] != 'd')) {
    return false;
  }
  unsigned number = 0;
  const char* begin = name.data() + 1;
  const char* end = name.data() + name.size();
  const auto [ptr, ec] = std::from_chars(begin, end, number);
  if (ec != std::errc{} || ptr != end) {
    return false;
  }
  return number <= 7 || (number >= 16 && number <= 31);
}

/// Removes `WriteReg` ops on untracked (`Vector`/`Float`) registers when the
/// written value is never read before being completely overwritten or clobbered
/// by a `Call` in the same basic block.
bool deadUntrackedRegWrites(il::Function& function,
                            const analysis::CallNameSources& names) {
  std::vector<std::pair<il::BlockId, il::OpId>> dead;
  const auto& regs = function.registers();

  std::unordered_set<uint32_t> readRoots;
  bool hasSretX8 = false;
  std::vector<il::ExprId> exprWork;
  std::vector<uint8_t> seenExpr(function.exprCount(), 0);
  const auto scanSret = [&](il::ExprId root) {
    if (!root.valid()) {
      return;
    }
    exprWork.clear();
    exprWork.push_back(root);
    while (!exprWork.empty()) {
      const il::ExprId id = exprWork.back();
      exprWork.pop_back();
      if (id.index() < seenExpr.size()) {
        if (seenExpr[id.index()] != 0) {
          continue;
        }
        seenExpr[id.index()] = 1;
      }
      const il::Expr& e = function.expr(id);
      if (e.op == il::ExprOp::EntryReg) {
        const il::RegId r{static_cast<uint32_t>(e.immediate)};
        if (regs.contains(r) && regs.nameOf(r) == "x8") {
          hasSretX8 = true;
        }
      } else {
        for (unsigned i = 0; i < e.operandCount; ++i) {
          exprWork.push_back(e.operands[i]);
        }
      }
    }
  };
  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      if (op.code == il::OpCode::ReadReg) {
        readRoots.insert(regs.rootOf(op.reg()).index());
      }
      for (const il::ExprId operand : function.operands(op)) {
        scanSret(operand);
      }
    }
  }

  for (const il::BlockId blockId : function.blockHandles()) {
    std::unordered_map<uint32_t, std::vector<il::OpId>> pending;
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      if (op.code == il::OpCode::ReadReg) {
        const il::RegId root = regs.rootOf(op.reg());
        pending.erase(root.index());
        continue;
      }
      if (op.code == il::OpCode::WriteReg) {
        const il::RegId reg = op.reg();
        const il::RegId root = regs.rootOf(reg);
        const il::RegisterInfo& rootInfo = regs[root];
        if (rootInfo.regClass != il::RegClass::Vector &&
            rootInfo.regClass != il::RegClass::Float) {
          continue;
        }
        if (!readRoots.contains(root.index())) {
          const std::string_view rname = regs.nameOf(root);
          const bool isQ0 = (rname == "q0" || rname == "v0" || rname == "d0");
          if (isCallerSavedVectorRoot(rname) && (!isQ0 || hasSretX8)) {
            dead.emplace_back(blockId, opId);
            continue;
          }
        }
        const il::RegisterInfo& regInfo = regs[reg];
        if (!regInfo.isSubRegister() || regInfo.zeroExtendsParent) {
          if (const auto it = pending.find(root.index()); it != pending.end()) {
            for (const il::OpId prev : it->second) {
              dead.emplace_back(blockId, prev);
            }
            it->second.clear();
          }
        }
        pending[root.index()].push_back(opId);
        continue;
      }
      if (op.code == il::OpCode::Call) {
        const auto operands = function.operands(op);
        bool preserving = false;
        if (!operands.empty()) {
          const std::optional<std::string> targetName =
              analysis::callTargetName(function, operands[0], names);
          preserving = targetName.has_value() &&
                       analysis::isRegisterPreservingHelper(*targetName);
        }
        if (!preserving) {
          for (const auto& [rootIdx, ops] : pending) {
            if (isCallerSavedVectorRoot(regs.nameOf(il::RegId{rootIdx}))) {
              for (const il::OpId prev : ops) {
                dead.emplace_back(blockId, prev);
              }
            }
          }
        }
        pending.clear();
        continue;
      }
      if (op.code == il::OpCode::Intrinsic || op.code == il::OpCode::Unimplemented ||
          op.isTerminator()) {
        pending.clear();
      }
    }
  }

  if (dead.empty()) {
    return false;
  }
  for (const auto& [blockId, opId] : dead) {
    function.removeOp(blockId, opId);
  }
  return true;
}

/// Narrows 64-bit register Phis that only ever carry <=32-bit values and are
/// only ever read through <=32-bit truncations/low extracts, eliminating
/// spurious `(uint32_t)`/`(int64_t)` round-trips around 32-bit loop counters
/// and offsets.
bool narrowPhiWidths(il::Function& function) {
  struct CandPhi {
    il::BlockId block;
    il::OpId op;
    il::ValueId result;
  };

  std::vector<CandPhi> cands;
  std::vector<int32_t> candIndexByVal(function.valueCount(), -1);
  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      if (op.code != il::OpCode::Phi) {
        break;
      }
      if (op.result.valid() && op.type == il::Type::integer(64)) {
        candIndexByVal[op.result.index()] = static_cast<int32_t>(cands.size());
        cands.push_back(CandPhi{blockId, opId, op.result});
      }
    }
  }
  if (cands.empty()) {
    return false;
  }

  std::vector<uint8_t> bad(cands.size(), 0);
  // Symmetric dependencies: if `v` reads `u` directly as an i64 phi operand
  // and either one is disqualified, the other cannot narrow either.
  std::vector<std::vector<std::size_t>> deps(cands.size());
  std::vector<std::size_t> disqWork;

  const auto disqualify = [&](std::size_t idx) {
    if (bad[idx] == 0) {
      bad[idx] = 1;
      disqWork.push_back(idx);
    }
  };

  const auto isNarrowableInput = [&](il::ExprId id, std::size_t consumerIdx) -> bool {
    const il::Expr& expr = function.expr(id);
    switch (expr.op) {
      case il::ExprOp::Const:
      case il::ExprOp::Undef:
        return true;
      case il::ExprOp::ZExt:
      case il::ExprOp::SExt: {
        const il::Type innerType = function.expr(expr.operands[0]).type;
        return innerType.isScalarInteger() && innerType.bits() <= 32;
      }
      case il::ExprOp::Value: {
        const il::ValueId src{static_cast<uint32_t>(expr.immediate)};
        if (src.valid() && src.index() < candIndexByVal.size() &&
            candIndexByVal[src.index()] >= 0) {
          const std::size_t srcIdx = static_cast<std::size_t>(candIndexByVal[src.index()]);
          if (srcIdx != consumerIdx) {
            deps[consumerIdx].push_back(srcIdx);
            deps[srcIdx].push_back(consumerIdx);
          }
          return true;
        }
        return false;
      }
      default:
        return false;
    }
  };

  for (std::size_t idx = 0; idx < cands.size(); ++idx) {
    for (const il::ExprId operand : function.operands(function.op(cands[idx].op))) {
      if (!isNarrowableInput(operand, idx)) {
        disqualify(idx);
        break;
      }
    }
  }

  // Scan all non-disqualified-only uses across the entire function. Any use of
  // a candidate phi that is not directly under Trunc/Extract(offset 0) <=32
  // disqualifies that phi.
  std::vector<uint8_t> seenExpr(function.exprCount(), 0);
  std::vector<il::ExprId> exprWork;
  const auto scanExprOutsidePhi = [&](il::ExprId root) {
    exprWork.clear();
    exprWork.push_back(root);
    while (!exprWork.empty()) {
      const il::ExprId id = exprWork.back();
      exprWork.pop_back();
      if (id.index() < seenExpr.size()) {
        if (seenExpr[id.index()] != 0) {
          continue;
        }
        seenExpr[id.index()] = 1;
      }
      const il::Expr& expr = function.expr(id);
      if ((expr.op == il::ExprOp::Trunc ||
           (expr.op == il::ExprOp::Extract && expr.immediate == 0)) &&
          expr.type.isScalarInteger() && expr.type.bits() <= 32 &&
          expr.operandCount == 1) {
        const il::Expr& inner = function.expr(expr.operands[0]);
        if (inner.op == il::ExprOp::Value) {
          const il::ValueId val{static_cast<uint32_t>(inner.immediate)};
          if (val.valid() && val.index() < candIndexByVal.size() &&
              candIndexByVal[val.index()] >= 0) {
            continue;  // safe narrow consumer; do not flag val
          }
        }
      }
      if (expr.op == il::ExprOp::Value) {
        const il::ValueId val{static_cast<uint32_t>(expr.immediate)};
        if (val.valid() && val.index() < candIndexByVal.size() &&
            candIndexByVal[val.index()] >= 0) {
          disqualify(static_cast<std::size_t>(candIndexByVal[val.index()]));
        }
        continue;
      }
      for (unsigned i = 0; i < expr.operandCount; ++i) {
        exprWork.push_back(expr.operands[i]);
      }
    }
  };

  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      if (op.code == il::OpCode::Phi && op.result.valid() &&
          op.result.index() < candIndexByVal.size() &&
          candIndexByVal[op.result.index()] >= 0) {
        continue;  // checked via isNarrowableInput + reverseDeps
      }
      for (const il::ExprId operand : function.operands(op)) {
        scanExprOutsidePhi(operand);
      }
    }
  }

  while (!disqWork.empty()) {
    const std::size_t v = disqWork.back();
    disqWork.pop_back();
    for (const std::size_t u : deps[v]) {
      disqualify(u);
    }
  }

  std::size_t keptCount = 0;
  std::vector<il::ValueId> newValues(cands.size());
  const il::Type i32 = il::Type::integer(32);
  for (std::size_t idx = 0; idx < cands.size(); ++idx) {
    if (bad[idx] != 0) {
      continue;
    }
    ++keptCount;
    const il::Op& oldOp = function.op(cands[idx].op);
    newValues[idx] = function.prependPhi(cands[idx].block, oldOp.va, i32);
  }
  if (keptCount == 0) {
    return false;
  }

  const auto narrowInput = [&](il::ExprId id) -> il::ExprId {
    const il::Expr expr = function.expr(id);
    switch (expr.op) {
      case il::ExprOp::Const:
        return function.constant(i32, expr.immediate & 0xffffffffu);
      case il::ExprOp::Undef:
        return function.undefined(i32);
      case il::ExprOp::ZExt:
      case il::ExprOp::SExt: {
        const il::ExprId inner = expr.operands[0];
        const il::Type innerType = function.expr(inner).type;
        if (innerType == i32) {
          return inner;
        }
        return function.cast(expr.op, i32, inner);
      }
      case il::ExprOp::Value: {
        const il::ValueId src{static_cast<uint32_t>(expr.immediate)};
        const std::size_t srcIdx = static_cast<std::size_t>(candIndexByVal[src.index()]);
        return function.valueRef(newValues[srcIdx]);
      }
      default:
        return id;
    }
  };

  for (std::size_t idx = 0; idx < cands.size(); ++idx) {
    if (bad[idx] != 0) {
      continue;
    }
    const auto oldOperands = function.operands(function.op(cands[idx].op));
    std::vector<il::ExprId> narrowedOps;
    narrowedOps.reserve(oldOperands.size());
    for (const il::ExprId operand : oldOperands) {
      narrowedOps.push_back(narrowInput(operand));
    }
    const il::OpId newOp = function.value(newValues[idx]).definition;
    function.setOperands(newOp, narrowedOps);
  }

  // Rewrite every consumer `Trunc:iW(val(oldPhi))` / `Extract:iW(val(oldPhi), 0)`.
  std::unordered_map<il::ExprId, il::ExprId> rewriteMemo;
  auto rewriteExpr = [&](auto& self, il::ExprId id) -> il::ExprId {
    if (const auto found = rewriteMemo.find(id); found != rewriteMemo.end()) {
      return found->second;
    }
    const il::Expr expr = function.expr(id);
    il::ExprId result = id;
    if ((expr.op == il::ExprOp::Trunc ||
         (expr.op == il::ExprOp::Extract && expr.immediate == 0)) &&
        expr.type.isScalarInteger() && expr.type.bits() <= 32 &&
        expr.operandCount == 1) {
      const il::Expr& inner = function.expr(expr.operands[0]);
      if (inner.op == il::ExprOp::Value) {
        const il::ValueId val{static_cast<uint32_t>(inner.immediate)};
        if (val.valid() && val.index() < candIndexByVal.size() &&
            candIndexByVal[val.index()] >= 0) {
          const std::size_t cIdx = static_cast<std::size_t>(candIndexByVal[val.index()]);
          if (bad[cIdx] == 0) {
            const il::ExprId newRef = function.valueRef(newValues[cIdx]);
            result = expr.type == i32 ? newRef
                                      : function.cast(il::ExprOp::Trunc, expr.type, newRef);
            rewriteMemo.emplace(id, result);
            return result;
          }
        }
      }
    }
    if (expr.operandCount > 0) {
      il::Expr rebuilt = expr;
      bool changed = false;
      for (unsigned i = 0; i < expr.operandCount; ++i) {
        const il::ExprId next = self(self, expr.operands[i]);
        changed |= next != expr.operands[i];
        rebuilt.operands[i] = next;
      }
      if (changed) {
        result = function.intern(rebuilt);
      }
    }
    rewriteMemo.emplace(id, result);
    return result;
  };

  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      if (op.code == il::OpCode::Phi && op.result.valid() &&
          op.result.index() < candIndexByVal.size() &&
          candIndexByVal[op.result.index()] >= 0 &&
          bad[static_cast<std::size_t>(candIndexByVal[op.result.index()])] == 0) {
        continue;
      }
      const auto operands = function.operands(op);
      if (operands.empty()) {
        continue;
      }
      std::vector<il::ExprId> rewritten(operands.begin(), operands.end());
      bool touched = false;
      for (il::ExprId& operand : rewritten) {
        const il::ExprId next = rewriteExpr(rewriteExpr, operand);
        touched |= next != operand;
        operand = next;
      }
      if (touched) {
        function.setOperands(opId, rewritten);
      }
    }
  }

  for (std::size_t idx = 0; idx < cands.size(); ++idx) {
    if (bad[idx] == 0) {
      function.removeOp(cands[idx].block, cands[idx].op);
    }
  }
  return true;
}

/// Elides pure-Phi trampoline blocks `B` (`Phi*` followed by `Branch(Next)`)
/// whose Phis are consumed exclusively by `Next`'s own Phis (e.g. `b2342` in
/// multi-arm merges), rewiring `B`'s predecessors straight to `Next` and
/// substituting `B`'s Phis into `Next`'s expanded Phi operand slots.
bool elidePhiTrampolines(il::Function& function) {
  struct PhiUseSummary {
    bool hasNonPhiUse = false;
    bool multipleBlocks = false;
    il::BlockId onlyPhiBlock{};
  };

  std::vector<PhiUseSummary> useSummary(function.valueCount());
  std::vector<uint32_t> seenGen(function.exprCount(), 0);
  uint32_t gen = 0;
  std::vector<il::ExprId> exprWork;

  const auto recordUses = [&](il::ExprId root, il::BlockId userBlock, bool isPhi) {
    if (++gen == 0) {
      std::fill(seenGen.begin(), seenGen.end(), 0);
      gen = 1;
    }
    exprWork.clear();
    exprWork.push_back(root);
    while (!exprWork.empty()) {
      const il::ExprId id = exprWork.back();
      exprWork.pop_back();
      if (id.index() < seenGen.size()) {
        if (seenGen[id.index()] == gen) {
          continue;
        }
        seenGen[id.index()] = gen;
      }
      const il::Expr& expr = function.expr(id);
      if (expr.op == il::ExprOp::Value) {
        const auto v = il::ValueId{static_cast<uint32_t>(expr.immediate)};
        if (v.valid() && v.index() < useSummary.size()) {
          PhiUseSummary& s = useSummary[v.index()];
          if (!isPhi) {
            s.hasNonPhiUse = true;
          } else if (!s.onlyPhiBlock.valid()) {
            s.onlyPhiBlock = userBlock;
          } else if (s.onlyPhiBlock != userBlock) {
            s.multipleBlocks = true;
          }
        }
        continue;
      }
      for (unsigned i = 0; i < expr.operandCount; ++i) {
        exprWork.push_back(expr.operands[i]);
      }
    }
  };

  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      const bool isPhi = op.code == il::OpCode::Phi;
      for (const il::ExprId operand : function.operands(op)) {
        recordUses(operand, blockId, isPhi);
      }
    }
  }

  constexpr std::size_t kMaxExpandedPreds = 16;
  bool anyChanged = false;
  for (unsigned pass = 0; pass < 4; ++pass) {
    struct TrampolinePlan {
      il::BlockId B;
      il::BlockId Next;
      uint64_t bTermVa = 0;
      std::vector<il::BlockId> predsB;
      std::vector<il::BlockId> oldPredsNext;
      std::size_t bSlotInNext = 0;
      std::unordered_map<uint32_t, std::vector<il::ExprId>> bPhiInputs;
      std::unordered_map<uint32_t, std::size_t> bSlotOfPred;
      struct NextPhiSnapshot {
        il::OpId opId;
        std::vector<il::ExprId> oldOperands;
      };
      std::vector<NextPhiSnapshot> nextPhis;
    };

    std::vector<uint8_t> touched(function.blockCount(), 0);
    std::vector<TrampolinePlan> plans;

    for (const il::BlockId B : function.blockHandles()) {
      if (B == function.entryBlock() || touched[B.index()] != 0) {
        continue;
      }
      const auto& opsB = function.block(B).ops;
      // Only elide blocks that actually carry Phi(s); 0-Phi forwarding blocks
      // are `trampoline-fold`'s domain at Cfg maturity, and SsaOptimize tests
      // expect 0-Phi empty arms to keep their CFG slots.
      if (opsB.size() < 2 || function.block(B).external) {
        continue;
      }
      const il::Op& termB = function.op(opsB.back());
      if (termB.code != il::OpCode::Branch) {
        continue;
      }
      bool onlyPhis = true;
      for (std::size_t idx = 0; idx + 1 < opsB.size(); ++idx) {
        if (function.op(opsB[idx]).code != il::OpCode::Phi) {
          onlyPhis = false;
          break;
        }
      }
      if (!onlyPhis) {
        continue;
      }
      const auto targetsB = function.targets(termB);
      if (targetsB.size() != 1) {
        continue;
      }
      const il::BlockId Next = targetsB[0];
      if (!Next.valid() || Next == B || Next == function.entryBlock() ||
          function.block(Next).external || touched[Next.index()] != 0) {
        continue;
      }
      const auto predsB = function.block(B).predecessors;
      if (predsB.empty() || predsB.size() > kMaxExpandedPreds) {
        continue;
      }

      bool phisClean = true;
      for (std::size_t idx = 0; idx + 1 < opsB.size(); ++idx) {
        const il::ValueId v = function.op(opsB[idx]).result;
        if (!v.valid() || v.index() >= useSummary.size()) {
          phisClean = false;
          break;
        }
        const PhiUseSummary& s = useSummary[v.index()];
        if (s.hasNonPhiUse || s.multipleBlocks ||
            (s.onlyPhiBlock.valid() && s.onlyPhiBlock != Next)) {
          phisClean = false;
          break;
        }
      }
      if (!phisClean) {
        continue;
      }

      const auto oldPredsNext = function.block(Next).predecessors;
      bool predsClean = true;
      for (std::size_t i = 0; i < predsB.size() && predsClean; ++i) {
        const il::BlockId p = predsB[i];
        if (!p.valid() || p == B || touched[p.index()] != 0) {
          predsClean = false;
          break;
        }
        const il::OpId pTermId = function.block(p).terminator();
        if (!pTermId.valid() ||
            function.op(pTermId).code == il::OpCode::IndirectBranch) {
          predsClean = false;
          break;
        }
        for (std::size_t j = i + 1; j < predsB.size(); ++j) {
          if (predsB[j] == p) {
            predsClean = false;
            break;
          }
        }
        if (std::find(oldPredsNext.begin(), oldPredsNext.end(), p) != oldPredsNext.end()) {
          predsClean = false;
          break;
        }
      }
      if (!predsClean) {
        continue;
      }

      TrampolinePlan plan;
      plan.B = B;
      plan.Next = Next;
      plan.bTermVa = termB.va;
      plan.predsB.assign(predsB.begin(), predsB.end());
      plan.oldPredsNext.assign(oldPredsNext.begin(), oldPredsNext.end());
      for (const il::OpId opId : function.block(Next).ops) {
        const il::Op& op = function.op(opId);
        if (op.code != il::OpCode::Phi) {
          break;
        }
        const auto operands = function.operands(op);
        plan.nextPhis.push_back(TrampolinePlan::NextPhiSnapshot{
            opId, std::vector<il::ExprId>(operands.begin(), operands.end())});
      }
      for (std::size_t idx = 0; idx + 1 < opsB.size(); ++idx) {
        const il::Op& phiB = function.op(opsB[idx]);
        const auto ops = function.operands(phiB);
        plan.bPhiInputs.emplace(phiB.result.index(),
                                std::vector<il::ExprId>(ops.begin(), ops.end()));
      }
      for (std::size_t slot = 0; slot < predsB.size(); ++slot) {
        plan.bSlotOfPred[predsB[slot].index()] = slot;
      }
      plan.bSlotInNext = oldPredsNext.size();
      for (std::size_t slot = 0; slot < oldPredsNext.size(); ++slot) {
        if (oldPredsNext[slot] == B) {
          plan.bSlotInNext = slot;
          break;
        }
      }
      if (plan.bSlotInNext >= oldPredsNext.size() && !plan.nextPhis.empty()) {
        continue;
      }

      touched[B.index()] = 1;
      touched[Next.index()] = 1;
      for (const il::BlockId p : predsB) {
        touched[p.index()] = 1;
      }
      plans.push_back(std::move(plan));
    }

    if (plans.empty()) {
      break;
    }

    // Apply all disjoint terminator rewirings in one batch, then rebuildEdges ONCE.
    for (const TrampolinePlan& plan : plans) {
      for (const il::BlockId p : plan.predsB) {
        const il::OpId pTermId = function.block(p).terminator();
        if (!pTermId.valid()) {
          continue;
        }
        const auto pTargets = function.targets(function.op(pTermId));
        std::vector<il::BlockId> rewritten(pTargets.begin(), pTargets.end());
        for (il::BlockId& t : rewritten) {
          if (t == plan.B) {
            t = plan.Next;
          }
        }
        function.setTargets(pTermId, rewritten);
      }
      function.dropTerminator(plan.B);
      (void)function.appendUnreachable(plan.B, plan.bTermVa);
    }

    function.rebuildEdges();

    for (const TrampolinePlan& plan : plans) {
      if (!plan.nextPhis.empty()) {
        const auto newPredsNext = function.block(plan.Next).predecessors;
        std::unordered_map<uint32_t, std::size_t> oldSlotOfPred;
        for (std::size_t slot = 0; slot < plan.oldPredsNext.size(); ++slot) {
          oldSlotOfPred.try_emplace(plan.oldPredsNext[slot].index(), slot);
        }
        for (const auto& snap : plan.nextPhis) {
          std::vector<il::ExprId> newOperands;
          newOperands.reserve(newPredsNext.size());
          const il::ExprId bOrigExpr =
              plan.bSlotInNext < snap.oldOperands.size()
                  ? snap.oldOperands[plan.bSlotInNext]
                  : il::ExprId{};
          for (const il::BlockId pred : newPredsNext) {
            if (const auto bIt = plan.bSlotOfPred.find(pred.index());
                bIt != plan.bSlotOfPred.end()) {
              const std::size_t slotInB = bIt->second;
              ValueSubst subst;
              for (const auto& [bValIdx, vec] : plan.bPhiInputs) {
                if (slotInB < vec.size()) {
                  subst.set(il::ValueId{bValIdx}, vec[slotInB]);
                }
              }
              newOperands.push_back(subst.apply(function, bOrigExpr));
            } else if (const auto oldIt = oldSlotOfPred.find(pred.index());
                       oldIt != oldSlotOfPred.end() &&
                       oldIt->second < snap.oldOperands.size()) {
              newOperands.push_back(snap.oldOperands[oldIt->second]);
            }
          }
          function.setOperands(snap.opId, newOperands);
        }
      }
      while (function.block(plan.B).ops.size() > 1) {
        function.removeOp(plan.B, function.block(plan.B).ops.front());
      }
    }
    anyChanged = true;
    // Refresh useSummary only if running another batch.
    if (pass + 1 < 4) {
      std::fill(useSummary.begin(), useSummary.end(), PhiUseSummary{});
      for (const il::BlockId blockId : function.blockHandles()) {
        for (const il::OpId opId : function.block(blockId).ops) {
          const il::Op& op = function.op(opId);
          const bool isPhi = op.code == il::OpCode::Phi;
          for (const il::ExprId operand : function.operands(op)) {
            recordUses(operand, blockId, isPhi);
          }
        }
      }
    }
  }
  return anyChanged;
}

class SsaOptimize final : public pass::FunctionPass {
 public:
  SsaOptimize()
      : FunctionPass([] {
          pass::PassInfo info;
          info.name = "ssa-optimize";
          info.level = il::Maturity::Ssa;
          // Produces Ssa, not Optimized: Resolved stands between, and that is
          // P8's gate (see the header).
          info.produces = il::Maturity::Ssa;
          info.fixpoint = true;
          info.invalidates = {"dominators", "scc"};
          return info;
        }()) {}

  Result<bool> run(pass::Context& context) override {
    il::Function& function = context.function();
    Clock clock;
    // A flags bundle written in one block and tested in another is a read of
    // nzcv until SSA replaces that read with the version that defined it, so
    // local-simplify sees no FlagCond-over-FlagDef to rewrite and the test stays
    // an opaque condition code. Folding again here is what turns a cross-block
    // `b.ne` into the compare it is — and an opaque condition is a wall to
    // everything downstream, propagation and switch recovery included.
    const bool unflagged = subPhase("flags", [&] { return foldFlagConditions(function); });
    const int64_t flags = clock.lap();
    XDEC_LOG_DEBUG(optimizeLog(), "  flags {}ms{}", flags, unflagged ? " changed" : "");
    const bool propagated = subPhase("sccp", [&] { return Sccp(function).run(); });
    const int64_t sccp = clock.lap();
    XDEC_LOG_DEBUG(optimizeLog(), "  sccp {}ms{}", sccp, propagated ? " changed" : "");
    // Algebra after propagation: constants the analysis materialised plug
    // straight into the rules, and the rules unblock the next propagation.
    const bool simplified = subPhase("algebra", [&] { return simplifyAlgebra(function); });
    const int64_t algebra = clock.lap();
    XDEC_LOG_DEBUG(optimizeLog(), "  algebra {}ms{}", algebra, simplified ? " changed" : "");
    const bool merged = subPhase("phis", [&] { return simplifyPhis(function); });
    const int64_t phis = clock.lap();
    XDEC_LOG_DEBUG(optimizeLog(), "  phis {}ms{}", phis, merged ? " changed" : "");
    const bool narrowed = subPhase("narrow-phis", [&] { return narrowPhiWidths(function); });
    const int64_t narrowMs = clock.lap();
    XDEC_LOG_DEBUG(optimizeLog(), "  narrow-phis {}ms{}", narrowMs,
                   narrowed ? " changed" : "");
    const bool elidedTrampolines =
        subPhase("phi-trampolines", [&] { return elidePhiTrampolines(function); });
    const int64_t trampMs = clock.lap();
    XDEC_LOG_DEBUG(optimizeLog(), "  phi-trampolines {}ms{}", trampMs,
                   elidedTrampolines ? " changed" : "");
    const bool collected = subPhase("dce", [&] {
      bool any = dce(function);
      any |= deadUntrackedRegWrites(function, callNameSources(context));
      return any;
    });
    // This pass runs to fixpoint over six sub-analyses of quite different cost,
    // so its total says nothing about which one to look at — and a sub-analysis
    // that reports a change it did not make costs the whole pass another
    // iteration, so which one claimed what is worth the same line.
    XDEC_LOG_DEBUG(
        optimizeLog(),
        "flags {}ms{}, sccp {}ms{}, algebra {}ms{}, phis {}ms{}, narrow {}ms{}, tramp {}ms{}, dce {}ms{}, {} expression(s)",
        flags, unflagged ? " changed" : "", sccp, propagated ? " changed" : "", algebra,
        simplified ? " changed" : "", phis, merged ? " changed" : "", narrowMs,
        narrowed ? " changed" : "", trampMs, elidedTrampolines ? " changed" : "",
        clock.lap(), collected ? " changed" : "", function.exprCount());
    return unflagged || propagated || simplified || merged || narrowed ||
           elidedTrampolines || collected;
  }
};

}  // namespace

std::unique_ptr<pass::Pass> makeSsaOptimizePass() {
  return std::make_unique<SsaOptimize>();
}

}  // namespace xdec::passes
