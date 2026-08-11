// Where a value is read across a whole function -- the fact that decides
// between "print this once, name it, and reference the name" and "fold what
// would be the only reference straight into the read."
//
// Function-wide rather than block-local (contrast dce.cpp's own per-block
// `collectValueUses`, which only needs to know what a block's own ops still
// reach) because a value's live range is not bounded by its own block: an
// ordinary SSA use, or a phi's incoming operand at a successor's head, can
// both reach it from somewhere a block-local scan never looks. Getting this
// wrong in the unsafe direction -- missing a use -- would make a later pass
// fold away a read something still needs, so every op's raw operands are
// walked, phi included.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "xdec/il/function.h"

namespace xdec::analysis {

/// One op that reads a value: `op`, in `block`, has the value somewhere in
/// its operand expression tree. Doesn't say how many times within that one
/// op's tree -- see FunctionValueUses's own note on why that distinction
/// does not matter here.
struct ValueUseSite {
  il::BlockId block;
  il::OpId op;
};

/// `ValueId.index()` -> every distinct op in the function that reads it. A
/// value read twice within the same op's expression tree (`x + x`) still
/// contributes one site here: what a fold decides on is "how many print
/// sites would need this value's own name," and both occurrences are the
/// same site. Two sites can end up sharing one printed occurrence between
/// them -- a subexpression a downstream CSE pass materializes once and both
/// ops merely reference by name -- but this map has no way to see that; it
/// is deliberately the full, possibly-redundant set, for a caller to narrow
/// with its own reasoning (see stack_load_fold.cpp's same-block
/// requirement, which is exactly that narrowing).
struct FunctionValueUses {
  std::unordered_map<uint32_t, std::vector<ValueUseSite>> sites;
};

/// Walks every op's operands (a Phi's incoming values included) and records
/// which values they read.
[[nodiscard]] FunctionValueUses collectValueUses(const il::Function& function);

}  // namespace xdec::analysis
