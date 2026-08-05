// Jump table recognition: the shapes computed branches take when they are
// really switches, extracted from the branch's target expression.
//
// Two families cover the obfuscated ARM64 samples seen so far:
//
//   - Pointer tables. `brind load(base + index*stride)` — entries are
//     absolute code addresses (the c66app form, including the two-table
//     select variant, whose arms each match).
//   - Offset tables. `brind anchor + sext(load32(base + index*stride))` —
//     entries are signed offsets from a pc-relative anchor (the ammana
//     dispatcher form), including the packed small-entry variant
//     `anchor + (zext(loadW) << k)` for 8/16-bit entries.
//
// Recognition is purely structural: the expression is what it is after the
// simplifier ran, and the index is deliberately NOT analysed. A dispatcher
// whose index is data-dependent still gets enumerated — the whole point of
// the table is that every entry is a valid target, so the index's value is
// the obfuscator's problem, not ours.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include "xdec/il/function.h"

namespace xdec::analysis {

/// Resolves a subexpression to the single value it can take, or nothing when it
/// can take more than one.
///
/// Supplied by the caller because a table's *shape* is structural but its base
/// and anchor are *values*. A dispatcher that computes its table address once
/// and reaches the branch from forty blocks has a base that is one constant
/// without being spelled as one — it arrives as a phi — and the flattened
/// dispatchers this exists to resolve are exactly that. Insisting on a literal
/// there is a fact about how the arithmetic happened to be written, not about
/// what a table is.
///
/// Only ever consulted for a base or an anchor. The index is still never
/// analysed (see below), and no resolver is required: without one, matching is
/// literal-only, which is where this started.
using ConstantResolver = std::function<std::optional<uint64_t>(il::ExprId)>;

struct JumpTable {
  /// Where entry zero lives in the image.
  uint64_t base = 0;
  /// Bytes between consecutive entries (the index scale; equals the entry
  /// width when the table is contiguous).
  uint32_t stride = 0;
  /// Bits per entry read: 64 for pointer tables, 8/16/32 for offset tables.
  uint32_t entryBits = 64;
  /// True when entries are offsets from `anchor` (offset tables); false when
  /// entries are the targets themselves (pointer tables).
  bool relative = false;
  /// Offset tables only: what entries are relative to.
  uint64_t anchor = 0;
  /// Offset tables only: entries are sign-extended from entryBits.
  bool signedOffsets = false;
  /// Offset tables only: the shift applied to the (extended) entry —
  /// `anchor + (entry << offsetShift)`. Small-entry tables pack branch
  /// distances this way (`(u16 << 2)` spans ±128 KiB of state blocks).
  uint32_t offsetShift = 0;
  /// The table index expression, when the address arithmetic carried one
  /// (absent for a bare base constant). Emission uses it as the switch
  /// selector; resolution deliberately never looks at it.
  il::ExprId index{};
};

/// Matches `target` (an IndirectBranch's operand) against the table families
/// above. The match is exact: a near-miss is not a table, it is just an
/// expression, and the branch stays unresolved rather than wrong.
[[nodiscard]] std::optional<JumpTable> matchJumpTable(const il::Function& function,
                                                      il::ExprId target,
                                                      const ConstantResolver& resolve = {});

}  // namespace xdec::analysis
