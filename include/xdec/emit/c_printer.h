// C emission: structured statement tree plus expressions to C text.
//
// Readability decisions, stated where they cannot drift from the code:
//   * Expressions inline bottom-up. A value with more than one use, and any
//     load, becomes a named temporary assigned at its defining op — this is
//     what keeps DAG sharing from exploding into exponential duplication.
//   * Stack slots print as local variables: a load through `entry(sp)+d` is
//     `var_d`, a store is an assignment. Everything else is an explicit
//     typed dereference `*(uint32_t*)(addr)`.
//   * Widths are honest: sub-64-bit arithmetic wraps in an explicit cast,
//     signed operations cast their operands, extensions are casts.
//   * What C can say unambiguously and portably gets a short, plain name --
//     `rotr32`, `bswap64`, `cc_lt32` -- defined once in xdec_helpers.h and
//     pulled in with one `#include` (see COptions::helpersHeader) instead of
//     repeating a definition inline in every decompiled file.
//   * What the IL cannot yet say in C at all gets an `xdec_`-prefixed
//     embedder stub instead of a guess: declared in that same header when
//     one fixed signature covers it (`xdec_clz32`, `xdec_fadd32`, ...), or
//     declared here, on demand, when it cannot be (`__xdec_intrin_*` is a
//     family of differently-named calls, `__xdec_syscall`'s declaration
//     predates the header, `__xdec_unimplemented` is never declared at all).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "xdec/analysis/stack_frame.h"
#include "xdec/analysis/variables.h"
#include "xdec/emit/structure.h"
#include "xdec/il/function.h"
#include "xdec/support/reader.h"
#include "xdec/types/binder.h"

namespace xdec::types {
class SyscallTable;
class TypeDatabase;
}  // namespace xdec::types

namespace xdec::analysis {
class TypedVariables;
}  // namespace xdec::analysis

namespace xdec::emit {

/// What a binary's symbol table says about an address.
///
/// `offset` is what makes this worth having as a struct rather than a name
/// lookup. A stripped-but-not-quite binary has a handful of large symbols and
/// nothing else, so the useful answer for most addresses is not "this is
/// `foo`" but "this is 0x334 bytes into `foo`" — which is exactly what tells a
/// reader that the function they are looking at is a chunk of a known export
/// rather than something anonymous.
struct SymbolRef {
  /// Empty when no defined symbol covers the address.
  std::string name;
  uint64_t offset = 0;
  bool isFunction = false;

  [[nodiscard]] bool found() const noexcept { return !name.empty(); }
  /// True when the address is the symbol itself, so the name can stand in for
  /// it — as a callee's name, say. At a non-zero offset the name is a location,
  /// not an identity, and using it as one would be wrong.
  [[nodiscard]] bool exact() const noexcept { return found() && offset == 0; }
};

/// Resolves an address to the symbol covering it. A resolver that is not set
/// resolves nothing, which is what emission from a pipeline with no image gets;
/// every caller must handle a not-found answer anyway, so there is no separate
/// "no symbols at all" case to write.
using SymbolResolver = std::function<SymbolRef(uint64_t va)>;

/// Which part of the address space an address belongs to.
///
/// The question a reader has about `*(uint64_t*)0x30cc20` is not what it is
/// called — in a stripped library it is called nothing — but whether it can
/// change. The same expression is a constant baked into the program when the
/// address is in `.rodata` and mutable state when it is in `.data`, and the
/// difference decides whether the value can be reasoned about at all. Nothing in
/// a bare hexadecimal address says which.
struct AddressFacts {
  /// Empty when the address is in no named section, which includes not being
  /// mapped at all.
  std::string section;
  bool mapped = false;
  /// Whether the program can write there. Only meaningful when mapped.
  bool writable = false;
  /// Whether this is somewhere the program keeps data, and so somewhere a name
  /// stands for something. Being mapped is not enough and is the wrong question
  /// to ask instead: a shared object maps its own symbol and relocation tables
  /// too, and it maps from address zero, so every small integer the code happens
  /// to dereference is "mapped" without being a variable.
  bool variable = false;
  /// The imported symbol a pointer slot here binds to, when a relocation says.
  /// Empty otherwise. This is the difference between a reader learning that the
  /// code calls through a table and learning that it calls `dlsym`.
  std::string importName;
};

/// Describes a data address. Unset describes nothing, and then addresses print
/// as the bare numbers they always did.
using AddressDescriber = std::function<AddressFacts(uint64_t va)>;

struct COptions {
  /// Function name; empty derives it from the entry block — the symbol's name
  /// when one starts there, `sub_<entry va>` otherwise.
  std::string name;
  /// Emit block addresses as comments before each block's statements.
  bool annotateBlocks = true;
  /// Write a returned or assigned conditional select as an `if` rather than a
  /// `?:`. The compiler's branchless form is one expression; a chain of them (a
  /// clamp, a three-way compare) is one expression per nesting level, and the
  /// guards it came from read better than the parentheses it became.
  ///
  /// Only where the select is the WHOLE value being returned or assigned. One
  /// nested inside a larger expression stays a `?:`, because hoisting it out
  /// would move a computation across code that may depend on where it sat --
  /// and because `x ^ (c ? y : 0)` is what the source looked like too.
  bool preferIfOverTernary = true;
  /// How to name addresses: callees, and the function itself. Absent means
  /// every address prints as a bare number, which is where this started.
  SymbolResolver symbols;
  /// What the image says about the addresses the body loads from and stores to.
  AddressDescriber addresses;
  /// A fixed call target's name when no symbol starts exactly there: the
  /// import behind a PLT stub's GOT indirection (see
  /// analysis/plt_stub.h), already aliased to the header's own spelling of it
  /// (bionic's `__errno` vs. the NDK header's `__errno_location`; see
  /// binary::TargetProfile). Consulted by CContext::calleeName and its own
  /// type binder after `symbols` finds no exact function there. Absent skips
  /// the fallback, same as before this existed: such a call still prints as
  /// `sub_<va>`.
  types::NameAt names;
  /// What the loader resolves a GOT/import slot to, for the same "call
  /// through a slot a `Load` reads" shape analysis::calleeThroughImportSlot
  /// answers for arity and typing. Absent leaves such a call typed only by
  /// the machine's own register width, same as before this existed.
  MemoryFacts memory;
  /// Declarations imported from headers, when the caller supplied any. What
  /// this changes is spelling, never structure: a signature becomes
  /// `int32_t f(EvalVec3 *v)` instead of `uint64_t f(uint64_t a0)`, and an
  /// address the header named stops being `g_30c420`. Absent — the default —
  /// leaves every one of those exactly as inference produced it.
  const types::TypeDatabase* types = nullptr;
  /// The kernel's syscall numbering, for turning a recovered `svc` into a
  /// named call. Absent leaves syscalls as the raw helper.
  const types::SyscallTable* syscalls = nullptr;
  /// Type evidence a call or `svc` site's own signature gave a value this
  /// function computes, layered the same way `variables` layers it onto
  /// arguments and locals (see analysis::TypedVariables) -- but keyed by
  /// every call/intrinsic result, not just the ones VariableTable models as
  /// a Variable. Absent leaves a call result declared at its measured width,
  /// same as before this analysis existed.
  const analysis::TypedVariables* typedVariables = nullptr;
  /// The `#include` path the preamble names for the portable semantics
  /// helpers (rotate, byte swap, population count, the overflow-exact
  /// condition codes -- see xdec_helpers.h) when the body used any of them.
  /// Empty suppresses the include; the caller is then responsible for
  /// making those names available some other way.
  std::string helpersHeader = "xdec_helpers.h";
};

[[nodiscard]] std::string printFunction(const il::Function& function,
                                        const analysis::VariableTable& variables,
                                        const analysis::StackFrame& frame,
                                        const StructuredFunction& structured,
                                        const COptions& options = {});

}  // namespace xdec::emit
