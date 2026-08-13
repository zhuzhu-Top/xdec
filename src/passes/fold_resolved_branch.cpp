// fold-resolved-branch (see the header for the contract).
#include "xdec/passes/fold_resolved_branch.h"

#include <set>
#include <utility>
#include <vector>

#include "xdec/il/expr_roots.h"
#include "xdec/il/function.h"

namespace xdec::passes {

namespace {

/// A resolved branch's discarded target expression may have been the last
/// reader of the load(s) that computed it -- the jump-table fetch itself,
/// and (for a two-level dispatch) any secondary pointer-table load feeding
/// it. Each load the expression touches is removed once nothing else in the
/// function still reads its result, the same "no other use anywhere" test
/// c_stmt.cpp's emit-time dead-load checks ask, just answered here at the IL
/// level, where the fold that could have orphaned it just happened -- so the
/// op is gone before emit ever sees it, rather than merely hidden from it.
///
/// Repeated to a fixpoint: removing the secondary table's load can be
/// exactly what makes the index computation it depended on unused too, and
/// that dependency can nest more than one level deep on a multi-stage
/// dispatch.
void removeOrphanedLoads(il::Function& function,
                         const std::vector<il::ExprId>& discardedTargets) {
  std::set<uint32_t> candidates;
  for (const il::ExprId root : discardedTargets) {
    il::collectValueLeaves(function, root, candidates);
  }

  bool removedAny = true;
  while (removedAny && !candidates.empty()) {
    removedAny = false;
    std::set<uint32_t> reached;
    for (const il::BlockId blockId : function.blockHandles()) {
      for (const il::OpId opId : function.block(blockId).ops) {
        for (const il::ExprId operand : function.operands(function.op(opId))) {
          il::collectValueLeaves(function, operand, reached);
        }
      }
    }
    std::set<uint32_t> nextCandidates;
    for (const uint32_t index : candidates) {
      const il::ValueId value{index};
      if (!function.hasValue(value) || reached.contains(index)) {
        continue;
      }
      const il::OpId definition = function.value(value).definition;
      if (!definition.valid() || !function.hasOp(definition) ||
          function.op(definition).code != il::OpCode::Load) {
        continue;
      }
      // Dead: gone, and whatever its own address read is worth reconsidering
      // next round -- that is how a secondary table load's removal can reach
      // back and orphan the index computation that fed it.
      for (const il::ExprId operand : function.operands(function.op(definition))) {
        il::collectValueLeaves(function, operand, nextCandidates);
      }
      function.removeOp(function.value(value).block, definition);
      removedAny = true;
    }
    candidates = std::move(nextCandidates);
  }
}

class FoldResolvedBranch final : public pass::FunctionPass {
 public:
  FoldResolvedBranch()
      : FunctionPass([] {
          pass::PassInfo info;
          info.name = "fold-resolved-branch";
          info.level = il::Maturity::Resolved;
          info.produces = il::Maturity::Resolved;
          info.requirements = {"resolve-indirect"};
          return info;
        }()) {}

  Result<bool> run(pass::Context& context) override {
    il::Function& function = context.function();
    bool changed = false;
    std::vector<il::ExprId> discardedTargets;
    for (const il::BlockId blockId : function.blockHandles()) {
      const il::Block& block = function.block(blockId);
      if (block.ops.empty()) {
        continue;
      }
      const il::OpId terminatorId = block.ops.back();
      il::Op& terminator = function.op(terminatorId);
      if (terminator.code != il::OpCode::IndirectBranch ||
          function.targets(terminator).size() != 1) {
        continue;
      }
      const il::BlockId target = function.targets(terminator)[0];
      discardedTargets.push_back(function.operands(terminator)[0]);
      terminator.code = il::OpCode::Branch;
      function.setOperands(terminatorId, {});
      function.setTargets(terminatorId, std::vector<il::BlockId>{target});
      changed = true;
    }
    if (changed) {
      function.rebuildEdges();
      removeOrphanedLoads(function, discardedTargets);
    }
    return changed;
  }
};

}  // namespace

std::unique_ptr<pass::Pass> makeFoldResolvedBranchPass() {
  return std::make_unique<FoldResolvedBranch>();
}

}  // namespace xdec::passes
