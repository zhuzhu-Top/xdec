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
#include <vector>

#include "xdec/analysis/dominators.h"
#include "xdec/il/function.h"

namespace xdec::analysis {

/// The largest value `index` can hold at the end of `dispatch`, when a guard
/// dominating that block proves one. Inclusive: a returned 0x90 means the table
/// has 0x91 entries.
[[nodiscard]] std::optional<uint64_t> boundOnIndex(const il::Function& function,
                                                   const Dominators& dominators,
                                                   il::BlockId dispatch, il::ExprId index);

/// Every value `index` can take, where its own structure pins them down to a
/// handful even though what it is computed *from* is unknown.
///
/// `boundOnIndex` answers "how far up does this index reach", which is the
/// right question about a table's length and the wrong one about its *live*
/// entries. An index spelled `(~x >> 31) | 0xa` never exceeds 0xb, so a
/// caller enumerating `0..bound` reads twelve entries -- but the OR puts
/// bits 1 and 3 in every value the expression can produce, so ten of those
/// twelve are entries this branch cannot select. On a table one whole binary
/// shares (absd's `0x100080ec0`: one blob of relative offsets covering every
/// flattened function in the image, with no marker between one function's
/// entries and the next) those ten belong to other functions, and claiming
/// them makes their blocks this branch's successors.
///
/// This walk therefore carries values rather than a ceiling, and it answers
/// where ImageEval (analysis/image_eval.h) cannot: that evaluator concedes
/// top as soon as a leaf is unknown, which is right for a question about
/// concrete values and useless for this one, because the shapes that matter
/// narrow their operand whatever it happens to be. `shr.u:i32(x, 31)` is 0
/// or 1 for every x there is, known or not, and ORing that onto a literal is
/// two values, not two billion.
///
/// Nullopt where nothing is proved, and never a partial answer: a returned
/// set is *every* value the index can take, so a caller may read exactly
/// those entries and treat anything else as evidence it misread the table.
[[nodiscard]] std::optional<std::vector<uint64_t>> preciseIndexSet(const il::Function& function,
                                                                   il::ExprId index);

}  // namespace xdec::analysis
