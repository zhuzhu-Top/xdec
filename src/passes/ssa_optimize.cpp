// makeSsaOptimizePass: SCCP, phi simplification, and global DCE (see the
// header for what lives here and why it stays at Ssa maturity).
#include "xdec/passes/ssa_optimize.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "xdec/il/ceval.h"
#include "xdec/il/function.h"
#include "xdec/support/log.h"

#include "algebra.h"
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
  explicit Sccp(il::Function& function) : function_(function) {}

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

  void indexUses() {
    for (const il::BlockId blockId : function_.blockHandles()) {
      for (const il::OpId opId : function_.block(blockId).ops) {
        opBlocks_[opId] = blockId;
        for (const il::ExprId operand : function_.operands(function_.op(opId))) {
          collectValueLeaves(operand, opId);
        }
      }
    }
  }

  void collectValueLeaves(il::ExprId id, il::OpId user) {
    // Iterative, like the verifier's walker: substituted chains are deeper
    // than any call stack this pass may rely on.
    std::vector<il::ExprId> work{id};
    std::unordered_set<uint32_t> seen;
    while (!work.empty()) {
      const il::ExprId at = work.back();
      work.pop_back();
      if (!seen.insert(at.index()).second) {
        continue;
      }
      const il::Expr& expr = function_.expr(at);
      if (expr.op == il::ExprOp::Value) {
        uses_[il::ValueId{static_cast<uint32_t>(expr.immediate)}].push_back(user);
        continue;
      }
      for (unsigned index = 0; index < expr.operandCount; ++index) {
        work.push_back(expr.operands[index]);
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
    if (overdefined_.contains(id)) {
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
    } else if (const auto found = evaluated_.find(id);
               found != evaluated_.end() && found->second.first == generation_) {
      return found->second.second;
    }
    const bool outerTruncated = truncated_;
    truncated_ = false;
    const Cell cell = evaluateNode(id, depth);
    const bool nodeTruncated = truncated_;
    truncated_ = outerTruncated || nodeTruncated;
    if (cell.kind == Cell::Kind::Overdefined) {
      // Unless the depth cutoff was involved, in which case the verdict is about
      // where this walk started rather than about the node, and a walk that
      // reaches it from closer by may do better.
      if (!nodeTruncated) {
        overdefined_.insert(id);
      }
      return cell;
    }
    evaluated_.insert_or_assign(id, std::pair{generation_, cell});
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
        return cells_[il::ValueId{static_cast<uint32_t>(expr.immediate)}];
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
    Cell& cell = cells_[value];
    if (next.kind == cell.kind && (next.kind != Cell::Kind::Const || next.value == cell.value)) {
      return;
    }
    cell = next;
    const auto found = uses_.find(value);
    if (found == uses_.end()) {
      return;
    }
    for (const il::OpId user : found->second) {
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
    if (execBlocks_.insert(blockId).second) {
      visitBlock(blockId);
    }
  }

  void drain() {
    while (!cfgWork_.empty() || !ssaWork_.empty()) {
      if (!cfgWork_.empty()) {
        const auto [from, to] = cfgWork_.back();
        cfgWork_.pop_back();
        if (execBlocks_.insert(to).second) {
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
      if (execBlocks_.contains(opBlocks_[opId])) {
        visitOp(opBlocks_[opId], opId);
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
      Cell merged = cells_[op.result];
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
    for (const auto& [value, cell] : cells_) {
      if (cell.kind != Cell::Kind::Const) {
        continue;
      }
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
  /// successor's phis lose this edge's operand slot, edges are rebuilt, and
  /// the CFG is one arm closer to the code the author wrote.
  bool foldBranches() {
    bool changed = false;
    for (const il::BlockId blockId : function_.blockHandles()) {
      if (!execBlocks_.contains(blockId)) {
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
      prunePhiSlot(dropped, blockId);
      function_.rebuildEdges();
      changed = true;
    }
    return changed;
  }

  /// Removes the operand slot edge `from` fed in each phi heading `block`.
  void prunePhiSlot(il::BlockId blockId, il::BlockId from) {
    const auto& predecessors = function_.block(blockId).predecessors;
    const auto at = std::find(predecessors.begin(), predecessors.end(), from);
    if (at == predecessors.end()) {
      return;
    }
    const std::size_t slot = static_cast<std::size_t>(at - predecessors.begin());
    for (const il::OpId opId : function_.block(blockId).ops) {
      if (function_.op(opId).code != il::OpCode::Phi) {
        break;
      }
      const auto operands = function_.operands(function_.op(opId));
      if (slot >= operands.size()) {
        continue;
      }
      std::vector<il::ExprId> pruned(operands.begin(), operands.end());
      pruned.erase(pruned.begin() + static_cast<std::ptrdiff_t>(slot));
      function_.setOperands(opId, pruned);
    }
  }

  static constexpr unsigned kMaxDepth = 64;

  il::Function& function_;
  std::unordered_map<il::ValueId, Cell> cells_;
  /// One top-level evaluation's answers, stamped with the evaluation they belong
  /// to, and the whole iteration's settled ones (see evaluate).
  std::unordered_map<il::ExprId, std::pair<uint64_t, Cell>> evaluated_;
  uint64_t generation_ = 0;
  std::unordered_set<il::ExprId> overdefined_;
  /// Whether the node being evaluated hit the depth cutoff, which is what makes
  /// its verdict provisional rather than settled.
  bool truncated_ = false;
  /// Scratch for visitPhisOnEdge, a member only to keep its allocation.
  std::vector<std::size_t> slots_;
  std::unordered_map<il::ValueId, std::vector<il::OpId>> uses_;
  std::unordered_map<il::OpId, il::BlockId> opBlocks_;
  /// Executable edges, keyed by the two block indices packed into one integer.
  /// Hashed rather than ordered because nothing here wants them in order and the
  /// membership test is the hottest operation in the pass: every phi asks it once
  /// per predecessor, so at a flattened dispatcher's head it is asked tens of
  /// millions of times, and a tree of pairs answers each with a chain of cache
  /// misses.
  std::unordered_set<uint64_t> execEdges_;
  std::unordered_set<il::BlockId> execBlocks_;
  std::vector<std::pair<il::BlockId, il::BlockId>> cfgWork_;
  std::vector<il::OpId> ssaWork_;
};

/// A phi that merges nothing is not a phi: one predecessor, or every distinct
/// non-self input the same expression, and the phi is that expression.
/// One generation of collapsible phis: those whose inputs, self-references
/// aside, are all the same expression.
bool simplifyPhisOnce(il::Function& function) {
  ValueSubst subst;
  std::vector<std::pair<il::BlockId, il::OpId>> removable;
  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      if (op.code != il::OpCode::Phi) {
        break;
      }
      const auto operands = function.operands(op);
      il::ExprId distinct;
      bool collapsed = true;
      for (const il::ExprId operand : operands) {
        const il::Expr& expr = function.expr(operand);
        if (expr.op == il::ExprOp::Value &&
            il::ValueId{static_cast<uint32_t>(expr.immediate)} == op.result) {
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
      // No distinct input at all (self-only, or zero predecessors): unknown.
      if (!distinct.valid()) {
        distinct = function.undefined(op.type);
      }
      subst.set(op.result, distinct);
      removable.emplace_back(blockId, opId);
    }
  }
  if (removable.empty()) {
    return false;
  }
  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
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
  for (const auto& [blockId, opId] : removable) {
    function.removeOp(blockId, opId);
  }
  return true;
}

/// Collapses phis until none can be, which is the pass's own job rather than its
/// caller's: collapsing one phi is what makes the phi reading it collapsible, so
/// a single generation leaves a chain of them one link shorter. The enclosing
/// pass runs to fixpoint and would get there too, but it would re-run the
/// propagation and the algebra once per link, and at a flattened dispatcher those
/// chains are as long as the dispatcher has states.
bool simplifyPhis(il::Function& function) {
  bool changed = false;
  while (simplifyPhisOnce(function)) {
    changed = true;
  }
  return changed;
}

void collectUsed(il::Function& function, il::ExprId id, std::unordered_set<uint32_t>& needed,
                 std::vector<il::ValueId>& work) {
  const il::Expr& expr = function.expr(id);
  if (expr.op == il::ExprOp::Value) {
    const auto value = il::ValueId{static_cast<uint32_t>(expr.immediate)};
    if (needed.insert(value.index()).second) {
      work.push_back(value);
    }
    return;
  }
  for (unsigned index = 0; index < expr.operandCount; ++index) {
    collectUsed(function, expr.operands[index], needed, work);
  }
}

/// Demand-driven global DCE. A value is needed when a side-effecting op or a
/// branch condition reads it, or when it feeds the definition of a needed
/// value; everything else — phis and loads — goes. The demand walk is what a
/// plain "no uses" scan cannot do: a phi cycle whose only consumer is the
/// cycle itself has uses everywhere and means nothing.
bool dce(il::Function& function) {
  bool any = false;
  while (true) {
    std::unordered_set<uint32_t> needed;
    std::vector<il::ValueId> work;
    const auto mark = [&](il::ExprId root) {
      collectUsed(function, root, needed, work);
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
            op.result.valid() && !needed.contains(op.result.index())) {
          dead.emplace_back(blockId, opId);
        }
      }
    }
    if (dead.empty()) {
      return any;
    }
    any = true;
    for (const auto& [blockId, opId] : dead) {
      function.removeOp(blockId, opId);
    }
  }
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
    const bool unflagged = foldFlagConditions(function);
    const int64_t flags = clock.lap();
    const bool propagated = Sccp(function).run();
    const int64_t sccp = clock.lap();
    // Algebra after propagation: constants the analysis materialised plug
    // straight into the rules, and the rules unblock the next propagation.
    const bool simplified = simplifyAlgebra(function);
    const int64_t algebra = clock.lap();
    const bool merged = simplifyPhis(function);
    const int64_t phis = clock.lap();
    const bool collected = dce(function);
    // This pass runs to fixpoint over four sub-analyses of quite different cost,
    // so its total says nothing about which one to look at — and a sub-analysis
    // that reports a change it did not make costs the whole pass another
    // iteration, so which one claimed what is worth the same line.
    XDEC_LOG_DEBUG(
        optimizeLog(),
        "flags {}ms{}, sccp {}ms{}, algebra {}ms{}, phis {}ms{}, dce {}ms{}, {} expression(s)",
        flags, unflagged ? " changed" : "", sccp, propagated ? " changed" : "", algebra,
        simplified ? " changed" : "", phis, merged ? " changed" : "", clock.lap(),
        collected ? " changed" : "", function.exprCount());
    return unflagged || propagated || simplified || merged || collected;
  }
};

}  // namespace

std::unique_ptr<pass::Pass> makeSsaOptimizePass() {
  return std::make_unique<SsaOptimize>();
}

}  // namespace xdec::passes
