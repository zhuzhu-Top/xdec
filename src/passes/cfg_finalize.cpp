// makeCfgFinalizePass: audits and seals the CFG contract (see the header).
#include "xdec/passes/cfg_finalize.h"

#include <format>
#include <vector>

#include "xdec/il/function.h"

namespace xdec::passes {

namespace {

class CfgFinalize final : public pass::FunctionPass {
 public:
  CfgFinalize()
      : FunctionPass([] {
          pass::PassInfo info;
          info.name = "cfg-finalize";
          info.level = il::Maturity::Local;
          info.produces = il::Maturity::Cfg;
          info.invalidates = {"cfg", "dominators", "scc"};
          return info;
        }()) {}

  Result<bool> run(pass::Context& context) override {
    il::Function& function = context.function();

    // Audit first: an unterminated non-stub block is the lifter's contract
    // broken, and edge repair below would hide that behind a fresh cache.
    for (const il::BlockId blockId : function.blockHandles()) {
      const il::Block& block = function.block(blockId);
      if (block.external) {
        if (!block.empty()) {
          return err(DiagCode::VerifyFailure,
                     std::format("external stub b{} @0x{:x} has content; stubs stand for "
                                 "code outside the function",
                                 blockId.index(), block.va));
        }
        continue;
      }
      const il::OpId last = block.terminator();
      if (!last.valid() || !function.op(last).isTerminator()) {
        return err(DiagCode::VerifyFailure,
                   std::format("block b{} @0x{:x} has no terminator at cfg maturity",
                               blockId.index(), block.va));
      }
    }

    // Repair: the edge cache is derivable state, so refreshing it is always
    // safe. Report honestly whether anything actually moved.
    std::vector<std::pair<std::vector<il::BlockId>, std::vector<il::BlockId>>> before;
    before.reserve(function.blockCount());
    for (const il::BlockId blockId : function.blockHandles()) {
      const il::Block& block = function.block(blockId);
      before.emplace_back(block.successors, block.predecessors);
    }
    function.rebuildEdges();
    bool changed = false;
    for (const il::BlockId blockId : function.blockHandles()) {
      const il::Block& block = function.block(blockId);
      const auto& [oldSucc, oldPred] = before[blockId.asSize()];
      if (block.successors != oldSucc || block.predecessors != oldPred) {
        changed = true;
        break;
      }
    }
    return changed;
  }
};

}  // namespace

std::unique_ptr<pass::Pass> makeCfgFinalizePass() {
  return std::make_unique<CfgFinalize>();
}

}  // namespace xdec::passes
