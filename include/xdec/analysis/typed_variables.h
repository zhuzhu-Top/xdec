// TypedVariables: type evidence traced from call and syscall sites back to
// the SSA values, entry registers, and stack slots that fill them.
//
// analysis::VariableTable answers "how wide is this, and is it a pointer" from
// how the body itself uses a value. That is honest but blind to the one other
// source of truth a decompiler can have: a callee's own declared signature.
// `write(fd, buf, n)` says the second argument is `const void*` and the third
// is `size_t`, and that fact is knowable the moment the call is seen, whether
// or not this function's body ever dereferences the pointer. This analysis is
// what carries it from the call site to wherever the argument came from.
//
// Three sources of signatures:
//
//   * an imported prototype, through types::TypeBinder, for a direct call or
//     one through a typed function-pointer parameter (see apply_types.cpp's
//     calleeOf, which this mirrors);
//   * the same prototype, for a call through a GOT/import slot -- the target
//     is not a constant the binder can look up, but the loader value the slot
//     resolves to names an import, and that name is what binds (see
//     calleeThroughImportSlot in the .cpp);
//   * the same import-slot binding, for a *data* symbol instead of a
//     function: a value loaded from a GOT slot that names an extern global is
//     a pointer to whatever the header declared under that name (see
//     globalPointerType in the .cpp), which is what lets a field read off it
//     print as a field instead of a raw offset;
//   * the syscall table, once SyscallTable::resolveTypes has turned its
//     spellings into TypeIds, for `svc`.
//
// From each typed call, the evidence is walked backward to where it can be
// recorded (see propagate() in the .cpp for the exact shapes): an entry
// register, a stack-frame slot, a phi, or -- through zero or more Select arms
// -- one of those. An expression the walk cannot place (arithmetic, a load, an
// opaque call result with no evidence of its own) is left alone rather than
// guessed at, the same "evidence, not instruction" rule types::TypeBinder
// documents.
//
// This is an analysis, not a pass: it produces a side table (mirroring
// StackFrame and VariableTable) rather than rewriting the IL, both because a
// TypeId has no business inside the pure expression pool (see
// types/type.h's own header on why analysis::CType and types::TypeId stay
// separate vocabularies) and because its only consumers -- VariableTable's
// importedType and the emitter -- already run as a post-pipeline analysis
// step, not inside the pass manager.
#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>

#include "xdec/analysis/stack_frame.h"
#include "xdec/analysis/variables.h"
#include "xdec/binary/target_profile.h"
#include "xdec/il/function.h"
#include "xdec/support/reader.h"
#include "xdec/types/binder.h"
#include "xdec/types/syscall_table.h"

namespace xdec::analysis {

/// A signature this analysis can use in place of a header, for a callee this
/// binary defines but which no header describes -- typically because it is a
/// local helper with no exported symbol, so types::TypeBinder has no name to
/// bind a prototype under. See summaryOf() for how one is produced from a
/// callee already decompiled in the same run, and recover()'s `summaries`
/// parameter for how it is consumed at that callee's call sites in a
/// *different* function. Deliberately kept out of both functions' own load
/// path so that "no summary available" -- a single-function run, or a callee
/// this one never reaches -- costs nothing.
struct CalleeSummary {
  std::vector<types::TypeId> paramTypes;
  types::TypeId returnType;
};

using CalleeSummaries = std::unordered_map<uint64_t, CalleeSummary>;

class TypedVariables {
 public:
  /// Recovers type evidence from every Call and `svc` in `function`, at Ssa
  /// maturity or beyond. `database`/`syscalls` may be null, matching
  /// pass::Context: a run with no header and no syscall table finds no
  /// evidence and every lookup below answers unset, exactly like a
  /// VariableTable recovered without one. `names` resolves an address to the
  /// symbol covering it, the same resolver types::TypeBinder takes. `memory`
  /// resolves a GOT/import slot's loader-filled value, so a call through one
  /// (`blr x8` after `ldr x8, [import_slot]`) binds to the imported symbol's
  /// own prototype the same way a direct call to it would (see
  /// calleeThroughImportSlot in the .cpp); an unset resolver finds no such
  /// calls, exactly like today. `summaries`, see CalleeSummary, is optional
  /// and empty in the CLI today. `profile`, when given, aliases a GOT/import
  /// slot's loader name to the header's spelling before binding (see
  /// analysis::calleeThroughImportSlot); null skips aliasing, same as before
  /// this parameter existed.
  [[nodiscard]] static TypedVariables recover(const il::Function& function,
                                              const StackFrame& frame,
                                              const types::TypeDatabase* database,
                                              const types::SyscallTable* syscalls,
                                              const types::NameAt& names,
                                              const CalleeSummaries& summaries = {},
                                              const MemoryFacts& memory = {},
                                              const binary::TargetProfile* profile = nullptr);

  /// Evidence for the entry value of `root` -- an argument register, in
  /// VariableTable's terms.
  [[nodiscard]] std::optional<types::TypeId> forArgument(il::RegId root) const;
  /// Evidence for an SSA value: a phi (VariableTable's temps) or a call/`svc`
  /// result (which VariableTable does not itself model; see c_context.cpp's
  /// valueTypes, the emitter's own table for exactly this).
  [[nodiscard]] std::optional<types::TypeId> forValue(il::ValueId value) const;
  /// Evidence for a stack-frame slot, keyed the same way StackFrame::classify
  /// reports one: signed displacement from the entry stack pointer.
  [[nodiscard]] std::optional<types::TypeId> forStackSlot(int64_t delta) const;
  /// Evidence for what this function itself returns, when every `ret` traces
  /// to a call result this analysis already typed (see recover()'s return
  /// pass). Distinct from a header's own declared return for this function's
  /// symbol, which the emitter already reads straight off the binder.
  [[nodiscard]] std::optional<types::TypeId> returnType() const noexcept { return returnType_; }

 private:
  std::unordered_map<uint32_t, types::TypeId> byArgument_;
  std::unordered_map<uint32_t, types::TypeId> byValue_;
  std::unordered_map<int64_t, types::TypeId> byStackSlot_;
  std::optional<types::TypeId> returnType_;
};

/// The CalleeSummary for a function already recovered as `variables` (layered
/// with `typed` via VariableTable::applyImportedTypes) and `typed` itself --
/// the two places this analysis' own evidence about its own signature ends
/// up. A caller decompiling more than one function from the same binary in
/// one run keys this by the function's entry address and passes the result
/// map back in as recover()'s `summaries` for whichever functions it visits
/// next (see Phase 6 of the type-propagation plan; nothing in this CLI wires
/// that discovery loop yet, so this is the building block such a driver would
/// call once per function it finishes).
///
/// Parameters are ordered a0, a1, ... by calling-convention position, read
/// off VariableTable's own "aN" argument naming; a register the body never
/// reads, or one with no imported-type evidence, is TypeId{} at that
/// position, same as "no evidence" everywhere else here.
[[nodiscard]] CalleeSummary summaryOf(const VariableTable& variables, const TypedVariables& typed);

}  // namespace xdec::analysis
