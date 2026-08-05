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
#include <utility>
#include <vector>

#include "xdec/analysis/stack_frame.h"
#include "xdec/analysis/variables.h"
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

class CContext {
 public:
  CContext(const il::Function& theFunction,
           const analysis::VariableTable& theVariables,
           const analysis::StackFrame& theFrame,
           const StructuredFunction& theStructured, const COptions& theOptions)
      : function(theFunction),
        variables(theVariables),
        frame(theFrame),
        structured(theStructured),
        options(theOptions) {
    if (options.types != nullptr) {
      binder_.emplace(*options.types, [this](uint64_t va) {
        const SymbolRef symbol = symbolAt(va);
        return symbol.exact()
                   ? types::BoundName{symbol.name, symbol.isFunction}
                   : types::BoundName{};
      });
    }
  }

  const il::Function& function;
  const analysis::VariableTable& variables;
  const analysis::StackFrame& frame;
  const StructuredFunction& structured;
  const COptions& options;

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

  /// Whether an argument is declared as a pointer, so that arithmetic on it
  /// has to cast back to an integer first. True when inference found one, and
  /// also when only the header says so -- the declaration is what the C
  /// compiler will see, and it is the declaration that makes `v + 4` scale.
  [[nodiscard]] bool argumentIsPointer(const analysis::Variable& variable) const;

  /// The imported type declared for an argument position, invalid when no
  /// prototype covers it.
  [[nodiscard]] types::TypeId argumentType(const analysis::Variable& variable) const;

  /// Names, and on first sight declares, the variable for a full register.
  [[nodiscard]] const RegVar& registerVariable(il::RegId root);

  /// What the symbol table says about `va`; not-found when no resolver is wired.
  [[nodiscard]] SymbolRef symbolAt(uint64_t va) const {
    return options.symbols ? options.symbols(va) : SymbolRef{};
  }

  /// The name to call the function at `va` by: its symbol's, when a symbol
  /// starts exactly there, and `sub_<va>` otherwise. A symbol that merely
  /// *covers* the address names a different function, so it is not used —
  /// calling `foo` when the target is halfway inside `foo` would be a lie of
  /// exactly the kind that makes decompiler output untrustworthy.
  [[nodiscard]] std::string calleeName(uint64_t va) const {
    const SymbolRef symbol = symbolAt(va);
    return symbol.exact() && symbol.isFunction ? symbol.name
                                               : std::format("sub_{:x}", va);
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
