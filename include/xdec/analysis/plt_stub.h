// Recognising an AArch64 ELF PLT stub and the import it forwards to.
//
// A direct call to an imported function almost never targets the import
// itself: it targets a small stub the linker placed in `.plt`, four
// instructions that load the real address out of a GOT slot the loader fills
// in and jump to it. Everything a decompiler wants to say about the call --
// its name, its prototype, its arity -- is a fact about the import behind
// that indirection, not about the stub's own (usually symbol-less) address.
// This is the one place that indirection is decoded, shared by every analysis
// that needs to see through it (see analysis/noreturn.h for the first one,
// and docs/10-import-resolution.md for the call-site taxonomy this serves).
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "xdec/support/reader.h"

namespace xdec::analysis {

/// If `stubVa` is the standard ELF/AArch64 PLT stub shape --
/// `adrp Xn, page; ldr Xm, [Xn, #off]; ...` -- returns the GOT slot address
/// the stub loads its real target from. `reader` reads the plain image: PLT
/// stub bytes are ordinary executable code, not a relocated slot, so no
/// MemoryFacts::isImmutable check applies to them.
///
/// Returns nullopt for anything that does not match: a direct call to a
/// function's own code, which is the overwhelmingly common case, is rejected
/// by the very first instruction almost always.
[[nodiscard]] std::optional<uint64_t> pltGotSlot(const ByteReader& reader, uint64_t stubVa);

/// The import `stubVa` ultimately calls, when `stubVa` is a PLT stub (see
/// pltGotSlot) whose GOT slot `facts` binds to a name. Nullopt for anything
/// that is not a recognised PLT stub, or whose GOT slot the loader has not
/// resolved to a name -- the same "no evidence, no claim" default every
/// import-resolution question in this project answers.
[[nodiscard]] std::optional<std::string> importNameForPltStub(uint64_t stubVa,
                                                               const ByteReader& reader,
                                                               const MemoryFacts& facts);

}  // namespace xdec::analysis
