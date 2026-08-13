// The state the C emission stages share.
//
// Emission is three stages over one context: expressions (c_expr), statements
// and ops (c_stmt), then the assembly of preamble, signature and declarations
// (c_printer). The context carries what all three read and the collections the
// last stage needs the first two to have filled — which callees were named,
// which helpers were used, which machine registers had to become variables.
#pragma once

#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "xdec/analysis/image_literals.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/analysis/variables.h"
#include "xdec/analysis/vtable_call.h"
#include "xdec/emit/c_printer.h"
#include "xdec/emit/structure.h"
#include "xdec/il/function.h"

#include "xdec/types/binder.h"

namespace xdec::types {
struct SyscallInfo;
}  // namespace xdec::types

namespace xdec::emit {

/// C integer type for a width. Width 1 is `bool`, which matters beyond
/// spelling: a 1-bit value's operations are logical, not bitwise.
[[nodiscard]] std::string intType(uint32_t width, bool isSigned = false);

/// A fixed address the body reads or writes, as it will be declared.
///
/// Named rather than left as a number for the reason any variable is: an address
/// repeated across two hundred dereferences is one thing being used two hundred
/// times, and a name says so where a number leaves the reader to notice. The
/// declaration is then the one place the facts about it go.
struct GlobalVar {
  std::string name;
  AddressFacts facts;
  /// True when a symbol named it, so the name is the program's own and the
  /// address is only corroboration.
  bool fromSymbol = false;
};

/// A machine register the IL kept in op form, because register SSA tracks only
/// the general, flag and stack-pointer classes. Vector and system registers
/// therefore still appear as read/write ops, and printing them as a named
/// variable is what keeps their values visible instead of vanishing.
struct RegVar {
  std::string name;
  uint32_t width = 64;
};

/// A `Value` substituted directly with the text of the stack slot a
/// `analysis::findFoldableStackLoads` load reads, in place of a temporary:
/// `var_980` where the ordinary path would have declared `t294 = var_980;`
/// and every use of `t294` read the temporary back. `pointer` mirrors the
/// same fact `tempFor`'s own pointer temps carry, so a use of this text in
/// address arithmetic still casts back to an integer where it needs to (see
/// ExprPrinter::integerOperand).
struct InlinedStackLoad {
  std::string text;
  bool pointer = false;
};

/// A `Value` substituted with a freshly re-rendered `(*(T*)ADDR)`, for a
/// `analysis::findFoldableMemoryLoads` load whose address is not a stack
/// slot: unlike a stack slot, there is no fixed name to precompute at
/// analysis time, so this carries just the address expression and width,
/// and `ExprPrinter::value` renders the dereference itself at the point of
/// substitution (see that function).
struct InlinedMemoryLoad {
  il::ExprId address;
  uint32_t width = 0;  // bits
};

class CContext {
 public:
  CContext(const il::Function& theFunction,
           const analysis::VariableTable& theVariables,
           const analysis::StackFrame& theFrame,
           const StructuredFunction& theStructured, const COptions& theOptions);

  const il::Function& function;
  const analysis::VariableTable& variables;
  const analysis::StackFrame& frame;
  const StructuredFunction& structured;
  const COptions& options;
  /// Recovers a constant address's referent as a literal, where `options`
  /// supplied an image to read it from (see AddressRenderer, the only
  /// consumer). Built with an absent reader when it did not, so `at()`
  /// answers nothing everywhere -- the same "no evidence" shape `addresses`
  /// and `symbols` already default to.
  analysis::ImageLiteralRecovery literals;

  /// Value index -> temporary name, for results that must not be re-evaluated
  /// at each use: loads, calls, intrinsics, and untracked register reads.
  std::map<uint32_t, std::string> tempNames;
  /// Value index -> the imported type a field read gave it (see typeOfValue).
  std::map<uint32_t, types::TypeId> valueTypes;
  /// Root register index -> the variable standing in for it.
  std::map<uint32_t, RegVar> regVars;
  /// Helper keys the body used; c_helpers turns them into declarations.
  std::set<std::string> helpers;
  /// Direct call targets, by address, each with the name it was called by, so
  /// the preamble declares exactly what the body used. A map rather than a set
  /// of addresses because the name is a lookup the body already did.
  std::map<uint64_t, std::string> callees;
  /// Entry-value externs the body referenced, with the width each carries.
  std::map<std::string, uint32_t> entryLeaves;
  /// Syscalls the body reached, by the name it called them with, each mapped to
  /// the prototype the preamble declares. Keyed by name rather than number so a
  /// syscall issued from two places is declared once.
  std::map<std::string, std::string> syscalls;
  /// `struct timeval` and friends: tags a syscall signature names but no header
  /// here defines, so the casts that use them need a forward declaration.
  std::set<std::string> syscallTags;
  /// Fixed addresses the body dereferenced, in address order so the declarations
  /// read like a memory map.
  std::map<uint64_t, GlobalVar> globals;
  /// Value index -> snapshot name, in force while one edge's phi copies are
  /// printed. Phi copies are parallel: a copy whose source reads another
  /// copy's destination must see the value from before the edge, so that
  /// destination is snapshotted and reads are redirected here.
  std::map<uint32_t, std::string> snapshots;
  /// Name and bit width, in declaration order, of every subexpression
  /// `ExprPrinter` promoted to a temporary because it was referenced 2+
  /// times in one printed scope (see ExprPrinter::beginScope). Declared like
  /// any other temp; c_stmt.cpp prints the assignment that fills each one
  /// as an ordinary statement right before the code that first needs it.
  std::vector<std::pair<std::string, uint32_t>> cseTemps;
  /// Ops the body will never print -- today, exactly `deadJumpTableLoad`'s
  /// finds (see c_stmt.h). Computed once, in the constructor, from the
  /// already-built structured tree: before `Assembler::nameResultTemps`
  /// walks every op deciding what needs a declaration, and before
  /// `StmtPrinter` walks the same ops deciding what to print, so a
  /// declared-but-never-assigned temporary can never happen.
  std::unordered_set<uint32_t> deadOps;

  /// Load op indices to print through an ACLE `__ldaxrN` call instead of a
  /// plain dereference (see analysis::findExclusiveLoads): the paired
  /// `aarch64.reserve` intrinsic is already folded into `deadOps`, and this
  /// set is what tells StmtPrinter's Load case to spell the reassembled
  /// operation the way ldaxr/ldxr's ACLE name does, not as an ordinary read.
  std::unordered_set<uint32_t> exclusiveLoads;

  /// `aarch64.store_exclusive_status` intrinsic op index -> the `Store`
  /// immediately before it in the same block (see
  /// analysis::findExclusiveStores), already folded into `deadOps`. Gives
  /// the intrinsic's own print the address and value ACLE's `__stlxrN`
  /// takes, which its own (empty) operand list does not carry.
  std::map<uint32_t, il::OpId> exclusiveStoreFor;

  /// Value index -> the slot text substituted for a folded stack load's
  /// result (see analysis::findFoldableStackLoads). Checked by
  /// ExprPrinter::value ahead of tempFor, so a value in here is never
  /// declared as its own temporary either -- its defining Load's OpId is in
  /// `deadOps` for exactly this reason, filled in by this constructor
  /// alongside this map.
  std::map<uint32_t, InlinedStackLoad> inlinedStackLoads;

  /// Value index -> the address/width substituted for a folded non-stack
  /// load's result (see analysis::findFoldableMemoryLoads). Checked by
  /// ExprPrinter::value alongside `inlinedStackLoads`; its defining Load's
  /// OpId is in `deadOps` for the same reason.
  std::map<uint32_t, InlinedMemoryLoad> inlinedMemoryLoads;

  /// Stack deltas analysis::findDeadStackStores proved entirely dead: every
  /// Store to this slot is one `deadOps` above already excludes, so nothing
  /// in the body ever assigns or reads it. Checked by c_printer.cpp's
  /// declarations() so a local in this state gets no declaration either --
  /// one nothing writes and nothing reads has no fact left worth naming.
  std::unordered_set<int64_t> deadLocalStackDeltas;

  /// Call op index -> the vtable slot it was confirmed to dispatch through
  /// (see analysis::findConfirmedVtableCalls). A computed call the analysis
  /// did not confirm as a vtable dispatch -- an ordinary function-pointer
  /// call, or an object seen at only one slot -- has no entry here, and
  /// prints exactly as it always did; this only ever adds a comment
  /// (StmtPrinter::printCall), never changes what the call itself casts to,
  /// since neither the object's struct layout nor its class name is known.
  std::map<uint32_t, analysis::VtableCallSite> vtableCalls;

  /// The temporary standing for a value, or nullptr when the value has none.
  [[nodiscard]] const std::string* tempFor(il::ValueId value) const;

  /// Records that the body called this syscall, so the preamble declares it and
  /// whatever aggregate tags its signature mentions.
  void useSyscall(const types::SyscallInfo& info);

  // -- imported types ---------------------------------------------------------

  /// What headers said about the addresses in this function, or null when no
  /// header was imported. Everything type-import changes goes through here.
  [[nodiscard]] const types::TypeBinder* binder() const noexcept {
    return binder_ ? &*binder_ : nullptr;
  }

  /// The imported prototype for the function being printed, when a header
  /// declared the symbol its entry block starts at.
  [[nodiscard]] const types::TypeEntry* prototype() const;

  /// The imported prototype for what a call's target expression calls: a
  /// symbol at a constant address, or the function a parameter's declared
  /// pointer type points at. Null when nothing says.
  ///
  /// Kept in step with passes/apply_types.cpp, which asks the same question of
  /// the same two shapes -- there it decides how many arguments the call has,
  /// here how the callee is spelled, and disagreeing would print a cast that
  /// does not match the argument list beside it.
  [[nodiscard]] const types::TypeEntry* calleeType(il::ExprId target) const;

  /// A memory access as a field of a declared struct: `node->left` for the
  /// eight-byte read at `node + 8` that the machine code actually performs.
  /// Empty when nothing licenses that spelling, and then the access prints as
  /// the cast it always did.
  ///
  /// Only through a parameter whose imported type is a pointer to a complete
  /// struct, and only on an exact offset-and-width match. Everything weaker is
  /// a guess: an offset that lands mid-field means the code is reading
  /// something this struct does not describe -- a different version of it, or
  /// not this struct at all -- and printing the nearest field name would turn
  /// that discrepancy into a plausible-looking lie.
  [[nodiscard]] std::string fieldAccess(il::ExprId address, uint32_t width,
                                        il::ValueId result);

  /// The imported type a value turned out to hold, when reading it through a
  /// declared struct said so: `t2 = node->left` makes `t2` an `EvalNode*`, and
  /// that is what lets the next access through it be a field too.
  ///
  /// Recorded while the body prints and read afterwards, which is sound only
  /// because the body prints before the declarations that consume this (see
  /// Assembler::run) and because a value is printed at its definition before
  /// any use of it.
  [[nodiscard]] types::TypeId typeOfValue(il::ValueId value) const;
  [[nodiscard]] bool valueIsPointer(il::ValueId value) const;

  /// The C spelling of an imported type, recording it as one the output has to
  /// define or forward-declare.
  [[nodiscard]] std::string spell(types::TypeId id);
  /// The same, as a declaration of `name` (`EvalNode* p`, `int (*f)(void)`).
  [[nodiscard]] std::string spellDeclaration(types::TypeId id, std::string_view name);

  /// Imported types the output mentioned, in first-use order, for the preamble
  /// to define. Recorded rather than dumped wholesale because a preset header
  /// declares thousands of types and this function used four.
  std::vector<types::TypeId> usedTypes;

  /// An argument variable's position in the convention's register order, read
  /// back off the name variable recovery gave it (`a3`), or -1.
  [[nodiscard]] static int argumentPosition(const analysis::Variable& variable);

  /// What to call one recovered argument: the name the prototype gave that
  /// position, when a header named it, and inference's `aN` otherwise. Both
  /// the signature and every use in the body go through here, or the body
  /// would read variables the signature never declared.
  [[nodiscard]] std::string argumentName(const analysis::Variable& variable) const;

  /// What to call parameter `position` (0-based) when nothing else names it:
  /// a header's own parameter name, and inference's own name for a Variable
  /// the body actually reads, both outrank this everywhere they apply (see
  /// `argumentName`). Only the two leftover spots reach here directly --
  /// c_printer.cpp's own "position nothing in the body reads" and "header
  /// parameter with no name of its own" cases -- so both read the same
  /// `COptions::indexedArgumentNames` choice `argumentName` does.
  [[nodiscard]] std::string positionalArgumentName(int position) const;

  /// Whether an argument is declared as a pointer, so that arithmetic on it
  /// has to cast back to an integer first. True when inference found one, and
  /// also when only the header says so -- the declaration is what the C
  /// compiler will see, and it is the declaration that makes `v + 4` scale.
  [[nodiscard]] bool argumentIsPointer(const analysis::Variable& variable) const;

  /// The imported type declared for an argument position, invalid when no
  /// prototype covers it.
  [[nodiscard]] types::TypeId argumentType(const analysis::Variable& variable) const;

  /// What this function itself returns, when every `ret` traced to the same
  /// call/`svc` result and that result's callee typed it (see
  /// analysis::TypedVariables::returnType) -- for a function no header names,
  /// so `signature()` has nothing else to prefer over the inferred width.
  /// Filtered through `consistent` against the inferred CType the same way an
  /// argument or local is, so a header's return type never overrides one the
  /// body's own operations contradict. Invalid when no evidence exists, no
  /// header was imported, or the two disagree.
  [[nodiscard]] types::TypeId functionReturnType() const;

  /// `&var_10` for an address expression that classifies as a stack slot
  /// (see StackFrame::classify) with a recovered local, empty otherwise --
  /// including for a Global or an Other address, which this never guesses
  /// at (see AddressRenderer, whose AddressOf/StackSlot branch this is a
  /// thin wrapper over).
  ///
  /// Checked ahead of the ordinary cast-and-print path (see StmtPrinter's
  /// call and syscall argument loops) because those wrap every argument text
  /// in the callee's own spelling: without this, `&var_10` would print as
  /// `(struct timeval*)(__entry_sp + -0x10)` -- not wrong, since the slot is
  /// declared as that struct, but a needless trip through arithmetic and a
  /// cast for what is now just one named local's address.
  [[nodiscard]] std::string addressOfLocal(il::ExprId address);

  /// `var_50.tv_sec` for a `width`-bit access to the stack slot at `delta`,
  /// when that slot is (or aliases into, see analysis::Variable::aliasBase) a
  /// struct TypedVariables typed and the access lands on one field exactly.
  /// Empty otherwise -- including a struct's own base slot read at some other
  /// width than any single field, which is memoryLvalue's cast fallback to
  /// handle, the same as it would for a local with no imported type at all.
  [[nodiscard]] std::string localFieldAccess(int64_t delta, uint32_t width) const;

  /// The lvalue text for a `width`-bit StackSlot access at `delta`: a named
  /// local, its aliased struct field, or a cast dereference of the local's
  /// address -- whichever `memoryLvalue`'s own StackSlot branch would have
  /// printed. Empty when no local was recovered there. Factored out so the
  /// constructor's stack-load-fold prepass can compute the identical text a
  /// folded load's one use should read instead of a temporary (see
  /// `inlinedStackLoads`).
  [[nodiscard]] std::string stackSlotLvalue(int64_t delta, uint32_t width) const;

  /// Names, and on first sight declares, the variable for a full register.
  [[nodiscard]] const RegVar& registerVariable(il::RegId root);

  /// What the symbol table says about `va`; not-found when no resolver is wired.
  [[nodiscard]] SymbolRef symbolAt(uint64_t va) const {
    return options.symbols ? options.symbols(va) : SymbolRef{};
  }

  /// The name to call the function at `va` by: its symbol's, when a symbol
  /// starts exactly there; the import behind a PLT stub's GOT indirection
  /// (see `options.names`), when nothing else names it; `sub_<va>` otherwise.
  /// A symbol that merely *covers* the address names a different function, so
  /// it is not used — calling `foo` when the target is halfway inside `foo`
  /// would be a lie of exactly the kind that makes decompiler output
  /// untrustworthy.
  [[nodiscard]] std::string calleeName(uint64_t va) const {
    const SymbolRef symbol = symbolAt(va);
    if (symbol.exact() && symbol.isFunction) {
      return symbol.name;
    }
    if (options.names) {
      if (const types::BoundName named = options.names(va); !named.empty() && named.isFunction) {
        return named.name;
      }
    }
    return std::format("sub_{:x}", va);
  }

  /// The name to print for the fixed address `va`, registering it for
  /// declaration, or nullptr when the image says nothing about it.
  ///
  /// Nothing known means nothing claimed: an address the pipeline cannot place is
  /// printed as the number it is, which is what every address did before. That is
  /// also what emission without an image gets, so no output depends on having
  /// one.
  [[nodiscard]] const std::string* globalName(uint64_t va) {
    if (const auto found = globals.find(va); found != globals.end()) {
      return &found->second.name;
    }
    if (!options.addresses) {
      return nullptr;
    }
    AddressFacts facts = options.addresses(va);
    if (!facts.variable) {
      return nullptr;
    }
    GlobalVar global;
    // A symbol starting exactly here names this object. One merely covering the
    // address names a larger object this is a field of, and borrowing its name
    // for the field would be wrong in the way that matters -- the reader would
    // take two different addresses for the same thing.
    const SymbolRef symbol = symbolAt(va);
    global.fromSymbol = symbol.exact() && !symbol.isFunction;
    if (global.fromSymbol) {
      global.name = symbol.name;
    } else if (!facts.importName.empty()) {
      // A slot holding an import is not that import, so it does not get its name
      // unqualified: the reader needs to see a pointer being read.
      global.name = std::format("ptr_{}", facts.importName);
      global.fromSymbol = true;
    } else {
      global.name = std::format("g_{:x}", va);
    }
    global.facts = std::move(facts);
    return &globals.emplace(va, std::move(global)).first->second.name;
  }

 private:
  std::optional<types::TypeBinder> binder_;
};

void appendLine(std::string& out, unsigned indent, std::string_view text);

}  // namespace xdec::emit
