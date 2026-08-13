// Block-local transforms, exposed as plain functions so tests drive them
// without the pass machinery. local-simplify composes them; P8's
// deobfuscation passes reuse them.
#pragma once

#include <unordered_map>

#include "xdec/il/function.h"

namespace xdec::passes {

/// Above this many ops, a block is a "mega-block": straight-line code (an
/// unrolled hash round, say) with no internal branch, dense enough that
/// copyPropagateBlock and forwardRedundantLoads stop paying for themselves.
/// Both walk substituted expression trees, and before copyprop capped how
/// large a tracked register value is allowed to grow (see
/// kMaxTrackedExprNodes in copyprop.cpp) that growth was unbounded and turned
/// a few thousand ops into minutes instead of milliseconds -- see
/// eval/FINDINGS.md's "mega-block local-simplify" note for the measured
/// curve. With the cap in place both transforms are linear in block size even
/// on a straight-line 5000+-op block (measured: <20ms/iteration), so this
/// threshold is now a fuse against a future regression rather than the
/// primary defense; local-simplify skips both past this size, and dceBlock
/// and the whole-function algebra/fold passes stay in scope regardless, since
/// neither ever showed the same blowup.
inline constexpr std::size_t kMegaBlockOpThreshold = 16384;

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
///
/// foldFlagConditions' whole-function form also distributes a FlagCond
/// through a flags-typed phi it merges through -- two blocks each setting
/// flags their own way before a shared test -- by synthesizing one boolean
/// phi per (flags phi, condition code) and rewriting each incoming edge
/// independently, so a real cross-block merge folds as far as its
/// individually-resolvable edges allow instead of degrading the whole test to
/// one opaque stub. The single-expression form does not: it has no function
/// to insert the synthesized phi into.
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

/// Block-local redundant load forwarding: a Load whose (already-substituted)
/// address repeats one already read since the last Store/Call/Intrinsic in
/// this block is replaced by that earlier Load's value and removed outright.
///
/// Safe where a generic "unused load" DCE is not (see dceBlock's own note on
/// why it keeps every load, dead or not): removing the second load never
/// changes whether the block can fault. The first load already proved the
/// address readable, and nothing between the two could have unmapped it, so
/// the second is guaranteed to see the same value and cannot introduce a
/// fault the first did not already risk. See docs/09-expression-reuse.md's
/// shape B for the duplicate this closes.
[[nodiscard]] bool forwardRedundantLoads(il::Function& function, il::BlockId block);

}  // namespace xdec::passes
