// Recognising calls that never return to their caller.
//
// The lifter's block scanner does not stop at a `call`: a call almost always
// returns, so its fall-through is ordinary control flow and probing it as a
// leader like any other address is what keeps straight-line code in one
// block. That default is wrong for the handful of C runtime entry points that
// are called specifically because they *don't* return -- `__stack_chk_fail`
// on a blown stack canary chief among them. When the byte just past such a
// call happens to be the first instruction of the next function in the
// image (as it usually is: the compiler does not reserve space for code that
// runs), scanning its fall-through walks straight into that function's body
// and attributes every block it finds there to the caller. One mis-scanned
// canary check away, a clean function is reported hundreds of blocks and
// several unrelated dispatch tables larger than it is.
//
// This file draws the line the engine does not: given a direct call target,
// say whether it is a PLT stub bound to a symbol on the short list of
// runtime functions that are documented to never return.
#pragma once

#include <string_view>

#include "xdec/analysis/plt_stub.h"
#include "xdec/support/reader.h"

namespace xdec::analysis {

/// Whether `name` is a widely used C runtime / libc entry point documented to
/// never return control to its caller. Deliberately a short, conservative
/// list: a false negative only keeps today's (already accepted) behaviour,
/// while a false positive would truncate a function that really does
/// continue after the call.
[[nodiscard]] bool isKnownNoreturnSymbol(std::string_view name) noexcept;

/// Whether a direct call to `target` is known to never return: `target` is a
/// PLT stub (see plt_stub.h's pltGotSlot) whose GOT slot `facts` binds to a
/// symbol isKnownNoreturnSymbol recognises.
///
/// Absent facts (a default-constructed MemoryFacts, as every caller other
/// than a real binary's decompile run supplies) answers false for everything,
/// which is the same "no information, assume it returns" default the engine
/// already has.
[[nodiscard]] bool isNoreturnCallTarget(uint64_t target, const ByteReader& reader,
                                       const MemoryFacts& facts);

}  // namespace xdec::analysis
