#include "phi_lowering.h"

#include <algorithm>
#include <set>

#include "xdec/analysis/dispatcher_shape.h"
#include "xdec/il/expr_roots.h"

namespace xdec::emit {

bool EdgePlan::needsSnapshot(const EdgeCopy& copy) const noexcept {
  return std::find(snapshots.begin(), snapshots.end(), &copy) != snapshots.end();
}

std::vector<EdgeCopy> collectEdgeCopies(const CContext& ctx, il::BlockId from, il::BlockId to) {
  std::vector<EdgeCopy> copies;
  if (!from.valid() || !to.valid()) {
    return copies;
  }
  const il::Block& target = ctx.function.block(to);
  const auto& predecessors = target.predecessors;
  const auto slot = std::find(predecessors.begin(), predecessors.end(), from);
  if (slot == predecessors.end()) {
    return copies;
  }
  const auto index = static_cast<std::size_t>(slot - predecessors.begin());
  for (const il::OpId opId : target.ops) {
    const il::Op& op = ctx.function.op(opId);
    if (op.code != il::OpCode::Phi) {
      break;
    }
    if (!op.result.valid()) {
      continue;
    }
    const analysis::Variable* temp = ctx.variables.tempFor(op.result);
    const auto operands = ctx.function.operands(op);
    if (temp == nullptr || index >= operands.size()) {
      continue;
    }
    const il::Expr& source = ctx.function.expr(operands[index]);
    if (source.op == il::ExprOp::Value) {
      const il::ValueId srcVal{static_cast<uint32_t>(source.immediate)};
      if (srcVal == op.result || ctx.variables.tempFor(srcVal) == temp) {
        continue;
      }
    }
    copies.push_back(EdgeCopy{op.result, temp->name, temp->type.format(), operands[index]});
  }
  return copies;
}

EdgePlan buildEdgePlan(const CContext& ctx, il::BlockId from, il::BlockId to,
                       const ActiveFrame* activeFrame) {
  EdgePlan plan;
  plan.copies = collectEdgeCopies(ctx, from, to);
  if (plan.copies.empty()) {
    return plan;
  }

  if (activeFrame != nullptr && to == activeFrame->merge) {
    const analysis::DispatcherShape shape{il::BlockId{}, activeFrame->merge, il::BlockId{}};
    const analysis::HandlerFrameExit exit =
        analysis::classifyHandlerExit(ctx.function, from, shape, activeFrame->frame);
    if (exit.kind != analysis::HandlerExitKind::Return) {
      const auto isSuppressed = [&](const EdgeCopy& copy) {
        for (std::size_t slot = 0; slot < activeFrame->frame.slots.size(); ++slot) {
          if (!exit.unchanged[slot]) {
            continue;
          }
          const il::OpId shadowPhi = activeFrame->frame.slots[slot].shadowPhiAtMerge;
          if (copy.destination == ctx.function.op(shadowPhi).result) {
            return true;
          }
        }
        return false;
      };
      plan.copies.erase(std::remove_if(plan.copies.begin(), plan.copies.end(), isSuppressed),
                        plan.copies.end());
      if (plan.copies.empty()) {
        return plan;
      }
    }
  }

  if (activeFrame != nullptr && from == activeFrame->merge) {
    const auto isIdentityRestore = [&](const EdgeCopy& copy) {
      for (std::size_t slot = 0; slot < activeFrame->unanimous.size(); ++slot) {
        if (!activeFrame->unanimous[slot]) {
          continue;
        }
        const il::OpId livePhi = activeFrame->frame.slots[slot].livePhiAtHub;
        if (copy.destination == ctx.function.op(livePhi).result) {
          return true;
        }
      }
      return false;
    };
    plan.copies.erase(std::remove_if(plan.copies.begin(), plan.copies.end(), isIdentityRestore),
                      plan.copies.end());
    if (plan.copies.empty()) {
      return plan;
    }
  }

  std::vector<std::set<uint32_t>> leavesOf(plan.copies.size());
  for (std::size_t index = 0; index < plan.copies.size(); ++index) {
    il::collectValueLeaves(ctx.function, plan.copies[index].source, leavesOf[index]);
  }
  for (std::size_t index = 0; index < plan.copies.size(); ++index) {
    const analysis::Variable* dstVar = ctx.variables.tempFor(plan.copies[index].destination);
    for (std::size_t other = 0; other < plan.copies.size(); ++other) {
      if (other == index) {
        continue;
      }
      bool readsDst = leavesOf[other].contains(plan.copies[index].destination.index());
      if (!readsDst && dstVar != nullptr) {
        for (const uint32_t leafIdx : leavesOf[other]) {
          if (ctx.variables.tempFor(il::ValueId{leafIdx}) == dstVar) {
            readsDst = true;
            break;
          }
        }
      }
      if (readsDst) {
        plan.snapshots.push_back(&plan.copies[index]);
        break;
      }
    }
  }
  plan.scoped = !plan.snapshots.empty();
  return plan;
}

il::BlockId firstBlockOf(const Stmt* stmt) {
  if (stmt == nullptr) {
    return il::BlockId{};
  }
  switch (stmt->kind) {
    case StmtKind::Block:
    case StmtKind::Goto:
    case StmtKind::Switch:
      return stmt->block;
    case StmtKind::While:
    case StmtKind::DoWhile:
      return stmt->block.valid() ? stmt->block : firstBlockOf(stmt->body.get());
    case StmtKind::Sequence:
      for (const StmtPtr& item : stmt->items) {
        if (const il::BlockId found = firstBlockOf(item.get()); found.valid()) {
          return found;
        }
      }
      return il::BlockId{};
    case StmtKind::If:
    case StmtKind::Continue:
    case StmtKind::Break:
      return il::BlockId{};
  }
  return il::BlockId{};
}

const il::Op* terminatorOf(const il::Function& function, il::BlockId block) {
  if (!block.valid()) {
    return nullptr;
  }
  const auto& ops = function.block(block).ops;
  if (ops.empty()) {
    return nullptr;
  }
  const il::Op& last = function.op(ops.back());
  return last.isTerminator() ? &last : nullptr;
}

}  // namespace xdec::emit
