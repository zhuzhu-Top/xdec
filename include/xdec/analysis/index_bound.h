// How many entries a jump table has, proved from the code that guards it.
//
// Enumerating a table needs its length, and the table itself does not carry one:
// past its last entry come more bytes, which read as offsets just as well as the
// real entries do. Scanning until an entry "looks wrong" therefore does not find
// the end — it finds the first entry unlucky enough to fail a plausibility test,
// which may be the fortieth or may be the four hundredth. In the samples this
// was written against it was neither: a 145-entry table and a 1351-entry table
// both scanned past 512 entries without a single implausible one, so the bound
// never fired at all and enumeration produced hundreds of blocks of decoded
// data.
//
// The length is written down, though, just not in the table. A dispatch on an
// unbounded index would be a jump to wherever the following data pointed, so
// real code checks the index first:
//
//     ldr  w8, [x19, #0x2c]     ; the state
//     subs w9, w8, #0x90        ; compare it with the last valid state
//     b.hi default              ; out of range: not a table entry at all
//     adr  x9, table
//     ldrh w11, [x9, x8, lsl #1]
//     ...
//     br   x10
//
// That `b.hi` states the table's length exactly: on the edge that reaches the
// dispatch, the index is at most 0x90, so the table has 0x91 entries. This
// module reads that statement back off the IL — a comparison against the index,
// on a block that dominates the dispatch, whose relevant edge is the one the
// dispatch is on.
//
// It is a proof, not an estimate. Either a guard is found and its bound holds on
// every path that reaches the branch, or nothing is returned and the caller is
// left knowing it does not know.
#pragma once

#include <cstdint>
#include <optional>

#include "xdec/analysis/dominators.h"
#include "xdec/il/function.h"

namespace xdec::analysis {

/// The largest value `index` can hold at the end of `dispatch`, when a guard
/// dominating that block proves one. Inclusive: a returned 0x90 means the table
/// has 0x91 entries.
[[nodiscard]] std::optional<uint64_t> boundOnIndex(const il::Function& function,
                                                   const Dominators& dominators,
                                                   il::BlockId dispatch, il::ExprId index);

}  // namespace xdec::analysis
