// PathInterpreter (see the header).
#include "xdec/analysis/path_interpreter.h"

#include <optional>

namespace xdec::analysis {

namespace {

/// The position of `cameFrom` in `block`'s predecessor list, which is also a
/// Phi's operand index for the value flowing in along that edge (see
/// op.h: "One operand per predecessor, in predecessor order").
[[nodiscard]] std::optional<std::size_t> predecessorIndex(const il::Block& block,
                                                          il::BlockId cameFrom) {
  for (std::size_t index = 0; index < block.predecessors.size(); ++index) {
    if (block.predecessors[index] == cameFrom) {
      return index;
    }
  }
  return std::nullopt;
}

}  // namespace

PathOutcome PathInterpreter::stepBlock(il::BlockId blockId, il::BlockId cameFrom,
                                       PathState& state) const {
  const il::Block& block = function_->block(blockId);
  const std::optional<std::size_t> predIndex = predecessorIndex(block, cameFrom);

  for (const il::OpId opId : block.ops) {
    const il::Op& op = function_->op(opId);
    switch (op.code) {
      case il::OpCode::Phi: {
        const auto operands = function_->operands(op);
        if (predIndex.has_value() && *predIndex < operands.size()) {
          state.bindPhi(op.result, operands[*predIndex]);
        } else {
          // Predecessor bookkeeping does not (yet) know this edge -- see
          // path_explorer.cpp's construction-time snapshot for when this can
          // happen mid-pass. Top, not a guess.
          state.bindPhiUnknown(op.result);
        }
        break;
      }
      case il::OpCode::Load: {
        const auto operands = function_->operands(op);
        state.bindLoad(op.result, op.type, operands[0]);
        break;
      }
      case il::OpCode::Store: {
        const auto operands = function_->operands(op);
        state.recordStore(operands[0], op.type, operands[1]);
        break;
      }
      case il::OpCode::Branch: {
        const auto targets = function_->targets(op);
        PathOutcome outcome;
        outcome.stop = PathStop::Branch;
        outcome.va = op.va;
        outcome.op = opId;
        outcome.target = targets[0];
        return outcome;
      }
      case il::OpCode::CondBranch: {
        const auto operands = function_->operands(op);
        const auto targets = function_->targets(op);
        PathOutcome outcome;
        outcome.stop = PathStop::CondBranch;
        outcome.va = op.va;
        outcome.op = opId;
        outcome.condition = operands[0];
        outcome.ifTrue = targets[0];
        outcome.ifFalse = targets[1];
        return outcome;
      }
      case il::OpCode::IndirectBranch: {
        const auto operands = function_->operands(op);
        const auto targets = function_->targets(op);
        PathOutcome outcome;
        outcome.stop = PathStop::IndirectBranch;
        outcome.va = op.va;
        outcome.op = opId;
        outcome.resolvedTargets = targets;
        if (targets.empty() && !operands.empty()) {
          outcome.indirectTarget = operands[0];
        }
        return outcome;
      }
      case il::OpCode::Return:
      case il::OpCode::Unreachable:
      case il::OpCode::Unimplemented: {
        PathOutcome outcome;
        outcome.stop = op.code == il::OpCode::Return       ? PathStop::Return
                       : op.code == il::OpCode::Unreachable ? PathStop::Unreachable
                                                            : PathStop::Stopped;
        outcome.va = op.va;
        outcome.op = opId;
        return outcome;
      }
      // Call, Intrinsic, Nop, ReadReg, WriteReg: side effects or values this
      // evaluator does not model (see the header). Not a terminator, so
      // execution simply continues to the next op -- a Call's own effect on
      // memory this path is tracking is not modelled either, which is the
      // same "do not guess past a call" default PathState's callers accept
      // (see docs/23-path-eval.md).
      default:
        break;
    }
  }
  // A verified block always ends in a terminator; reaching here means the
  // block was empty, which the verifier already rejects upstream.
  return PathOutcome{};
}

}  // namespace xdec::analysis
