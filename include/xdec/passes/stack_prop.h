// The stack pass: canonical frame addressing and memory forwarding.
//
// Three rewrites, both block-local and whole-function, standing on
// StackFrame's classification:
//
//   - Canonicalisation. Every address that bottoms out at `entry(sp)` plus a
//     constant becomes the two-operand normal form `add(entry(sp), delta)`.
//     The prologue's subtract, the frame-pointer copy, and every scaled
//     access then collapse onto one hash-consed expression per slot, which is
//     what lets later passes see that two accesses are the same slot by
//     comparing ExprIds.
//   - Block-local forwarding. Memory the block already knows is not read
//     twice: a load from a slot an earlier store wrote is the stored
//     expression, and a reload of an untouched address — the dispatcher's
//     constant context fetches — is the first load's value (truncated when
//     narrower, little-endian low bits). A call, an intrinsic, or a store
//     through an unclassified address clears the tracked state: memory may
//     move under any of them, and honesty beats reach.
//   - Whole-function global reuse. The block-local reset above cannot see a
//     global reloaded from a different block. When nothing anywhere in the
//     function stores to the address, and nothing anywhere is a call, an
//     intrinsic, or a store through an unclassified address (the same
//     clobber rule as above, just checked function-wide instead of forgotten
//     per block), every load of it denotes the same value for the function's
//     whole lifetime — a caller-established constant the decompiler cannot
//     name, but can still stop reloading. That is what lets a later algebra
//     idiom see two occurrences of the same operand instead of two different
//     loads of it.
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
