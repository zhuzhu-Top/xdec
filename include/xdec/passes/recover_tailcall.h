// The Ssa-level pass that tells a tail call apart from a computed jump.
//
// AArch64 spells both with `br xN`: a switch dispatching through a jump table
// and a function returning `f(a, b)` produce the same instruction, and the
// lifter cannot tell them apart because nothing at the instruction says which
// it is. So it lifts an indirect branch with no successors, and something has
// to decide, or `resolve-indirect` fails the whole decompilation over a branch
// that was never going to land inside this function.
//
// The decision here rests on where the destination comes from, not on what the
// surrounding code looks like:
//
//   * A jump table lives in this image. Its base is an address the compiler
//     wrote down -- a constant in the instruction stream -- and only the index
//     is data. So an expression that mentions any address in this image is a
//     computed jump, and this pass leaves it alone whatever else it mentions.
//
//   * A tail call's destination is a pointer somebody else owns: an argument
//     register's entry value, possibly loaded through (`ops[i](a, b)` loads from
//     an array the caller passed). Nothing in this image says where it points,
//     so no table can be enumerated and no successor exists to find. Control
//     leaves the function; the only question left is what it was called with.
//
//   * A PLT thunk's `br x17` reads a slot the loader binds to another module.
//     The loader's own account of that slot -- an import name and no address --
//     is the evidence, and it is conclusive: a slot that is bound at load time
//     cannot be a jump table in this image.
//
// What the rewrite produces is a call followed by a return, which is what a
// tail call is. The arguments come from the snapshot SSA construction records
// on an unresolved indirect branch (see ssa_construct.cpp): by the time this
// pass can tell what the branch is, the instructions that set up the arguments
// are dead and folded away, so the values have to be captured earlier, in the
// one place that still knows the register versions.
//
// Running before `resolve-call` is what makes an imported tail call print as a
// name, and before `apply-types` is what lets an imported prototype trim the
// arguments the ABI snapshot over-approximates.
//
// A branch this declines to rewrite is left exactly as it was: unresolved, for
// `resolve-indirect` to resolve or to fail on. Declining is the right answer
// whenever the evidence is not conclusive, because rewriting a dispatcher into
// a call to itself would be a confident lie about control flow, and those are
// far more expensive to read past than a reported gap.
#pragma once

#include <memory>

#include "xdec/pass/pass.h"

namespace xdec::passes {

[[nodiscard]] std::unique_ptr<pass::Pass> makeRecoverTailCallPass();

}  // namespace xdec::passes
