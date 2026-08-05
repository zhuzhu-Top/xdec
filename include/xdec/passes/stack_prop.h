// The stack pass: canonical frame addressing and block-local memory
// forwarding.
//
// Two rewrites, one block-local walk, both standing on StackFrame's
// classification:
//
//   - Canonicalisation. Every address that bottoms out at `entry(sp)` plus a
//     constant becomes the two-operand normal form `add(entry(sp), delta)`.
//     The prologue's subtract, the frame-pointer copy, and every scaled
//     access then collapse onto one hash-consed expression per slot, which is
//     what lets later passes see that two accesses are the same slot by
//     comparing ExprIds.
//   - Forwarding. Memory the block already knows is not read twice: a load
//     from a slot an earlier store wrote is the stored expression, and a
//     reload of an untouched address — the dispatcher's constant context
//     fetches — is the first load's value (truncated when narrower,
//     little-endian low bits). A call, an intrinsic, or a store through an
//     unclassified address clears the tracked state: memory may move under
//     any of them, and honesty beats reach.
//
// What this pass deliberately does not do: dead-store elimination. A stack
// store is observable until escape analysis proves the slot cannot outlive
// the function, and that proof is the Vars phase's (P9), not this one's.
#pragma once

#include <memory>

#include "xdec/pass/pass.h"

namespace xdec::passes {

[[nodiscard]] std::unique_ptr<pass::Pass> makeStackPropPass();

}  // namespace xdec::passes
