// findDeadStackStores (see the header for the safety rules).
#include "xdec/analysis/stack_store_fold.h"

namespace xdec::analysis {

namespace {

/// Whether `index` is the operand slot both Load and Store treat as "the
/// address", the one use of a stack delta this analysis expects and does not
/// itself count as an escape.
[[nodiscard]] bool isOwnAddressOperand(il::OpCode code, std::size_t index) {
  return (code == il::OpCode::Load || code == il::OpCode::Store) && index == 0;
}

}  // namespace

std::unordered_set<uint32_t> findDeadStackStores(const il::Function& function,
                                                  const StackFrame& frame,
                                                  const VariableTable& variables) {
  std::unordered_set<int64_t> readDeltas;
  std::unordered_set<int64_t> escapedDeltas;

  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      const auto operands = function.operands(op);
      for (std::size_t index = 0; index < operands.size(); ++index) {
        const AddressInfo info = frame.classify(operands[index]);
        if (info.kind != AddressKind::StackSlot) {
          continue;
        }
        if (op.code == il::OpCode::Load && index == 0) {
          readDeltas.insert(info.delta);
          continue;
        }
        if (isOwnAddressOperand(op.code, index)) {
          continue;
        }
        // Anything else holding a stack address -- a call argument, a
        // stored value, a phi input -- may read through it somewhere this
        // analysis cannot follow.
        escapedDeltas.insert(info.delta);
      }
    }
  }

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
          escapedDeltas.contains(info.delta)) {
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
