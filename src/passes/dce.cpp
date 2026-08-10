// Block-local dead code elimination (see transform.h).
#include "transform.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xdec::passes {

namespace {

/// `visitedExprs` memoizes on the expression's own identity, not just the
/// Value leaves it bottoms out at: exprs are a DAG (algebra shares subtrees
/// across many parents), so without this a compound node reachable from N
/// operands gets walked from scratch N times -- combinatorial, not merely
/// redundant, on an MBA-heavy straight-line block (see eval/FINDINGS.md's
/// "mega-block local-simplify" note). Marking `id` visited before recursing
/// is safe because exprs are acyclic: the first visit always completes its
/// subtree before this can be asked about the same id again.
void collectValueUses(il::Function& function, il::ExprId id,
                      std::unordered_set<uint32_t>& uses,
                      std::unordered_set<uint32_t>& visitedExprs) {
  if (!visitedExprs.insert(id.index()).second) {
    return;
  }
  const il::Expr& expr = function.expr(id);
  if (expr.op == il::ExprOp::Value) {
    uses.insert(static_cast<uint32_t>(expr.immediate));
    return;
  }
  for (unsigned index = 0; index < expr.operandCount; ++index) {
    collectValueUses(function, expr.operands[index], uses, visitedExprs);
  }
}

}  // namespace

bool dceBlock(il::Function& function, il::BlockId blockId) {
  const il::Block& block = function.block(blockId);

  // Value uses across the whole block. Below SSA values are block-local, so a
  // value unused inside its block is unused, period.
  std::unordered_set<uint32_t> uses;
  std::unordered_set<uint32_t> visitedExprs;
  for (const il::OpId opId : block.ops) {
    for (const il::ExprId operand : function.operands(function.op(opId))) {
      collectValueUses(function, operand, uses, visitedExprs);
    }
  }

  std::vector<il::OpId> removable;
  /// Root register -> a WriteReg not read since; overwritten again it is dead.
  std::unordered_map<il::RegId, il::OpId> pendingWrites;

  for (const il::OpId opId : block.ops) {
    const il::Op& op = function.op(opId);

    // A snapshot with an unused result and no side effect is dead. Loads are
    // deliberately kept: whether a dead load is removable depends on fault
    // behaviour, which is the alias phase's question, not this sweep's.
    if (op.code == il::OpCode::ReadReg && !uses.contains(op.result.index())) {
      removable.push_back(opId);
      continue;
    }

    if (il::touchesRegister(op.code)) {
      const il::RegId root = function.registers().rootOf(op.reg());
      if (op.code == il::OpCode::ReadReg) {
        // The register is observed, so the pending write is not dead.
        pendingWrites.erase(root);
      } else {  // WriteReg
        if (const auto found = pendingWrites.find(root); found != pendingWrites.end()) {
          removable.push_back(found->second);
        }
        pendingWrites[root] = opId;
      }
      continue;
    }

    // Anything that can touch unknown state makes every pending write live.
    if (op.code == il::OpCode::Call || op.code == il::OpCode::Intrinsic ||
        op.code == il::OpCode::Unimplemented) {
      pendingWrites.clear();
    }
  }

  for (const il::OpId opId : removable) {
    function.removeOp(blockId, opId);
  }
  return !removable.empty();
}

}  // namespace xdec::passes
