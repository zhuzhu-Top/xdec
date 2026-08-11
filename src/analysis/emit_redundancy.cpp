// analyzeEmitRedundancy (see the header for what this counts and why).
#include "xdec/analysis/emit_redundancy.h"

#include <format>
#include <unordered_set>

#include "xdec/analysis/stack_load_fold.h"
#include "xdec/analysis/stack_store_fold.h"

namespace xdec::analysis {

std::string EmitRedundancyReport::format() const {
  return std::format(
      "stack loads: {}/{} folded, stack stores: {}/{} dead, {} write-only local(s)",
      stackLoadsFolded, stackLoads, stackStoresDead, stackStores, writeOnlyLocals);
}

EmitRedundancyReport analyzeEmitRedundancy(const il::Function& function, const StackFrame& frame,
                                           const VariableTable& variables) {
  EmitRedundancyReport report;

  std::unordered_set<int64_t> readDeltas;
  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      const auto operands = function.operands(op);
      if (op.code == il::OpCode::Load) {
        const AddressInfo info = frame.classify(operands[0]);
        if (info.kind == AddressKind::StackSlot) {
          ++report.stackLoads;
          readDeltas.insert(info.delta);
        }
      } else if (op.code == il::OpCode::Store) {
        report.stackStores += frame.classify(operands[0]).kind == AddressKind::StackSlot;
      }
    }
  }

  report.stackLoadsFolded = findFoldableStackLoads(function, frame, {}).size();
  report.stackStoresDead = findDeadStackStores(function, frame, variables).size();

  for (const analysis::Variable& local : variables.locals()) {
    if (!readDeltas.contains(local.stackDelta)) {
      ++report.writeOnlyLocals;
    }
  }
  return report;
}

}  // namespace xdec::analysis
