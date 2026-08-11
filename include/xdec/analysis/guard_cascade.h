// Recognising two guard conditions that share one fallback body.
//
// A diamond (structure.cpp's tryDiamond) closes when both of a CondBranch's
// arms walk cleanly to one shared post-dominator, each claiming its own
// blocks along the way. That breaks down the moment two *different*
// CondBranches want the very same block as part of their own arm: bc_lib's
// `sub_2f9a38` has an outer guard (`property_get(...) <= 0`) whose "bad"
// arm falls straight into a fallback block, and an inner guard
// (`strncmp(...) == 0`) nested under the outer guard's "good" arm, whose own
// "bad" arm falls into that *same* fallback block. Whichever CondBranch's
// diamond attempt runs first claims the fallback as its own arm; the second
// attempt then finds it already `emitted_` and has nowhere to put its own
// copy, so both diamonds fail and structure.cpp's `gotoChain` fallback
// prints two labelled `goto`s where nothing about the underlying control
// flow actually needs one.
//
// This is not a diamond with an unlucky claim order -- it is a different
// shape, one `tryDiamond` cannot represent at all: a fallback body with two
// predecessors is not what "each arm claims its own blocks" describes, no
// matter which arm goes first. GuardCascadeShape names that shape instead:
// two nested guards funnelling their failure paths into one shared body,
// which the emit layer's own `claimSharedFallbackBody` (structure.cpp) can
// then print exactly once.
#pragma once

#include <optional>

#include "xdec/analysis/dominators.h"
#include "xdec/il/function.h"

namespace xdec::analysis {

struct GuardCascadeShape {
  il::BlockId outerHead;
  il::BlockId innerHead;
  /// The body both guards' failure arms share.
  il::BlockId fallback;
  /// Where every path -- the inner guard's success arm, and the shared
  /// fallback's own single successor -- reconverges.
  il::BlockId merge;
  il::ExprId outerCond;
  il::ExprId innerCond;
  /// Whether `innerHead`'s arm that reaches `merge` directly is its "taken"
  /// (condition-true) arm rather than its "untaken" one. `outerHead`'s own
  /// polarity is not carried here: whoever already parsed `outerHead`'s
  /// CondBranch to find `head` in the first place has `outerHead`'s taken/
  /// untaken targets in hand already, and comparing either one against
  /// `innerHead` says the same thing this field would.
  bool innerSuccessIsTaken = false;
};

/// Looks for the shape rooted at `head`: `head` is a CondBranch whose two
/// targets split into a private inner guard and a fallback body, the inner
/// guard is itself a CondBranch whose two targets split into `ipdom(head)`
/// and that very same fallback body, and the fallback's own single successor
/// is that same merge point. Nullopt when any of that fails to line up --
/// including the ordinary case of a ready-made diamond, which this
/// deliberately leaves alone (see the header comment for why the two shapes
/// do not overlap: a fallback with only one predecessor is already a
/// diamond arm, not this).
[[nodiscard]] std::optional<GuardCascadeShape> matchGuardCascade(
    const il::Function& function, const PostDominators& postDominators, il::BlockId head);

}  // namespace xdec::analysis
