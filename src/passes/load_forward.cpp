// Block-local redundant load forwarding (see transform.h).
#include "transform.h"

#include <unordered_map>
#include <vector>

namespace xdec::passes {

bool forwardRedundantLoads(il::Function& function, il::BlockId blockId) {
  ValueSubst subst;
  // Address ExprId index -> the Load result still known to hold it, since
  // the last op that could have clobbered it.
  std::unordered_map<uint32_t, il::ValueId> lastLoadOf;
  std::vector<il::OpId> removable;
  bool changed = false;

  const il::Block& block = function.block(blockId);
  for (const il::OpId opId : block.ops) {
    const il::Op& op = function.op(opId);

    // Operands see the substitutions recorded so far, same as copyprop: a
    // forwarded load's address may itself be built from an earlier forward.
    const auto operands = function.operands(op);
    std::vector<il::ExprId> rewritten(operands.begin(), operands.end());
    if (!rewritten.empty()) {
      bool opChanged = false;
      for (il::ExprId& operand : rewritten) {
        const il::ExprId next = subst.apply(function, operand);
        opChanged |= next != operand;
        operand = next;
      }
      if (opChanged) {
        function.setOperands(opId, rewritten);
        changed = true;
      }
    }

    switch (op.code) {
      case il::OpCode::Load: {
        if (rewritten.empty()) {
          break;
        }
        const il::ExprId address = rewritten[0];
        if (const auto found = lastLoadOf.find(address.index());
            found != lastLoadOf.end() && function.value(found->second).type == op.type) {
          subst.set(op.result, function.valueRef(found->second));
          removable.push_back(opId);
          changed = true;
          break;  // keep pointing at the original definition, not this one
        }
        lastLoadOf[address.index()] = op.result;
        break;
      }
      case il::OpCode::Store:
      case il::OpCode::Call:
      case il::OpCode::Intrinsic:
        // Any of these may write through an address this analysis cannot
        // prove disjoint from a tracked one.
        lastLoadOf.clear();
        break;
      default:
        break;
    }
  }

  for (const il::OpId opId : removable) {
    function.removeOp(blockId, opId);
  }
  return changed;
}

}  // namespace xdec::passes
