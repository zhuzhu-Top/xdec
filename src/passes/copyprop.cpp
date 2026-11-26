// Block-local register copy propagation (see transform.h).
#include "transform.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xdec::passes {

namespace {

/// Past this many distinct nodes, a tracked register's contents are not worth
/// tracking anymore: substituting it into the *next* write re-embeds and
/// re-interns the whole thing, so straight-line code that keeps combining
/// already-large values compounds the cost of every following op until the
/// block is unusable (the mega-block hang -- see eval/FINDINGS.md's
/// "mega-block local-simplify" note). Capping the tracked size bounds that
/// compounding; past the cap a write just clobbers, exactly like the
/// non-zero-extending partial write case below.
constexpr std::size_t kMaxTrackedExprNodes = 64;

/// Distinct-node count of the DAG rooted at `id`, stopping as soon as it is
/// clear the count exceeds `limit` -- callers only need to know "small enough
/// to keep chaining" vs. not, never the exact size once it is not.
[[nodiscard]] std::size_t boundedExprNodeCount(const il::Function& function, il::ExprId id,
                                               std::size_t limit) {
  if (function.expr(id).operandCount == 0) {
    return 1;
  }
  // Small fixed buffer on the stack for the standard kMaxTrackedExprNodes cap
  // avoids a heap unordered_set + vector allocation on every WriteReg.
  uint32_t visitedBuf[kMaxTrackedExprNodes + 1];
  std::size_t visitedCount = 0;
  std::vector<uint32_t> overflowVisited;
  std::vector<il::ExprId> stack;
  stack.reserve(16);
  stack.push_back(id);
  const auto alreadySeen = [&](uint32_t idx) -> bool {
    if (limit <= kMaxTrackedExprNodes) {
      for (std::size_t i = 0; i < visitedCount; ++i) {
        if (visitedBuf[i] == idx) {
          return true;
        }
      }
      visitedBuf[visitedCount++] = idx;
      return false;
    }
    if (std::find(overflowVisited.begin(), overflowVisited.end(), idx) !=
        overflowVisited.end()) {
      return true;
    }
    overflowVisited.push_back(idx);
    visitedCount = overflowVisited.size();
    return false;
  };
  while (!stack.empty() && visitedCount <= limit) {
    const il::ExprId current = stack.back();
    stack.pop_back();
    if (alreadySeen(current.index())) {
      continue;
    }
    const il::Expr& expr = function.expr(current);
    for (unsigned index = 0; index < expr.operandCount; ++index) {
      stack.push_back(expr.operands[index]);
    }
  }
  return visitedCount;
}

/// One block's propagation state: the expression each root register currently
/// holds, when known.
struct BlockProp {
  explicit BlockProp(il::Function& f) : function(f) {}

  il::Function& function;
  ValueSubst subst;
  /// Root register -> the expression it currently contains.
  std::unordered_map<il::RegId, il::ExprId> contents;
  /// Ops to remove after the walk (writes to zero-class registers).
  std::vector<il::OpId> removable;
  bool changed = false;

  /// The expression a read of `reg` denotes, when the register's contents are
  /// tracked: direct for a full register, an extract for a view.
  [[nodiscard]] il::ExprId readExpr(il::RegId reg) {
    const il::RegId root = function.registers().rootOf(reg);
    const auto found = contents.find(root);
    if (found == contents.end()) {
      return il::ExprId{};
    }
    const il::RegisterInfo& info = function.registers()[reg];
    if (!info.isSubRegister()) {
      return found->second;
    }
    return function.extract(info.type(), found->second, info.offsetInParent);
  }

  void write(il::RegId reg, il::ExprId value) {
    const il::RegisterInfo& info = function.registers()[reg];
    if (info.regClass == il::RegClass::Zero) {
      return;  // the write is discarded; the op itself goes away separately
    }
    const il::RegId root = function.registers().rootOf(reg);
    if (boundedExprNodeCount(function, value, kMaxTrackedExprNodes) > kMaxTrackedExprNodes) {
      contents.erase(root);
      return;
    }
    if (!info.isSubRegister()) {
      contents[root] = value;
      return;
    }
    if (info.zeroExtendsParent) {
      const il::Type rootType = function.registers()[root].type();
      contents[root] = function.cast(il::ExprOp::ZExt, rootType, value);
      return;
    }
    // A partial write without zero-extension semantics merges with the old
    // contents; nothing precise to track.
    contents.erase(root);
  }

  /// Everything not modelled clobbers every tracked register.
  void clobberAll() { contents.clear(); }
};

}  // namespace

bool copyPropagateBlock(il::Function& function, il::BlockId blockId) {
  BlockProp prop(function);
  il::Block& block = function.block(blockId);
  std::vector<il::ExprId> rewritten;

  for (const il::OpId opId : block.ops) {
    const il::Op& op = function.op(opId);

    // Operands see the substitutions recorded so far.
    if (!prop.subst.empty()) {
      const auto operands = function.operands(op);
      if (!operands.empty()) {
        rewritten.assign(operands.begin(), operands.end());
        bool opChanged = false;
        for (il::ExprId& operand : rewritten) {
          const il::ExprId next = prop.subst.apply(function, operand);
          opChanged |= next != operand;
          operand = next;
        }
        if (opChanged) {
          function.setOperands(opId, rewritten);
          prop.changed = true;
        }
      }
    }

    switch (op.code) {
      case il::OpCode::ReadReg: {
        const il::RegId reg = op.reg();
        const il::RegisterInfo& info = function.registers()[reg];
        if (info.regClass == il::RegClass::Zero) {
          prop.subst.set(op.result, function.constant(info.type(), 0));
          prop.changed = true;
          break;
        }
        if (const il::ExprId known = prop.readExpr(reg); known.valid()) {
          prop.subst.set(op.result, known);
          prop.changed = true;
        }
        break;
      }
      case il::OpCode::WriteReg: {
        const il::RegId reg = op.reg();
        if (function.registers()[reg].regClass == il::RegClass::Zero) {
          prop.removable.push_back(opId);
          prop.changed = true;
          break;
        }
        const auto after = function.operands(function.op(opId));
        prop.write(reg, after[0]);
        break;
      }
      case il::OpCode::Call:
      case il::OpCode::Intrinsic:
      case il::OpCode::Unimplemented:
        prop.clobberAll();
        break;
      default:
        break;
    }
  }

  for (const il::OpId opId : prop.removable) {
    function.removeOp(blockId, opId);
  }
  return prop.changed;
}

}  // namespace xdec::passes
