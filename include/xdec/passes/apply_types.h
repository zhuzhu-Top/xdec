// apply-types: what an imported header changes about the IL, before anything
// downstream counts registers.
//
// Most of type import is spelling, and spelling belongs in the emitter. One
// part of it is not: **arity**. SSA construction attaches the calling
// convention's full argument set to every call, because with no declaration in
// sight any of the eight registers might be an argument — the same
// over-approximation recover-syscall removes for `svc` once the syscall table
// names the number. A header naming the callee is exactly that same kind of
// evidence, so this pass does the same thing with it:
//
//   * A **direct** call whose target address a symbol names, where the header
//     declared a prototype under that name, keeps that prototype's parameters
//     and drops the rest.
//   * An **indirect** call through a value whose type is known — one of this
//     function's own parameters, typed by this function's own imported
//     prototype, or a GOT/import slot a relocation resolves to another
//     module's symbol — is trimmed to the pointee function's arity.
//
// Why here and not in the emitter: `vars` counts what the body reads to decide
// what this function's parameters are, so a call that still passes eight
// registers makes eight parameters appear in a signature the header says has
// three. Trimming after that point fixes the call and leaves the signature
// wrong. Trimming before it fixes both.
//
// What this pass will not do is *extend* a call. A prototype that declares
// more parameters than the convention attached describes a call this code does
// not make, which means the header is for a different version of the callee —
// and inventing the extra arguments would put values into a call that the
// program never passes. It also never touches a variadic prototype's tail:
// `printf` really does read as many registers as it was given.
//
// Absent a type database this pass is a no-op, which is the normal case.
#pragma once

#include <memory>

#include "xdec/pass/pass.h"

namespace xdec::passes {

[[nodiscard]] std::unique_ptr<pass::Pass> makeApplyTypesPass();

}  // namespace xdec::passes
