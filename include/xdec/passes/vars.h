// The Vars-level pass: how many arguments does this call actually pass?
//
// SSA construction attaches all eight integer argument registers to every call,
// because at that point it has no way to know better — it is renaming registers,
// not reading a calling convention (see ssa_construct.h, which says as much and
// names this phase as the place the question belongs). The result is a call with
// eight operands whatever the callee's arity, and until now the emitter papered
// over it: trim the trailing unknowns, print `/* + unknown arg(s) */`, and leave
// the reader to guess. Sixty-eight of those in one function is not a caveat, it
// is wallpaper.
//
// The recovery this pass does is caller-side, and rests on one property of every
// calling convention worth the name: **the caller materialises exactly the
// arguments it passes.** So for each argument register, at the call, ask what
// put the value there.
//
//   - A value this function computed, or the caller's own incoming value: the
//     register was set up. It is an argument.
//   - `Undef`: nothing on any path here defines it. In practice this is a
//     previous call's clobber — the ABI says a callee may leave x0..x7 holding
//     anything, and SSA records that honestly. No caller deliberately passes a
//     value it never wrote, so this slot is not an argument.
//
// The recovered arity is then the last set-up slot, and the operand list is
// trimmed to it in the IL. Doing it here rather than at emission is what makes
// it checkable: the arity becomes a property of the function that the verifier
// sees, the text form prints, and a test can assert on, instead of a decision
// re-made inside a printer.
//
// One case gets a note rather than a trim: a *gap*, where a later slot is set up
// and an earlier one is not. Positions are fixed by the convention, so the gap
// cannot be closed — and it should not be, because it means the caller set up
// argument three without setting up argument one. Either the code really does
// pass a stale register, or this analysis lost a definition. Both are worth
// saying out loud at the call site.
//
// What this pass cannot do is ask the callee. Arity is the callee's property;
// everything above is evidence from one side of the call. Where the evidence
// runs out the pass keeps the operand rather than inventing an answer.
#pragma once

#include <memory>

#include "xdec/pass/pass.h"

namespace xdec::passes {

[[nodiscard]] std::unique_ptr<pass::Pass> makeVarsPass();

}  // namespace xdec::passes
