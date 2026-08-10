#include "xdec/analysis/expr_reuse.h"

#include <algorithm>
#include <format>
#include <unordered_map>
#include <unordered_set>

#include "xdec/analysis/jump_table.h"
#include "xdec/il/expr_roots.h"

namespace xdec::analysis {

namespace {

/// Every ExprId reachable from `roots`, `roots` themselves included.
void collectReachable(const il::Function& function, const std::vector<il::ExprId>& roots,
                      std::unordered_set<uint32_t>& out) {
  std::vector<il::ExprId> stack;
  for (const il::ExprId root : roots) {
    if (root.valid() && out.insert(root.index()).second) {
      stack.push_back(root);
    }
  }
  while (!stack.empty()) {
    const il::ExprId id = stack.back();
    stack.pop_back();
    const il::Expr& expr = function.expr(id);
    for (unsigned index = 0; index < expr.operandCount; ++index) {
      const il::ExprId child = expr.operands[index];
      if (out.insert(child.index()).second) {
        stack.push_back(child);
      }
    }
  }
}

/// Worth flagging: the same threshold the emitter uses to decide a node is
/// worth a name at all (see emit/c_expr.cpp's own isShared) -- a bare
/// constant or leaf duplicated twice costs nothing to read twice.
bool worthReporting(const il::Expr& expr) {
  return expr.operandCount > 0 && expr.type.isScalarInteger();
}

/// The block's terminator discriminant a real StmtPrinter would materialize
/// as its own CSE root: a CondBranch's condition, or a resolved
/// IndirectBranch's jump-table index. Anything else (an unresolved computed
/// branch, a plain Branch/Return) has none.
il::ExprId terminatorDiscriminant(const il::Function& function, const il::Op& terminator) {
  const auto operands = function.operands(terminator);
  if (terminator.code == il::OpCode::CondBranch) {
    return operands.empty() ? il::ExprId{} : operands[0];
  }
  if (terminator.code == il::OpCode::IndirectBranch && !operands.empty()) {
    if (const auto table = matchJumpTable(function, operands[0]); table.has_value()) {
      return table->index;
    }
  }
  return il::ExprId{};
}

void findExactDuplicates(const il::Function& function, il::BlockId blockId,
                         const il::Block& block, std::vector<ReuseFinding>& findings) {
  const il::Op* terminator = block.ops.empty() ? nullptr : &function.op(block.ops.back());
  if (terminator == nullptr) {
    return;
  }
  const il::ExprId discriminant = terminatorDiscriminant(function, *terminator);
  if (!discriminant.valid()) {
    return;
  }
  std::vector<il::ExprId> opRoots;
  for (const il::OpId opId : block.ops) {
    il::addExprRoots(function, function.op(opId), opRoots);
  }
  std::unordered_set<uint32_t> opsReach;
  collectReachable(function, opRoots, opsReach);
  std::unordered_set<uint32_t> termReach;
  collectReachable(function, {discriminant}, termReach);
  for (const uint32_t index : termReach) {
    if (opsReach.contains(index) && worthReporting(function.expr(il::ExprId{index}))) {
      findings.push_back({.kind = ReuseKind::ExactDuplicate,
                          .block = blockId,
                          .shared = il::ExprId{index},
                          .firstUnit = "block ops",
                          .secondUnit = "terminator condition"});
    }
  }
}

void findStructuralDuplicates(const il::Function& function, il::BlockId blockId,
                              const il::Block& block, std::vector<ReuseFinding>& findings) {
  // Address -> the Load op that most recently read it since the last
  // potential clobber. Cleared whenever a Store/Call/Intrinsic runs, since
  // any of those may write through an address this analysis cannot prove is
  // disjoint.
  std::unordered_map<uint32_t, il::OpId> lastLoadOf;
  for (const il::OpId opId : block.ops) {
    const il::Op& op = function.op(opId);
    switch (op.code) {
      case il::OpCode::Load: {
        const auto operands = function.operands(op);
        if (operands.empty()) {
          break;
        }
        const il::ExprId address = operands[0];
        if (const auto found = lastLoadOf.find(address.index()); found != lastLoadOf.end()) {
          const il::Op& earlier = function.op(found->second);
          findings.push_back(
              {.kind = ReuseKind::StructuralDuplicate,
              .block = blockId,
              .shared = address,
              .firstUnit = std::format("load at 0x{:x}", earlier.va),
              .secondUnit = std::format("load at 0x{:x}", op.va)});
        }
        lastLoadOf[address.index()] = opId;
        break;
      }
      case il::OpCode::Store:
      case il::OpCode::Call:
      case il::OpCode::Intrinsic:
        lastLoadOf.clear();
        break;
      default:
        break;
    }
  }
}

}  // namespace

std::size_t ExpressionReuseReport::count(ReuseKind kind) const {
  return static_cast<std::size_t>(
      std::count_if(findings.begin(), findings.end(),
                    [kind](const ReuseFinding& finding) { return finding.kind == kind; }));
}

std::string ExpressionReuseReport::format(const il::Function& function) const {
  std::string out;
  for (const ReuseFinding& finding : findings) {
    const uint64_t va = function.hasBlock(finding.block) ? function.block(finding.block).va : 0;
    out += std::format("{} @0x{:x}: expr{} shared between {} and {}\n",
                       finding.kind == ReuseKind::ExactDuplicate ? "exact-dup" : "structural-dup",
                       va, finding.shared.index(), finding.firstUnit, finding.secondUnit);
  }
  return out;
}

ExpressionReuseReport analyzeExpressionReuse(const il::Function& function) {
  ExpressionReuseReport report;
  for (const il::BlockId blockId : function.blockHandles()) {
    const il::Block& block = function.block(blockId);
    findExactDuplicates(function, blockId, block, report.findings);
    findStructuralDuplicates(function, blockId, block, report.findings);
  }
  return report;
}

}  // namespace xdec::analysis
