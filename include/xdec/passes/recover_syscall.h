// The Ssa-level pass that turns `svc` from an event into a call.
//
// After the lifter, an `svc #0` is an intrinsic reading x8 and x0..x5 and
// defining x0 (see specs/arm64/system.xspec, which explains why the ABI is
// named there rather than recovered here). That is faithful but not yet
// useful: the reader sees seven register values going into an opaque box.
//
// This pass answers the one question that unlocks the rest — *which* syscall —
// and then acts on the answer:
//
//   1. **Read the number.** x8 is operand one. Running after `ssa-optimize`
//      means constant and copy propagation have already chased it through
//      register moves, zero-extensions, and a thunk's parameter, so in the
//      common case the operand simply *is* a constant. No backwards scan, no
//      dominator walk, no heuristic; the data flow was made explicit at lift
//      time precisely so this step could be a lookup.
//
//   2. **Trim the arguments.** `write(fd, buf, n)` reads three registers; the
//      other three are whatever happened to be in x3..x5. Keeping them would
//      not just be noise — a syscall in a function that never writes x5 reads
//      the *entry* value of x5, which makes the function look like it takes
//      six arguments. Cutting the operand list to the syscall's real arity
//      removes both problems, and lets dead code elimination collect whatever
//      was keeping those registers alive.
//
//   3. **Record what was learned.** The syscall's name goes on the op as a
//      note, so the emitter prints `write(...)` and the IL dump says which
//      syscall a bare number was.
//
// Every step degrades rather than guesses. No syscall table wired up: nothing
// happens, and the intrinsic prints as it did. Number not a constant: a note
// says so and all six arguments stay, because with an unknown callee every
// register might be an argument. Number constant but not in the table: the
// number is recorded and the arguments are kept, since the table's silence is
// about the table, not about the code.
#pragma once

#include <memory>

#include "xdec/pass/pass.h"

namespace xdec::passes {

[[nodiscard]] std::unique_ptr<pass::Pass> makeRecoverSyscallPass();

/// The intrinsic name the AArch64 spec lifts `svc` to. Shared with the emitter,
/// which must recognise exactly the same ops this pass annotated.
inline constexpr std::string_view kSyscallIntrinsic = "aarch64.svc";

/// Operand layout of that intrinsic: `imm16`, then x8, then x0..x5.
inline constexpr std::size_t kSyscallImmOperand = 0;
inline constexpr std::size_t kSyscallNumberOperand = 1;
inline constexpr std::size_t kSyscallFirstArgOperand = 2;
inline constexpr std::size_t kSyscallMaxArgs = 6;

}  // namespace xdec::passes
