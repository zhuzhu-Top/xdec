// The pass interface.
//
// A pass is the only way an IL function changes after lifting. Making every
// transformation go through one interface buys three things:
//
// 1. Provenance. The manager tags the function with the pass's identity before
//    running it, so every op ever printed can name the pass that created it.
// 2. Verification. A pass declares the maturity it consumes and the maturity it
//    produces; the manager checks the verifier's invariants for the produced
//    level after every run. A pass that silently breaks an invariant fails at
//    the boundary, with its name in the error, instead of twenty stages later
//    in emitted C.
// 3. Scheduling. Passes do not order themselves; the registry resolves a
//    pipeline from declared requirements, so a new pass slots in without
//    touching anyone else's code.
//
// The interface is deliberately small. A pass sees exactly one function and
// reports exactly one bit (did it change anything); everything else — looping,
// ordering, checking — is the framework's job, because that is the code that
// must be right exactly once.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "xdec/il/function.h"
#include "xdec/il/maturity.h"
#include "xdec/support/reader.h"
#include "xdec/support/result.h"

namespace xdec::types {
class SyscallTable;
class TypeDatabase;
}  // namespace xdec::types

namespace xdec::analysis {
class EntryRegFacts;
}  // namespace xdec::analysis

namespace xdec::pass {

/// What the image's symbol table says starts at an address.
///
/// Exact starts only, which is the same rule types::TypeBinder states and for
/// the same reason: a symbol that merely covers an address says where the code
/// is, not what it is. Declared here rather than reusing the binder's own
/// BoundName so that the pass framework keeps depending on nothing.
struct SymbolName {
  std::string name;
  bool isFunction = false;

  [[nodiscard]] bool empty() const noexcept { return name.empty(); }
};

using NameAt = std::function<SymbolName(uint64_t va)>;

/// What an indirect branch proved about itself while failing to resolve.
///
/// `targets` is the whole candidate set — the same set the branch would have
/// resolved to had the blocks behind it existed — and `missing` is the part of
/// it the lifter has never reached. Both, because the driver wants both and
/// they answer different questions: `missing` is what to lift next, `targets`
/// is the edge to put back once it has been. Reporting only the first costs a
/// round per level of a dispatcher chain, because a block lifted with no
/// predecessor has no incoming dataflow and so cannot resolve its own branch
/// (see decompile/driver.h).
struct Discovery {
  uint64_t branch = 0;
  std::vector<uint64_t> targets;
  std::vector<uint64_t> missing;
};

class Pass;

/// What a pass is, without running it. The registry schedules purely on this
/// metadata, so it must describe the pass completely and honestly; the
/// verifier enforces the level claims, and dishonest requirements show up as
/// pipeline resolution errors rather than wrong output.
struct PassInfo {
  /// Stable identifier, also used for op provenance. Lowercase-with-dashes by
  /// convention ("const-prop", "flatten-resolve").
  std::string name;

  /// The maturity the function must be at when the pass runs.
  il::Maturity level = il::Maturity::Lifted;

  /// The maturity the function is at when the pass finishes. Greater than or
  /// equal to `level`; a same-level transform is the common case.
  il::Maturity produces = il::Maturity::Lifted;

  /// Passes that must have run (at any earlier point in the pipeline) before
  /// this one. Names, because ordering by identity keeps the graph explicit;
  /// a typo is a resolution error, not a misordered run. (Named
  /// `requirements`, not `requires` — the latter is a keyword.)
  std::vector<std::string> requirements;

  /// Advisory: the analyses/results these passes produced are no longer valid
  /// after this pass runs. The framework records it; the analysis cache
  /// (P7) is what will act on it.
  std::vector<std::string> invalidates;

  /// Repeat the pass until it reports no change. The manager bounds the loop
  /// and a pass that never converges is an error, not a hang.
  bool fixpoint = false;
};

/// The one thing a pass can see. Deliberately a narrow view: passes that need
/// more context (the binary image, options) get it here, which keeps every
/// pass's dependencies grep-able from one struct.
class Context {
 public:
  explicit Context(il::Function& function) noexcept : function_(&function) {}

  [[nodiscard]] il::Function& function() const noexcept { return *function_; }

  /// The binary image, for passes whose job is reading memory (jump-table
  /// resolution reads the table through it). Absent in pipelines that run
  /// without an image; a pass that needs one must say so in its error, not
  /// silently do nothing.
  void setImage(ByteReader reader) { image_ = std::move(reader); }
  [[nodiscard]] const ByteReader* image() const noexcept {
    return image_.has_value() ? &*image_ : nullptr;
  }

  /// What the pipeline knows about that image beyond its bytes: which ranges
  /// never change, which ones hold code (see MemoryFacts). Always answerable —
  /// an unwired pipeline reports "unknown" for everything, which every asker
  /// must already handle.
  void setMemoryFacts(MemoryFacts facts) { memory_ = std::move(facts); }
  [[nodiscard]] const MemoryFacts& memoryFacts() const noexcept { return memory_; }

  /// What the user told the pipeline about the world outside the binary:
  /// declarations imported from headers, and the kernel's syscall numbering.
  /// Both are absent by default and every pass that reads one must work
  /// without it — a decompilation with no `--types` is the normal case, not a
  /// degraded one, so a pass may only *add* to what it would have said.
  ///
  /// Held as pointers to objects the caller owns for the pipeline's lifetime.
  /// Forward-declared rather than included: the pass framework has no business
  /// depending on the type system to pass a pointer through.
  void setTypeDatabase(const types::TypeDatabase* database) noexcept {
    types_ = database;
  }
  [[nodiscard]] const types::TypeDatabase* typeDatabase() const noexcept { return types_; }

  void setSyscallTable(const types::SyscallTable* table) noexcept { syscalls_ = table; }
  [[nodiscard]] const types::SyscallTable* syscallTable() const noexcept { return syscalls_; }

  /// What the platform (not the image) says a leaked entry register holds --
  /// see analysis/entry_reg.h. Absent is the default and costs nothing: a
  /// pass that reads it (resolve-indirect, through analysis::ImageEval) finds
  /// every EntryReg leaf top, exactly as before this existed.
  void setEntryRegFacts(const analysis::EntryRegFacts* facts) noexcept {
    entryRegs_ = facts;
  }
  [[nodiscard]] const analysis::EntryRegFacts* entryRegFacts() const noexcept {
    return entryRegs_;
  }

  /// The image's symbol table, for the one thing a pass can do with a name that
  /// it cannot do with an address: look up what a header declared under it.
  /// Unset resolves nothing, which is what a pipeline with no image gets.
  void setNames(NameAt names) { names_ = std::move(names); }
  [[nodiscard]] SymbolName nameAt(uint64_t va) const {
    return names_ ? names_(va) : SymbolName{};
  }

  /// What to do with a computed branch that resolution could not answer.
  ///
  /// Failing is the default and the honest one: an unresolved branch at
  /// Resolved is a hole in the CFG, and every analysis above it would be
  /// reasoning about a function it cannot see all of. But on a mega-dispatcher
  /// the hole is a few hundred branches out of a few hundred blocks that *were*
  /// understood, and handing back nothing at all serves nobody -- the same
  /// argument the driver makes for its round cap. Sealing turns each such
  /// branch into an opaque terminator carrying the target expression it could
  /// not evaluate, which is legal at Resolved and true: control leaves here,
  /// and where it goes is not knowable from the image alone.
  ///
  /// Off unless a caller asks, so nothing quietly downgrades an answer.
  void setSealUnresolvedBranches(bool seal) noexcept { seal_ = seal; }
  [[nodiscard]] bool sealUnresolvedBranches() const noexcept { return seal_; }

  /// Out-channel for passes that find code the lifter never reached: an
  /// indirect target with no block is a discovery, and the orchestrating
  /// driver (decompile/driver.h) listens here to lift it on demand. Passes
  /// call reportDiscovery, never the sink directly.
  void setDiscoverySink(std::function<void(const Discovery&)> sink) {
    discoverySink_ = std::move(sink);
  }
  void reportDiscovery(const Discovery& discovery) {
    if (discoverySink_) {
      discoverySink_(discovery);
    }
  }

 private:
  il::Function* function_;
  std::optional<ByteReader> image_;
  MemoryFacts memory_;
  const types::TypeDatabase* types_ = nullptr;
  const types::SyscallTable* syscalls_ = nullptr;
  NameAt names_;
  bool seal_ = false;
  std::function<void(const Discovery&)> discoverySink_;
  const analysis::EntryRegFacts* entryRegs_ = nullptr;
};

class Pass {
 public:
  virtual ~Pass() = default;

  [[nodiscard]] virtual const PassInfo& info() const noexcept = 0;

  /// Transforms the function. Returns true when anything changed; the fixpoint
  /// scheduler and the change statistics both depend on this being truthful,
  /// and a pass that reports falsely in either direction is a bug (a false
  /// "unchanged" stops convergence early, a false "changed" never stops).
  [[nodiscard]] virtual Result<bool> run(Context& context) = 0;
};

/// Base class storing the info block, so concrete passes declare it once as a
/// member initialiser and write only `run`.
class FunctionPass : public Pass {
 public:
  explicit FunctionPass(PassInfo info) noexcept : info_(std::move(info)) {}

  [[nodiscard]] const PassInfo& info() const noexcept override { return info_; }

 private:
  PassInfo info_;
};

}  // namespace xdec::pass
