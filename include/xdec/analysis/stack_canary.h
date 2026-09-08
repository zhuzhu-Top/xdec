// Recognising the `-fstack-protector` save/compare round trip: a value read
// from some fixed address is stashed in a stack slot near a function's start,
// then re-read from that same address and compared against the stashed copy
// right before a return -- the shape every mainstream compiler leaves behind
// for a stack canary, regardless of what the guard address's own symbol (if
// it even has one xdec's current symbol tables can see) is called.
//
// This is deliberately silent about *which* runtime global the address is:
// on a Mach-O or ELF image plt_stub.h/noreturn.h already have a real name to
// offer through the ordinary import-naming path when one is bound there, and
// on a dyld_shared_cache image the address the const-fold pass leaves behind
// is frequently a private slot with no exact symbol in the subset of tables
// xdec has loaded (see docs/22-dyld-shared-cache.md) -- printing a name in
// that case would be exactly the kind of guess this project does not make.
// What is provable from the IL alone, on every platform alike, is the
// round-trip *shape* itself, and that is all this reports: enough for the
// emitter to drop one comment on the save that tells a reader "this local
// holds a canary, not a value the function computed for its own use" instead
// of leaving them to notice the same raw address appears twice, sixty lines
// apart, and work that out for themselves.
#pragma once

#include <cstdint>
#include <vector>

#include "xdec/analysis/stack_frame.h"
#include "xdec/il/function.h"

namespace xdec::analysis {

struct StackCanarySave {
  /// The `Store` that copies the guard's value into the stack slot.
  il::OpId save;
  /// The `CondBranch` whose condition re-reads `guardAddress` and compares it
  /// against the slot `save` filled in -- the epilogue side of the round trip.
  il::OpId check;
  /// The fixed address both the save and the check load from. Not resolved
  /// to a name here; see the header comment for why.
  uint64_t guardAddress = 0;
  /// The stack slot's displacement from the entry stack pointer, purely for
  /// callers that want to say which local without re-deriving it from `save`.
  int64_t savedDelta = 0;
};

/// Every save/check round trip `function` proves: a `Store` of `Load(A)` into
/// a stack slot at delta D, later joined by a `CondBranch` whose condition is
/// `Load(A) != Load(slot at D)` (or the `==` polarity), for a Global address
/// `A` in the analysis/stack_frame.h sense -- a plain constant, the one shape
/// a guard address takes on every target this project supports. Each save is
/// reported once, at its earliest confirmed check; a slot saved but never
/// re-checked is not this shape (dead-store analyses already have their own
/// say about it) and is left out.
[[nodiscard]] std::vector<StackCanarySave> findStackCanarySaves(const il::Function& function,
                                                                const StackFrame& frame);

}  // namespace xdec::analysis
