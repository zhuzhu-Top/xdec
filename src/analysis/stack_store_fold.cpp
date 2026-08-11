// findDeadStackStores (see the header for the safety rules).
#include "xdec/analysis/stack_store_fold.h"

#include "xdec/analysis/stack_escape.h"

namespace xdec::analysis {

std::unordered_set<uint32_t> findDeadStackStores(const il::Function& function,
                                                  const StackFrame& frame,
                                                  const VariableTable& variables) {
  std::unordered_set<int64_t> readDeltas;
  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      if (op.code != il::OpCode::Load) {
        continue;
      }
      const auto operands = function.operands(op);
      if (operands.empty()) {
        continue;
      }
      const AddressInfo info = frame.classify(operands[0]);
      if (info.kind == AddressKind::StackSlot) {
        readDeltas.insert(info.delta);
      }
    }
  }
  const StackEscapeMap escapes = StackEscapeMap::compute(function, frame);

  std::unordered_set<uint32_t> dead;
  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      if (op.code != il::OpCode::Store) {
        continue;
      }
      const auto operands = function.operands(op);
      if (operands.empty()) {
        continue;
      }
      const AddressInfo info = frame.classify(operands[0]);
      if (info.kind != AddressKind::StackSlot || readDeltas.contains(info.delta) ||
          escapes.isEscaped(info.delta)) {
        continue;
      }
      const Variable* local = variables.localAt(info.delta);
      if (local != nullptr && local->aliasBase.has_value()) {
        continue;
      }
      // The one slot VariableTable::recover promotes to "state" is kept
      // store-only on purpose: a flattening dispatcher's real state value
      // often lives in a register/phi between the spill and its use, with
      // this slot never read back at all (see that promotion's own note on
      // why a read requirement was tried and dropped). Folding its store
      // away would erase the one thing the promotion exists to show a
      // reader -- which stack slot is the dispatcher's own state -- so this
      // analysis leaves it alone even though nothing else here reads it.
      if (local != nullptr && local->name == "state") {
        continue;
      }
      dead.insert(opId.index());
    }
  }
  return dead;
}

}  // namespace xdec::analysis
