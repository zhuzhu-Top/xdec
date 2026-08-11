// Recognises a Store through a stack slot nothing in the function ever reads
// back -- the write-side mirror of stack_load_fold.h's read-side folding.
//
// A spill the register allocator (or, here, the pattern this analysis
// exists for: an MBA round function threading its intermediate words through
// stack slots) writes and never reloads is not an IL fact worth deleting --
// the Store is exactly the one memory write the machine code performs -- but
// it is still redundant *emitted C*: `var_aa8 = _cse8;` right after
// `_cse8 = bswap32(t32);` says nothing a reader needs, because `var_aa8`
// never appears again. `printOp`'s Store case (emit/c_stmt.cpp) prints every
// Store unconditionally, on the same "a use might be arbitrarily far away"
// assumption `nameResultTemps` makes for loads; when nothing anywhere reads
// the slot back, and the address never escapes somewhere this analysis
// cannot see through, that assumption is provably false and the statement
// buys nothing.
#pragma once

#include <cstdint>
#include <unordered_set>

#include "xdec/analysis/stack_frame.h"
#include "xdec/analysis/variables.h"
#include "xdec/il/function.h"

namespace xdec::analysis {

/// Every Store in `function` provably dead, keyed by its `OpId::index()`.
/// Safety rules, all required, checked function-wide rather than between one
/// store and one reader (contrast findFoldableStackLoads): a dead store's
/// value must be unobservable from *anywhere* in the function, not just
/// absent along one path.
///
///  1. The store's address classifies as a StackSlot.
///  2. No Load anywhere in the function reads that same delta -- live or
///     already dead itself (see stack_load_fold.h): a load `deadOps` folds
///     into its reader's text still performs the read as far as this
///     analysis is concerned, it is only spelled differently.
///  3. The slot's delta is not escaped (see analysis::StackEscapeMap): it is
///     never an operand of anything other than as the address of a Load or a
///     Store (its own address operand included) -- not a Call or Intrinsic
///     argument, which may read through any pointer at all, and not stored
///     as a value into some other location this analysis would then have to
///     track indirectly. An escaped delta's protection also covers every
///     other delta StackEscapeMap closes into the same region: a Store one
///     slot above an escaped pointer may be writing a field of the same
///     aggregate the callee reads through it, and this analysis has no way
///     to tell that store's field apart from the pointer's own.
///  4. The slot is not an aliased field (`Variable::aliasBase`): an aliased
///     local's liveness is tied to its base slot's own accesses, which this
///     analysis does not model.
///
/// A delta with several Stores and no reader at all makes every one of them
/// dead together, regardless of order: nothing between any two of them is
/// ever observed either.
[[nodiscard]] std::unordered_set<uint32_t> findDeadStackStores(
    const il::Function& function, const StackFrame& frame, const VariableTable& variables);

}  // namespace xdec::analysis
