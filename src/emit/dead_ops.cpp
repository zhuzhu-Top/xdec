#include "dead_ops.h"

#include <set>
#include <vector>

#include "xdec/il/expr_roots.h"

namespace xdec::emit {

namespace {

using il::addExprRoots;
using il::collectValueLeaves;

[[nodiscard]] bool isGotoLeaf(const Stmt* arm) {
  return arm != nullptr && (arm->kind == StmtKind::Goto || arm->kind == StmtKind::Continue);
}

[[nodiscard]] bool localMayBeReadBeforeRewrite(const il::Function& function,
                                               const analysis::StackFrame& frame,
                                               il::BlockId start, int64_t delta,
                                               unsigned budget) {
  std::set<il::BlockId> visited{start};
  std::vector<il::BlockId> queue{start};
  std::size_t explored = 0;
  while (!queue.empty()) {
    if (++explored > budget) {
      return true;
    }
    const il::BlockId current = queue.back();
    queue.pop_back();
    bool rewritten = false;
    for (const il::OpId opId : function.block(current).ops) {
      const il::Op& op = function.op(opId);
      const auto operands = function.operands(op);
      if (op.code == il::OpCode::Load && frame.classify(operands[0]).delta == delta) {
        return true;
      }
      if (op.code == il::OpCode::Store && frame.classify(operands[0]).delta == delta) {
        rewritten = true;
        break;
      }
    }
    if (rewritten) {
      continue;
    }
    for (const il::BlockId successor : function.block(current).successors) {
      if (visited.insert(successor).second) {
        queue.push_back(successor);
      }
    }
  }
  return false;
}

[[nodiscard]] bool namesErrnoLocation(const COptions& options, uint64_t va) {
  if (options.symbols) {
    if (const SymbolRef symbol = options.symbols(va); symbol.exact() && symbol.isFunction) {
      return symbol.name == "__errno_location";
    }
  }
  if (options.names) {
    const types::BoundName named = options.names(va);
    return !named.empty() && named.isFunction && named.name == "__errno_location";
  }
  return false;
}

[[nodiscard]] il::OpId foldableErrnoCall(const il::Function& function, il::BlockId block,
                                         const COptions& options) {
  if (!block.valid()) {
    return {};
  }
  const auto& ops = function.block(block).ops;
  for (const il::OpId opId : ops) {
    const il::Op& op = function.op(opId);
    if (op.code != il::OpCode::Store) {
      continue;
    }
    const std::optional<il::ValueId> value = importAccessorStoreValue(function, op);
    if (!value.has_value() || !function.hasValue(*value)) {
      continue;
    }
    const il::OpId definition = function.value(*value).definition;
    if (!definition.valid()) {
      continue;
    }
    const il::Op& call = function.op(definition);
    if (call.code != il::OpCode::Call || function.operands(call).size() != 1) {
      continue;
    }
    uint64_t address = 0;
    if (!function.asConstant(function.operands(call)[0], address) ||
        !namesErrnoLocation(options, address)) {
      continue;
    }
    std::set<uint32_t> reached;
    for (const il::BlockId otherBlock : function.blockHandles()) {
      for (const il::OpId other : function.block(otherBlock).ops) {
        if (other == opId || other == definition) {
          continue;
        }
        const il::Op& otherOp = function.op(other);
        if (otherOp.code == il::OpCode::Phi) {
          for (const il::ExprId edgeValue : function.operands(otherOp)) {
            collectValueLeaves(function, edgeValue, reached);
          }
          continue;
        }
        std::vector<il::ExprId> roots;
        addExprRoots(function, otherOp, roots);
        for (const il::ExprId root : roots) {
          collectValueLeaves(function, root, reached);
        }
      }
    }
    if (reached.contains(value->index())) {
      continue;
    }
    return definition;
  }
  return {};
}

void collectDeadOpsInto(const il::Function& function, const analysis::StackFrame& frame,
                        const analysis::VariableTable& variables, const Stmt& stmt,
                        const COptions& options, std::unordered_set<uint32_t>& dead) {
  switch (stmt.kind) {
    case StmtKind::Sequence:
      for (std::size_t index = 0; index < stmt.items.size(); ++index) {
        const Stmt& item = *stmt.items[index];
        if (item.kind == StmtKind::Block && index + 1 < stmt.items.size()) {
          const Stmt& next = *stmt.items[index + 1];
          if (next.kind == StmtKind::Switch && next.block == item.block && next.cond.valid()) {
            if (const il::OpId deadLoad =
                    deadJumpTableLoad(function, item.block, next.tableMode, next.cond);
                deadLoad.valid()) {
              dead.insert(deadLoad.index());
            }
            const bool enumerated = next.tableMode || !next.caseValues.empty();
            if (const il::OpId deadStore = deadStateDiscriminantStore(
                    function, frame, variables, enumerated, item.block, next.cond);
                deadStore.valid()) {
              dead.insert(deadStore.index());
            }
          } else if (next.kind == StmtKind::If && next.block == item.block && next.cond.valid()) {
            if (const il::OpId deadLoad =
                    deadJumpTableLoad(function, item.block, /*tableMode=*/true, next.cond);
                deadLoad.valid()) {
              dead.insert(deadLoad.index());
            }
            const bool nextArmsLeaveDirectly = isGotoLeaf(next.thenArm.get()) ||
                                               isGotoLeaf(next.elseArm.get()) || !next.thenArm ||
                                               !next.elseArm;
            if (const il::OpId deadStore = deadRoutingStateStore(
                    function, frame, variables, item.block, next.cond, nextArmsLeaveDirectly);
                deadStore.valid()) {
              dead.insert(deadStore.index());
            }
          }
        }
        collectDeadOpsInto(function, frame, variables, item, options, dead);
      }
      break;
    case StmtKind::Block:
      if (const il::OpId deadCall = foldableErrnoCall(function, stmt.block, options);
          deadCall.valid()) {
        dead.insert(deadCall.index());
      }
      break;
    case StmtKind::If:
      if (stmt.thenArm) {
        collectDeadOpsInto(function, frame, variables, *stmt.thenArm, options, dead);
      }
      if (stmt.elseArm) {
        collectDeadOpsInto(function, frame, variables, *stmt.elseArm, options, dead);
      }
      if (stmt.epilogue) {
        collectDeadOpsInto(function, frame, variables, *stmt.epilogue, options, dead);
      }
      break;
    case StmtKind::While:
    case StmtKind::DoWhile:
      if (stmt.body) {
        collectDeadOpsInto(function, frame, variables, *stmt.body, options, dead);
      }
      break;
    case StmtKind::Switch:
      for (const StmtPtr& body : stmt.caseBodies) {
        if (body) {
          collectDeadOpsInto(function, frame, variables, *body, options, dead);
        }
      }
      if (stmt.defaultBody) {
        collectDeadOpsInto(function, frame, variables, *stmt.defaultBody, options, dead);
      }
      if (stmt.epilogue) {
        collectDeadOpsInto(function, frame, variables, *stmt.epilogue, options, dead);
      }
      break;
    default:
      break;
  }
}

}  // namespace

il::OpId deadJumpTableLoad(const il::Function& function, il::BlockId block, bool tableMode,
                           il::ExprId cond) {
  if (!tableMode || !block.valid()) {
    return {};
  }
  const auto& ops = function.block(block).ops;
  if (ops.empty()) {
    return {};
  }
  const il::Op& terminator = function.op(ops.back());
  if (terminator.code != il::OpCode::IndirectBranch) {
    return {};
  }
  const auto termOperands = function.operands(terminator);
  if (termOperands.empty()) {
    return {};
  }
  const il::Expr& targetExpr = function.expr(termOperands[0]);
  if (targetExpr.op != il::ExprOp::Value) {
    return {};
  }
  const il::ValueId target{static_cast<uint32_t>(targetExpr.immediate)};

  il::OpId candidate{};
  for (const il::OpId opId : ops) {
    if (const il::Op& op = function.op(opId); op.code == il::OpCode::Load && op.result == target) {
      candidate = opId;
      break;
    }
  }
  if (!candidate.valid()) {
    return {};
  }

  std::set<uint32_t> reached;
  il::collectValueLeaves(function, cond, reached);
  if (reached.contains(target.index())) {
    return {};
  }
  for (const il::OpId opId : ops) {
    if (opId == candidate) {
      continue;
    }
    std::vector<il::ExprId> roots;
    addExprRoots(function, function.op(opId), roots);
    for (const il::ExprId root : roots) {
      il::collectValueLeaves(function, root, reached);
    }
    if (reached.contains(target.index())) {
      return {};
    }
  }
  return candidate;
}

il::OpId deadStateDiscriminantStore(const il::Function& function,
                                    const analysis::StackFrame& frame,
                                    const analysis::VariableTable& variables, bool enumerated,
                                    il::BlockId block, il::ExprId cond) {
  if (!enumerated || !block.valid() || !cond.valid()) {
    return {};
  }
  for (const il::OpId opId : function.block(block).ops) {
    const il::Op& op = function.op(opId);
    if (op.code != il::OpCode::Store) {
      continue;
    }
    const auto operands = function.operands(op);
    if (operands[1] != cond) {
      continue;
    }
    const analysis::AddressInfo address = frame.classify(operands[0]);
    if (address.kind != analysis::AddressKind::StackSlot) {
      continue;
    }
    const analysis::Variable* local = variables.localAt(address.delta);
    if (local == nullptr || local->type.pointerDepth > 0 || local->aliasBase.has_value() ||
        local->type.width != op.type.bits()) {
      continue;
    }
    return opId;
  }
  return {};
}

il::OpId deadRoutingStateStore(const il::Function& function, const analysis::StackFrame& frame,
                               const analysis::VariableTable& variables, il::BlockId block,
                               il::ExprId ifCond, bool ifArmsLeaveDirectly) {
  if (!ifArmsLeaveDirectly || !block.valid() || !ifCond.valid()) {
    return {};
  }
  constexpr unsigned kReachBudget = 32;
  const auto& ops = function.block(block).ops;
  for (std::size_t index = 0; index < ops.size(); ++index) {
    const il::Op& op = function.op(ops[index]);
    if (op.code != il::OpCode::Store) {
      continue;
    }
    const auto operands = function.operands(op);
    const il::Expr& value = function.expr(operands[1]);
    if (value.op != il::ExprOp::Select || value.operand(0) != ifCond ||
        function.expr(value.operand(1)).op != il::ExprOp::Const ||
        function.expr(value.operand(2)).op != il::ExprOp::Const) {
      continue;
    }
    const analysis::AddressInfo address = frame.classify(operands[0]);
    if (address.kind != analysis::AddressKind::StackSlot) {
      continue;
    }
    const analysis::Variable* local = variables.localAt(address.delta);
    if (local == nullptr || local->type.pointerDepth > 0 || local->aliasBase.has_value() ||
        local->type.width != op.type.bits()) {
      continue;
    }
    bool readLater = false;
    for (std::size_t later = index + 1; later < ops.size() && !readLater; ++later) {
      const il::Op& laterOp = function.op(ops[later]);
      readLater = laterOp.code == il::OpCode::Load &&
                  frame.classify(function.operands(laterOp)[0]).delta == address.delta;
    }
    if (readLater) {
      continue;
    }
    bool readDownstream = false;
    for (const il::BlockId successor : function.block(block).successors) {
      if (localMayBeReadBeforeRewrite(function, frame, successor, address.delta, kReachBudget)) {
        readDownstream = true;
        break;
      }
    }
    if (!readDownstream) {
      return ops[index];
    }
  }
  return {};
}

std::optional<il::ValueId> importAccessorStoreValue(const il::Function& function,
                                                    const il::Op& store) {
  const auto operands = function.operands(store);
  if (function.expr(operands[1]).op != il::ExprOp::Neg) {
    return std::nullopt;
  }
  const il::Expr& addressExpr = function.expr(operands[0]);
  if (addressExpr.op != il::ExprOp::Value) {
    return std::nullopt;
  }
  return il::ValueId{static_cast<uint32_t>(addressExpr.immediate)};
}

std::unordered_set<uint32_t> collectDeadOps(const il::Function& function,
                                            const analysis::StackFrame& frame,
                                            const analysis::VariableTable& variables,
                                            const Stmt& root, const COptions& options) {
  std::unordered_set<uint32_t> dead;
  collectDeadOpsInto(function, frame, variables, root, options, dead);
  return dead;
}

}  // namespace xdec::emit
