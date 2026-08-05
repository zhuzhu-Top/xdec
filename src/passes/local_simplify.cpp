// makeLocalSimplifyPass: the block-local cleanup fixpoint (see the header).
#include "xdec/passes/local_simplify.h"

#include "algebra.h"
#include "transform.h"
#include "xdec/il/function.h"

namespace xdec::passes {

namespace {

class LocalSimplify final : public pass::FunctionPass {
 public:
  LocalSimplify()
      : FunctionPass([] {
          pass::PassInfo info;
          info.name = "local-simplify";
          info.level = il::Maturity::Lifted;
          info.produces = il::Maturity::Local;
          info.fixpoint = true;
          return info;
        }()) {}

  Result<bool> run(pass::Context& context) override {
    il::Function& function = context.function();
    // Algebra before folding: the rules strip filler the constant evaluator
    // cannot see through (and(x,0) over an unknown x, say), and folding then
    // collapses what the rules exposed. Both run again next iteration.
    bool changed = simplifyAlgebra(function);
    changed |= foldConstants(function);
    changed |= foldFlagConditions(function);
    for (const il::BlockId blockId : function.blockHandles()) {
      changed |= copyPropagateBlock(function, blockId);
      changed |= dceBlock(function, blockId);
    }
    return changed;
  }
};

}  // namespace

std::unique_ptr<pass::Pass> makeLocalSimplifyPass() {
  return std::make_unique<LocalSimplify>();
}

}  // namespace xdec::passes
