// ExpressionReuseReport: measuring same-block subexpression duplication
// before it is fixed, so a fix can be judged by a number instead of a single
// eyeballed sample.
//
// The taxonomy (see docs/08-expression-reuse.md) has four shapes, but only
// two are decidable purely from the IL without re-deriving the structurizer's
// own decisions:
//
//   * Exact duplicate  -- the identical ExprId is reachable from both a
//     block's own straight-line ops and its terminator's discriminant (a
//     CondBranch condition, or a resolved IndirectBranch's jump-table index).
//     Emitting the block and the discriminant from two unrelated CSE scopes
//     (see emit/c_expr.h) prints it twice under two different names; sharing
//     one scope across the pair (see StmtPrinter::printBlock's `extraRoots`)
//     prints it once.
//   * Structural duplicate -- two distinct Load ops in the same block read
//     the same address with no Store/Call/Intrinsic between them to have
//     invalidated it. Nothing hash-conses two Loads (see il/expr.h's own
//     note on why a memory read is an Op, not an Expr), so this is a
//     candidate for value forwarding (Phase 2 of the expression-reuse plan),
//     never merged here.
//
// This never rewrites IL or changes what the emitter prints; it only counts,
// for `xdec decompile --reuse-report` and for judging a later phase's actual
// effect on a real sample instead of trusting that it should have helped.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "xdec/il/function.h"

namespace xdec::analysis {

enum class ReuseKind : uint8_t {
  ExactDuplicate,
  StructuralDuplicate,
};

struct ReuseFinding {
  ReuseKind kind;
  il::BlockId block;
  /// Exact: the one ExprId reachable from both units. Structural: the
  /// shared address both Loads read.
  il::ExprId shared;
  /// A short label for each side, e.g. "block ops" / "terminator
  /// condition", or a Load's own result temp -- for report output only.
  std::string firstUnit;
  std::string secondUnit;
};

struct ExpressionReuseReport {
  std::vector<ReuseFinding> findings;

  [[nodiscard]] std::size_t count(ReuseKind kind) const;
  /// One line per finding, its block's address and both units named.
  [[nodiscard]] std::string format(const il::Function& function) const;
};

/// Scans every block of `function` for the two duplicate shapes above.
/// Cross-arm duplication (an if's two branches, two switch cases) is
/// deliberately not reported: those units never both execute on the same
/// path, so recomputing between them costs nothing at runtime, and forcing
/// them to share a CSE scope would violate the not-yet-assigned-on-this-path
/// rule that scope split exists to uphold (see emit/c_expr.h).
[[nodiscard]] ExpressionReuseReport analyzeExpressionReuse(const il::Function& function);

}  // namespace xdec::analysis
