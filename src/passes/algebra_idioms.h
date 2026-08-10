// The idiom tier of the simplifier: rewrites that recognise a *shape* spanning
// several nodes, rather than a local property of one node.
//
// Split from algebra.cpp along that line deliberately. A local rule ("and with
// zero is zero") is read and checked in isolation; an idiom ("this or of two
// masked halves is a sign extension") is only correct as a whole, and each one
// needs a paragraph explaining which instruction or obfuscator emits it. Mixing
// the two made the rule table hard to read in exactly the place where being
// able to read it matters most.
//
// Every function here is a matcher: it returns the rewritten expression, or an
// invalid ExprId when the shape does not hold. None of them recurse — the
// caller's bottom-up walk has already simplified the children — and none of
// them guess. Each is proven by the same random-binding oracle as the local
// rules (tests/passes/test_algebra.cpp).
#pragma once

#include "xdec/il/function.h"

namespace xdec::passes {

/// `(sext(bit s of x) & ~mask) | (x & mask)` where mask covers bits 0..s, the
/// closed form AArch64's sbfm semantics produce for every sxtb/sxth/sxtw:
/// rewritten to `sext(trunc(x))`, which is what the instruction was called in
/// the first place. `orExpr` is the Or node.
[[nodiscard]] il::ExprId matchSignExtend(il::Function& function, const il::Expr& orExpr);

/// A rotate whose result is masked down to just one of its two halves is a
/// shift: `rotr(v, k) & M` with M clear below bit `w - k` keeps only the bits
/// `v << (w - k)` contributed. `andExpr` is the And node. Obfuscators reach for
/// rotates because a rotate has no C spelling and forces a helper call in the
/// output; half of them are shifts wearing a hat.
[[nodiscard]] il::ExprId matchMaskedRotate(il::Function& function, const il::Expr& andExpr);

/// A mask that covers every bit a shift could have produced does nothing:
/// `(v << k) & M` is `v << k` when M has all of bits k..w set. The masked-rotate
/// rewrite above leaves exactly this shape behind, and so does ordinary
/// bitfield code.
[[nodiscard]] il::ExprId matchMaskedShift(il::Function& function, const il::Expr& andExpr);

/// `(x ^ y) + 2·(x & y)` and `(x | y) + (x & y)` are both `x + y`, and
/// `(x & y) + (x ^ y)` is `x | y`. `addExpr` is the Add node; the operand order
/// and the spelling of the doubling (`mul 2` or `shl 1`) vary between
/// obfuscators, so all of them are matched.
[[nodiscard]] il::ExprId matchMbaAdd(il::Function& function, const il::Expr& addExpr);

/// `(x | y) - (x & y)` is `x ^ y`, and `2·(x | y) - (x ^ y)` is `x + y`.
/// `subExpr` is the Sub node.
[[nodiscard]] il::ExprId matchMbaSub(il::Function& function, const il::Expr& subExpr);

/// `(x & y) | (x ^ y)` is `x | y`. `orExpr` is the Or node.
[[nodiscard]] il::ExprId matchMbaOr(il::Function& function, const il::Expr& orExpr);

/// `(x | y) ^ (x & y)` is `x ^ y`. `xorExpr` is the Xor node.
[[nodiscard]] il::ExprId matchMbaXor(il::Function& function, const il::Expr& xorExpr);

/// A comparison of two values that were both shifted up by the same amount is
/// the comparison of the bits that survived: `(x << 32) < (y << 32)` is
/// `(int32)x < (int32)y`, and the same with a constant on the right, whose low
/// bits must then be clear for the shift to be undoable. True for every one of
/// the six integer comparisons and for both signednesses, because `v ↦ v·2^k`
/// is strictly monotone on the `w-k` bits it keeps.
///
/// This is the general form of the check a caller performs on a syscall's
/// return value. `r` is an error if it lands in `[-4095, -1]`, so the test is
/// against `0xfffff001`; obfuscated code does that test in the *high* half, as
/// `(magic - state) << 32` against `0xfffff00100000000`, which reads as a
/// 64-bit comparison against an unrecognisable constant until the shift comes
/// off both sides. `cmpExpr` is the comparison node.
[[nodiscard]] il::ExprId matchShiftedCompare(il::Function& function,
                                             const il::Expr& cmpExpr);

/// The `cmn C; cset hi` errno idiom, once `fold.cpp`'s lazy-flag folding has
/// already turned it into pure comparisons: `(a+C <u a) & (a+C != 0)` is
/// AArch64's `UnsignedGreater` condition over an `Add` flag bundle
/// (`rewriteAdd`'s own case), spelling "the unsigned add of `a` and `C`
/// overflows, and does not land exactly on zero" — which is true exactly when
/// `a` (unsigned) is strictly greater than `-C`. Folds to that one comparison.
/// `andExpr` is the And node; `C` must be a nonzero constant so `-C` is a
/// meaningful bound (a zero addend can never overflow, and the rule does not
/// need to fire there — the And already simplifies away on its own).
[[nodiscard]] il::ExprId matchCarryCompare(il::Function& function, const il::Expr& andExpr);

/// The `ls` (UnsignedLessEqual) mirror of `matchCarryCompare`: `(a <=u a+C) |
/// (a+C == 0)` is the negated overflow test, folding to `a <=u -C` instead.
/// `orExpr` is the Or node.
[[nodiscard]] il::ExprId matchCarryCompareOr(il::Function& function, const il::Expr& orExpr);

/// `(a + (k1 - x)) == (k2 - x)` is `a == (k2 - k1)`, and the same for `!=`:
/// subtracting the identical `x` from both sides of an equality cancels it,
/// whatever `x` is. An obfuscator's opaque predicate leans on exactly this to
/// hide a fixed comparison behind a shared in-flight value -- a loop counter,
/// a decrypted dispatcher state -- with `k1` and `k2` picked to look unrelated
/// on their own. Not sound for an ordered comparison: which way `a <u b`
/// points can change once the same `x` is subtracted from both, because the
/// two sides wrap around zero independently. `cmpExpr` is the CmpEq/CmpNe
/// node.
[[nodiscard]] il::ExprId matchCancelledSubtrahend(il::Function& function,
                                                  const il::Expr& cmpExpr);

}  // namespace xdec::passes
