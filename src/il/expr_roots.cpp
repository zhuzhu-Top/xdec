#include "xdec/il/expr_roots.h"

#include <vector>

namespace xdec::il {

void addExprRoots(const Function& function, const Op& op, std::vector<ExprId>& roots) {
  const auto operands = function.operands(op);
  switch (op.code) {
    case OpCode::Load:
      if (!operands.empty()) {
        roots.push_back(operands[0]);
      }
      break;
    case OpCode::Store:
    case OpCode::Call:
    case OpCode::Intrinsic:
      roots.insert(roots.end(), operands.begin(), operands.end());
      break;
    case OpCode::WriteReg:
    case OpCode::Return:
      if (!operands.empty()) {
        roots.push_back(operands[0]);
      }
      break;
    default:
      break;
  }
}

void collectValueLeaves(const Function& function, ExprId root, std::set<uint32_t>& out) {
  std::vector<ExprId> stack{root};
  std::set<uint32_t> seen;
  while (!stack.empty()) {
    const ExprId id = stack.back();
    stack.pop_back();
    if (!seen.insert(id.index()).second) {
      continue;
    }
    const Expr& expr = function.expr(id);
    if (expr.op == ExprOp::Value) {
      out.insert(static_cast<uint32_t>(expr.immediate));
      continue;
    }
    for (unsigned index = 0; index < expr.operandCount; ++index) {
      stack.push_back(expr.operands[index]);
    }
  }
}

}  // namespace xdec::il
