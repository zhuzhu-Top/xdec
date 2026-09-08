// findStackCanarySaves (see the header).
#include "xdec/analysis/stack_canary.h"

#include <map>
#include <optional>
#include <utility>

namespace xdec::analysis {

namespace {

/// The `Load` defining the value `exprId` names, when `exprId` is a bare
/// value reference and that value's whole definition is one `Load`. Nullopt
/// for a computed expression, an argument, or a value defined some other way
/// -- the same "stop at the first thing that is not this exact shape" rule
/// matchVtableCallTarget uses for its own call target.
[[nodiscard]] std::optional<il::OpId> loadDefining(const il::Function& function,
                                                    il::ExprId exprId) {
  const il::Expr& expr = function.expr(exprId);
  if (expr.op != il::ExprOp::Value) {
    return std::nullopt;
  }
  const il::ValueId value{static_cast<uint32_t>(expr.immediate)};
  if (!function.hasValue(value)) {
    return std::nullopt;
  }
  const il::OpId definition = function.value(value).definition;
  if (!function.hasOp(definition) || function.op(definition).code != il::OpCode::Load) {
    return std::nullopt;
  }
  return definition;
}

struct Save {
  il::OpId store;
  uint64_t guardAddress = 0;
};

/// Every `Store(stackSlot, Load(globalAddress))` in `function`, keyed by the
/// slot's delta. First writer at a given delta wins: a canary save happens
/// once, in the prologue, so a second store to the same delta (unrelated
/// reuse of the slot later in the function) is not this shape and is simply
/// never looked up by the second pass below.
[[nodiscard]] std::map<int64_t, Save> findGuardSaves(const il::Function& function,
                                                      const StackFrame& frame) {
  std::map<int64_t, Save> saves;
  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      if (op.code != il::OpCode::Store) {
        continue;
      }
      const auto operands = function.operands(op);
      if (operands.size() < 2) {
        continue;
      }
      const AddressInfo dst = frame.classify(operands[0]);
      if (dst.kind != AddressKind::StackSlot) {
        continue;
      }
      const std::optional<il::OpId> loadOp = loadDefining(function, operands[1]);
      if (!loadOp.has_value()) {
        continue;
      }
      const auto loadOperands = function.operands(function.op(*loadOp));
      if (loadOperands.empty()) {
        continue;
      }
      const AddressInfo src = frame.classify(loadOperands[0]);
      if (src.kind != AddressKind::Global) {
        continue;
      }
      saves.emplace(dst.delta, Save{opId, src.address});
    }
  }
  return saves;
}

}  // namespace

std::vector<StackCanarySave> findStackCanarySaves(const il::Function& function,
                                                  const StackFrame& frame) {
  const std::map<int64_t, Save> saves = findGuardSaves(function, frame);
  std::vector<StackCanarySave> result;
  if (saves.empty()) {
    return result;
  }

  for (const il::BlockId blockId : function.blockHandles()) {
    const il::Block& block = function.block(blockId);
    if (block.ops.empty()) {
      continue;
    }
    const il::OpId terminatorId = block.ops.back();
    const il::Op& terminator = function.op(terminatorId);
    if (terminator.code != il::OpCode::CondBranch) {
      continue;
    }
    const il::ExprId condition = function.operands(terminator)[0];
    const il::Expr& conditionExpr = function.expr(condition);
    if (conditionExpr.op != il::ExprOp::CmpEq && conditionExpr.op != il::ExprOp::CmpNe) {
      continue;
    }
    const il::ExprId lhs = conditionExpr.operand(0);
    const il::ExprId rhs = conditionExpr.operand(1);
    // Try both orderings: `guard != saved` and `saved != guard` are both
    // things a compiler (or xdec's own expression canonicalisation) might
    // print the comparison as.
    const std::pair<il::ExprId, il::ExprId> orderings[] = {{lhs, rhs}, {rhs, lhs}};
    for (const auto& [reload, savedSlot] : orderings) {
      const std::optional<il::OpId> reloadLoad = loadDefining(function, reload);
      if (!reloadLoad.has_value()) {
        continue;
      }
      const auto reloadOperands = function.operands(function.op(*reloadLoad));
      if (reloadOperands.empty()) {
        continue;
      }
      const AddressInfo reloadAddress = frame.classify(reloadOperands[0]);
      if (reloadAddress.kind != AddressKind::Global) {
        continue;
      }
      const std::optional<il::OpId> savedLoad = loadDefining(function, savedSlot);
      if (!savedLoad.has_value()) {
        continue;
      }
      const auto savedOperands = function.operands(function.op(*savedLoad));
      if (savedOperands.empty()) {
        continue;
      }
      const AddressInfo savedAddress = frame.classify(savedOperands[0]);
      if (savedAddress.kind != AddressKind::StackSlot) {
        continue;
      }
      const auto found = saves.find(savedAddress.delta);
      if (found == saves.end() || found->second.guardAddress != reloadAddress.address) {
        continue;
      }
      result.push_back(StackCanarySave{found->second.store, terminatorId,
                                       found->second.guardAddress, savedAddress.delta});
      break;  // this branch matched one ordering; the other cannot also match
    }
  }
  return result;
}

}  // namespace xdec::analysis
