// The Cfg-level trampoline-elimination pass.
//
// A flattened dispatcher's block layout is address-ordered, not flow-ordered:
// a state's "success" and "failure" exits often land on separate labels that
// both immediately fall into the same next state, so the lifter hands the CFG
// plenty of blocks that do nothing but jump on (one op, an unconditional
// branch, no value or side effect of their own). Left alone, every edge that
// happens to land on one of these prints an extra label and an extra goto hop
// for zero information: `goto trampoline; ... trampoline: goto real;` reads
// exactly as `goto real;` would, just longer.
//
// This pass retargets every real edge in the function straight at the first
// non-trampoline block a chain of these forwards to, then lets rebuildEdges
// drop the trampolines' own predecessors to nothing (still valid IL — an
// unreachable block is a warning, not an error — just no longer anyone's way
// through). It runs at Cfg, strictly before ssa-construct: eliding a block
// that carries no value and no phi is a pure retarget at this level, with
// none of the phi-operand bookkeeping the same idea would need one maturity
// later.
#pragma once

#include <memory>

#include "xdec/pass/pass.h"

namespace xdec::passes {

[[nodiscard]] std::unique_ptr<pass::Pass> makeTrampolineFoldPass();

}  // namespace xdec::passes
