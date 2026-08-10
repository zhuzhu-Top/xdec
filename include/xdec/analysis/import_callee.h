// resolveCallee / calleeThroughImportSlot: the function type behind a call
// target, from every shape of evidence a call site can carry.
//
// passes::ApplyTypes (arity trimming, before emission) and
// analysis::TypedVariables (argument/return typing, for the emitter) ask
// exactly this question of exactly the same three shapes -- a constant
// address, a GOT/import slot a `Load` reads, a function-pointer parameter --
// and disagreeing would be visible: a callee ApplyTypes trimmed one way and
// TypedVariables typed another would leave a call's argument list and its
// declared parameter types out of step. This is the one place the question is
// answered, so both stay in step by construction.
#pragma once

#include <optional>
#include <string>

#include "xdec/binary/target_profile.h"
#include "xdec/il/function.h"
#include "xdec/support/reader.h"
#include "xdec/types/binder.h"

namespace xdec::analysis {

/// Which parameter an entry register is, under the AArch64 convention.
[[nodiscard]] int calleeArgumentIndex(const il::Function& function, il::RegId root);

/// The import name behind a GOT/import slot call target -- the same shape
/// `calleeThroughImportSlot` matches (`Load` of a constant loader-bound
/// address), stopping one step short of binding it against a header. Split
/// out because naming a callee and typing it are different questions with
/// different answers when no header is loaded: emit's indirect-call comment
/// (c_stmt.cpp's printCall) wants the name alone, with no `TypeBinder` to ask
/// and no need for one.
///
/// `profile` aliases the result the same way `calleeThroughImportSlot` does;
/// nullopt for anything that is not this shape, or whose slot the loader has
/// not resolved to a name.
[[nodiscard]] std::optional<std::string> importNameThroughSlot(
    const il::Function& function, const MemoryFacts& memory, il::ExprId target,
    const binary::TargetProfile* profile = nullptr);

/// The prototype for a call through a GOT/import slot: the target is a value
/// read (`Load`) from a constant address, and that address is one the loader
/// fills in from another module rather than one the file's own bytes name.
/// The name that names the *fill* is the callee's, not the slot's own (see
/// types::TypeBinder's header on this exact rule) -- the same shape
/// recover_tailcall.cpp's origin walk resolves for a tail call.
///
/// `profile`, when given, aliases the loader's spelling to the header's
/// before binding (see binary::TargetProfile::symbolAliases): Bionic's GOT
/// entry for the syscall-error accessor is `__errno`, the NDK header declares
/// it as `__errno_location`, and without the alias neither name would find
/// the other's declaration.
[[nodiscard]] const types::TypeEntry* calleeThroughImportSlot(
    const il::Function& function, const types::TypeBinder& binder, const MemoryFacts& memory,
    il::ExprId target, const binary::TargetProfile* profile = nullptr);

/// The function type behind a call target, from the three kinds of evidence
/// there are: a symbol at a constant address (including one reached through a
/// PLT stub, once `binder`'s own name resolver sees through it -- see
/// analysis/plt_stub.h), an import a GOT/relocation slot resolves to (see
/// calleeThroughImportSlot), and the declared type of a function-pointer
/// parameter of the enclosing function (`self`).
[[nodiscard]] const types::TypeEntry* resolveCallee(const il::Function& function,
                                                    const types::TypeBinder& binder,
                                                    const types::TypeEntry* self,
                                                    const MemoryFacts& memory, il::ExprId target,
                                                    const binary::TargetProfile* profile = nullptr);

}  // namespace xdec::analysis
