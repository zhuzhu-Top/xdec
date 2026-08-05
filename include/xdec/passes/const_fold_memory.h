// The Ssa-level immutable-memory folding pass.
//
// A load from an address the program can never write is not an observation of
// memory; it is a constant of the program that happens to be spelled as a
// load. Obfuscators lean on the difference: an MBA chain whose seed is
// `ldr x0, [#0x30c420]` looks data-dependent and analysis-proof, but the seed
// sits in `.rodata`, so every value derived from it is knowable from the file
// alone. Leaving the load opaque strands the whole chain — SCCP marks a load's
// result overdefined, and one overdefined leaf poisons every meet above it.
//
// So this pass reads the bytes and puts the constant where the load's value
// was, which is what lets the constant propagator and the algebra rules see
// through to the arithmetic underneath. That is its real payoff: not the
// handful of loads it removes, but the expression trees that collapse once
// their leaves are literals.
//
// What makes it sound is the narrow question it asks (see
// BinaryImage::isImmutable): every byte mapped, no byte writable, no
// relocation patching any of them. `.rodata` answers yes; `.data` answers no
// because the program may write it; `.data.rel.ro` answers no because the
// loader writes it with a value that depends on which module wins a symbol.
// Nothing here guesses — an address whose immutability is unknown keeps its
// load, and a pipeline that wires no immutability facts at all folds nothing.
#pragma once

#include <memory>

#include "xdec/pass/pass.h"

namespace xdec::passes {

[[nodiscard]] std::unique_ptr<pass::Pass> makeConstFoldMemoryPass();

}  // namespace xdec::passes
