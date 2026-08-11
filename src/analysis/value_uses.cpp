// FunctionValueUses (see the header for why this is function-wide).
#include "xdec/analysis/value_uses.h"

#include <set>

#include "xdec/il/expr_roots.h"

namespace xdec::analysis {

FunctionValueUses collectValueUses(const il::Function& function) {
  FunctionValueUses result;
  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      std::set<uint32_t> leaves;
      for (const il::ExprId operand : function.operands(op)) {
        il::collectValueLeaves(function, operand, leaves);
      }
      for (const uint32_t valueIndex : leaves) {
        result.sites[valueIndex].push_back(ValueUseSite{blockId, opId});
      }
    }
  }
  return result;
}

}  // namespace xdec::analysis
