// Block-local transforms, exposed as plain functions so tests drive them
// without the pass machinery. local-simplify composes them; P8's
// deobfuscation passes reuse them.
#pragma once

#include <unordered_map>

#include "xdec/il/function.h"

namespace xdec::passes {

/// Rewrites Value leaves through a substitution map. Replacement expressions
/// were themselves substituted at the time they were recorded, so the walk
/// does not recurse into them. Shared by every transform that replaces SSA
/// values with the expressions behind them.
class ValueSubst {
 public:
  void set(il::ValueId value, il::ExprId replacement) { map_[value] = replacement; }

  [[nodiscard]] il::ExprId apply(il::Function& function, il::ExprId id) {
    if (const auto found = memo_.find(id); found != memo_.end()) {
      return found->second;
    }
    // By value: rebuilding interns into the pool, which dangles references.
    const il::Expr expr = function.expr(id);
    il::ExprId result = id;
    if (expr.op == il::ExprOp::Value) {
      const il::ValueId value{static_cast<uint32_t>(expr.immediate)};
      if (const auto found = map_.find(value); found != map_.end()) {
        result = found->second;
      }
    } else if (expr.operandCount > 0) {
      il::Expr rebuilt = expr;
      bool changed = false;
      for (unsigned index = 0; index < expr.operandCount; ++index) {
        const il::ExprId next = apply(function, expr.operands[index]);
        changed |= next != expr.operands[index];
        rebuilt.operands[index] = next;
      }
      if (changed) {
        result = function.intern(rebuilt);
      }
    }
    memo_.emplace(id, result);
    return result;
  }

 private:
  std::unordered_map<il::ValueId, il::ExprId> map_;
  std::unordered_map<il::ExprId, il::ExprId> memo_;
};

/// Post-order constant fold of an expression tree. Fully-const subtrees are
/// replaced by a Const of the same type; anything the constant evaluator
/// declines is left structurally alone. Flags-typed results are never folded
/// (a flags Const is not representable), but their consumers are.
[[nodiscard]] il::ExprId foldConstants(il::Function& function, il::ExprId id);

/// Rewrites FlagCond-of-FlagDef patterns into plain integer compares, the
/// transformation that makes lazy flags pay: `subs a, b; b.eq` becomes
/// `cmp.eq a, b`, the flag bundle ceases to exist for that use, and the
/// structurer sees a real condition. Unrepresentable conditions (overflow
/// tests on anything but Logical, carry tests on the carry-in forms) are left
/// alone.
[[nodiscard]] il::ExprId foldFlagConditions(il::Function& function, il::ExprId id);

/// Whole-function drivers over the expression rewrites. Return whether any op
/// operand changed.
bool foldConstants(il::Function& function);
bool foldFlagConditions(il::Function& function);

/// Block-local register copy propagation with sub-register semantics: a read
/// of a register whose contents were written earlier in the block is replaced
/// by the written expression, with extracts/zero-extensions inserted where
/// the access width differs. Calls, intrinsics and unimplemented ops clobber
/// the tracked state; writes to zero-class registers are removed.
[[nodiscard]] bool copyPropagateBlock(il::Function& function, il::BlockId block);

/// Block-local dead code elimination: ReadReg ops whose value is never used,
/// and WriteReg ops overwritten again before any read of the register.
[[nodiscard]] bool dceBlock(il::Function& function, il::BlockId block);

}  // namespace xdec::passes
