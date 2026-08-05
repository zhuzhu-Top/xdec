// fold-resolved-branch (see the header for the contract).
#include "xdec/passes/fold_resolved_branch.h"

#include "xdec/il/function.h"

namespace xdec::passes {

namespace {

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
      terminator.code = il::OpCode::Branch;
      function.setOperands(terminatorId, {});
      function.setTargets(terminatorId, std::vector<il::BlockId>{target});
      changed = true;
    }
    if (changed) {
      function.rebuildEdges();
    }
    return changed;
  }
};

}  // namespace

std::unique_ptr<pass::Pass> makeFoldResolvedBranchPass() {
  return std::make_unique<FoldResolvedBranch>();
}

}  // namespace xdec::passes
