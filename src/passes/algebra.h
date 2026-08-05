// Rule-based algebraic simplification: the identities that turn obfuscated
// arithmetic back into arithmetic.
//
// The rules come in two tiers, both semantics-exact and both proven by the
// test harness that binds their free variables to random constants and runs
// the shared constant evaluator over both sides:
//
//   - Local rules, in algebra.cpp. `x & 0`, `x + 0`, double negation, no-op
//     casts, constant reassociation (`(x + k1) + k2`), `~(-x) → x - 1`. Each
//     is a property of one node and its immediate operands. Obfuscators bury
//     meaning under these; honest compilers emit them too, so the tier belongs
//     in the pipeline independent of any profile.
//   - Idioms, in algebra_idioms.cpp. Shapes spanning several nodes: the MBA
//     forms (`x^y + 2·(x&y) → x + y` and kin), the sign-extension closed form
//     AArch64's sbfm semantics expand to, a rotate masked down to one of its
//     halves. Each needs a paragraph of its own to justify, which is why they
//     are not interleaved with the one-liners above.
//
// Both tiers run in one bottom-up walk, because they feed each other: the local
// rules establish the commutative-normal shape the idioms match against, and an
// idiom match leaves arithmetic the local rules then fold.
//
// What is NOT here: rules that are only sometimes true, speculative
// transforms, or anything the evaluator cannot check. An unproven rule is a
// wrong binary with extra confidence.
#pragma once

#include "xdec/il/function.h"

namespace xdec::passes {

/// Post-order simplification of one expression tree. Memoized; safe to call
/// from any pass at any maturity.
[[nodiscard]] il::ExprId simplifyAlgebra(il::Function& function, il::ExprId id);

/// Whole-function driver over op operands. Returns whether anything changed.
bool simplifyAlgebra(il::Function& function);

}  // namespace xdec::passes
