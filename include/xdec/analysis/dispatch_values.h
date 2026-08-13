// Recovering the real values behind a resolved table-mode dispatch.
//
// Once analysis::matchJumpTable finds the table and resolve_indirect narrows
// it to a handful of concrete targets, the IL still knows exactly which
// index value reaches each one -- resolve_indirect had to compute that to
// resolve the branch at all (see resolve_indirect.cpp's sorted
// `preciseIndices`). By the time a target list reaches emission, though,
// that correspondence is gone: each case prints as `case 0`, `case 1`, ...
// in discovery order, which says nothing about the dispatcher's actual
// state values.
//
// This is a second, independent way to recover the same correspondence,
// purely from the index expression's own constant/select structure -- no
// image access needed, because for the overwhelmingly common OLLVM shape
// (an opaque predicate's obfuscated `if` compiled down to `state = cond ? A
// : B` and then dispatched through the shared table) the index is built
// entirely out of constants and selects, with no load in it at all. When it
// enumerates to exactly as many values as there are targets, the two agree
// by construction (both are asking the same question about the same
// expression), and the sort order lines the values back up with the
// targets exactly like resolve_indirect's own ordering does.
//
// For the two-target case specifically, this also recovers *which* select
// in the index's tree is the original boolean the obfuscator branched on --
// letting a two-case "table switch" be read for what it actually is: an
// ordinary `if`/`else` that happens to reach its arms through a jump table.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "xdec/il/function.h"

namespace xdec::analysis {

/// `values[i]` is the index value that reaches the caller's `targets[i]`,
/// ascending -- the same order `resolve_indirect.cpp` sorts its own
/// candidates in, so a caller holding a resolved `IndirectBranch`'s target
/// list can zip the two directly.
struct DispatchValues {
  std::vector<uint64_t> values;
  /// Valid only when `values.size() == 2` and one `Select` node inside
  /// `index`'s own expression tree splits exactly into the two of them: the
  /// boolean a reader would recognise as the original branch condition,
  /// rather than the clamp/select formula wrapping it. Invalid (default)
  /// otherwise -- the values are still meaningful, there is simply no single
  /// boolean that names the split.
  il::ExprId condition{};
  /// Whether `condition` true is the branch that reaches `values[0]` (and so
  /// the caller's `targets[0]`); false means `values[1]`/`targets[1]`.
  /// Meaningless when `condition` is invalid.
  bool conditionTrueIsFirst = false;
};

/// Symbolically enumerates every constant `index` can take -- from its own
/// constant/select structure alone, evaluating any condition that is itself
/// decidable so a select wrapping an unreachable arm does not inflate the
/// count (see analysis::ImageEval, reused here with a reader that never
/// maps anything, so a load anywhere in the tree degrades to "top" instead
/// of a wrong answer) -- and pairs the result with `targetCount` targets in
/// ascending order. Nullopt when `index` is not fully enumerable this way
/// (a load, an unresolved register, top) or the count does not match: either
/// way this is not wrong, just not reconstructable from the IL alone, and
/// the caller's existing per-target handling stays the honest fallback.
[[nodiscard]] std::optional<DispatchValues> matchDispatchValues(const il::Function& function,
                                                                 il::ExprId index,
                                                                 std::size_t targetCount);

}  // namespace xdec::analysis
