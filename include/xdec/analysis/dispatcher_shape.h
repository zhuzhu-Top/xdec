// Recognising a flattening dispatcher's shared tail.
//
// A resolved jump-table switch built from an OLLVM-flattened function usually
// has one thing every ordinary switch does not: most of its cases do not
// leave the function or the loop directly. Each computes its own state
// transition and live-register handoff, then falls into the very same block
// as every other case, which restores those registers and jumps back to the
// dispatch loop's header. That shared block is not really part of any one
// case — it is the switch's own epilogue, printed once instead of duplicated
// (or, worse, left as a label 150+ cases all `goto`) — see structure.cpp's
// `claimDispatcherCaseBody` and `switchFor`.
#pragma once

#include <optional>
#include <span>

#include "xdec/il/function.h"

namespace xdec::analysis {

/// `dispatch` is the resolved `IndirectBranch` block the switch was built
/// from; `merge` is the shared tail most of its targets fall through to;
/// `hub` is `merge`'s own unconditional successor, the loop header the whole
/// state machine jumps back to.
struct DispatcherShape {
  il::BlockId dispatch;
  il::BlockId merge;
  il::BlockId hub;
};

/// Looks for the shape among `dispatch`'s own switch targets. A target counts
/// as a vote for a candidate tail when `dispatch` is its only predecessor
/// (so it is a private handler, not a block other code also reaches) and it
/// ends in one plain unconditional jump (so it is not itself a return, or a
/// branch of its own). The candidate with the most votes has to clear a high
/// bar — most of `targets`, not just a bare majority of the ones that voted
/// at all — and its own single successor becomes `hub`. Nullopt when nothing
/// clears that bar, including the ordinary case of a switch with no shared
/// tail at all.
[[nodiscard]] std::optional<DispatcherShape> matchDispatcherShape(
    const il::Function& function, il::BlockId dispatch,
    std::span<const il::BlockId> targets);

}  // namespace xdec::analysis
