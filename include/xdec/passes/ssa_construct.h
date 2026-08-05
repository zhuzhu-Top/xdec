// The Cfg -> Ssa pass: registers become SSA variables.
//
// Design stance, stated here because everything in the implementation
// follows from it:
//
//   - Roots, not views. SSA versions track root registers (x0, not w0); a
//     sub-register read becomes an extract, a sub-register write becomes the
//     merge the register file declares (zero-extension on AArch64). View
//     semantics live in exactly one place.
//   - Values replace register ops. A WriteReg disappears and the written
//     expression becomes the register's current version; a ReadReg
//     disappears and its uses see the current version. What remains is pure
//     data flow plus the operations that genuinely do something: memory,
//     calls, control flow, intrinsics.
//   - Semi-pruned placement. Phis are placed, per Cytron, for registers
//     defined in more than one block — no full liveness. The surplus is
//     small, and dead phis are the next phase's DCE fodder rather than this
//     pass's complexity.
//   - Calls clobber honestly. A call produces fresh unknown (undef) versions
//     of every tracked register except the stack pointer and zero-class
//     registers, which the target's discipline preserves. Registers the IL
//     cannot track (float/vector/special classes) keep their op form
//     untouched. An ABI-aware refinement is the P9 variable phase's job.
//   - Memory is not versioned here. Loads and stores stay as they are; the
//     alias and stack-model phase (P7f) introduces memory versioning with
//     real disambiguation behind it.
#pragma once

#include <memory>

#include "xdec/pass/pass.h"

namespace xdec::passes {

[[nodiscard]] std::unique_ptr<pass::Pass> makeSsaConstructPass();

}  // namespace xdec::passes
