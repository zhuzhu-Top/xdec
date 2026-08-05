// The Ssa-level optimisation fixpoint: sparse conditional constant
// propagation, phi simplification, and global dead code elimination.
//
// Why the pass produces Ssa rather than Optimized: the maturity ladder gates
// Optimized behind Resolved, and Resolved belongs to the deobfuscation phase
// (P8) — the verifier rejects unresolved indirect branches at Resolved and
// above, so a VMP-flattened function physically cannot be Optimized yet. The
// machinery here is exactly what the deflattening pass will lean on, which is
// why it lands first: SCCP computes the dispatcher's state values that make
// indirect targets resolvable at all.
//
// What lives here:
//
//   - SCCP, Wegman-Zadeck over the SSA value graph. Lattice: Unknown (not yet
//     computed) < Const < Overdefined. `undef` expressions are Overdefined —
//     they model a genuinely unknown machine value, and folding one to a
//     constant would invent information. Phis meet over executable edges
//     only, so an edge a folded branch abandons stops poisoning the merge.
//   - Conditional branches with a constant condition become unconditional,
//     the abandoned successor's phi operands lose the edge's slot, and the
//     edge cache is rebuilt per fold. Folding a dispatcher test is how the
//     CFG sheds the flattening layer's arms one by one.
//   - Phi simplification: a phi with one predecessor, or whose distinct
//     non-self inputs are a single expression, is that expression. Undef-only
//     phis collapse the same way, which is what reclaims the registers a call
//     clobbered and nothing redefined.
//   - Global DCE: phis and loads whose value nothing reads, to a fixed
//     point. Loads go because a decompiler assumes memory reads do not trap;
//     intrinsics stay because their effects are opaque by definition.
#pragma once

#include <memory>

#include "xdec/pass/pass.h"

namespace xdec::passes {

[[nodiscard]] std::unique_ptr<pass::Pass> makeSsaOptimizePass();

}  // namespace xdec::passes
