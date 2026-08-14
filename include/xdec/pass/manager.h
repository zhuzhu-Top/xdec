// Pipeline execution.
//
// The manager is the only code that runs passes, and it wraps every pass in
// the same protocol:
//
//   before: check the function sits at the pass's declared level, tag the
//           function with the pass's identity for provenance, notify the
//           observer;
//   run:    the pass, once or to its fixpoint (bounded — a non-converging
//           pass is reported, not looped on);
//   after:  verify the IL against the pass's produced maturity, advance the
//           function's recorded maturity, notify the observer with stats.
//
// A failure anywhere stops the pipeline and names the pass. "Which pass broke
// the invariants" is the single most common debugging question in a pass
// pipeline, and the design goal here is that the answer is always in the
// error message, never in a bisection session.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "xdec/il/function.h"
#include "xdec/pass/pass.h"
#include "xdec/pass/registry.h"
#include "xdec/support/reader.h"
#include "xdec/support/result.h"

namespace xdec::pass {

/// What happened in one pass's slot of the pipeline, reported to observers and
/// returned to the caller.
struct RunStats {
  std::string passName;
  /// How many times the pass's run() executed: one for a plain pass, up to the
  /// convergence limit for a fixpoint one.
  unsigned iterations = 0;
  /// Whether any iteration changed the function.
  bool changed = false;
};

/// Observation hooks. Observers must not mutate the function; the dump/diff
/// observer (P6d) and the CLI's progress reporting both hang off this.
class Observer {
 public:
  virtual ~Observer() = default;

  virtual void beforePass(const Pass& pass, const il::Function& function) {
    (void)pass;
    (void)function;
  }

  virtual void afterPass(const Pass& pass, const il::Function& function,
                         const RunStats& stats) {
    (void)pass;
    (void)function;
    (void)stats;
  }

  virtual void pipelineDone(const il::Function& function) { (void)function; }
};

class Manager {
 public:
  explicit Manager(Observer* observer = nullptr) noexcept : observer_(observer) {}

  /// Hands every pass's Context the binary image. Pipelines that include
  /// memory-reading passes (resolve-indirect) need this; pipelines of pure
  /// transforms never notice it.
  void setImage(ByteReader reader) { image_ = std::move(reader); }

  /// What is known about that image beyond its bytes (see
  /// Context::setMemoryFacts). A pipeline that sets the reader but not this
  /// still runs: passes that need a fact find it unknown and decline to act,
  /// which costs simplification and risks nothing.
  void setMemoryFacts(MemoryFacts facts) { memory_ = std::move(facts); }

  /// External knowledge handed to every pass's Context: types imported from
  /// headers, and the kernel's syscall numbering (see Context::setTypeDatabase).
  /// Both default to absent, and both objects must outlive the pipeline.
  void setTypeDatabase(const types::TypeDatabase* database) noexcept {
    types_ = database;
  }
  void setSyscallTable(const types::SyscallTable* table) noexcept { syscalls_ = table; }

  /// See Context::setEntryRegFacts. Must outlive the pipeline, same as
  /// types/syscalls above.
  void setEntryRegFacts(const analysis::EntryRegFacts* facts) noexcept {
    entryRegs_ = facts;
  }

  /// The image's symbol table, as passes ask about it (see Context::setNames).
  void setNames(NameAt names) { names_ = std::move(names); }

  /// What resolution does with a branch it cannot answer (see
  /// Context::setSealUnresolvedBranches).
  void setSealUnresolvedBranches(bool seal) noexcept { seal_ = seal; }

  /// The discovery sink every pass's Context reports newly found code into
  /// (see Context::setDiscoverySink).
  void setDiscoverySink(std::function<void(const Discovery&)> sink) {
    discoverySink_ = std::move(sink);
  }

  /// Runs `pipeline` in order. The span borrows from the caller, typically a
  /// Registry::resolve result.
  [[nodiscard]] Result<std::vector<RunStats>> run(
      il::Function& function, std::span<Pass* const> pipeline) const;

  /// resolve() + run(): take the function from its current maturity to
  /// `target` using whatever the registry offers.
  [[nodiscard]] Result<std::vector<RunStats>> runTo(il::Function& function,
                                                    const Registry& registry,
                                                    il::Maturity target) const;

  /// Hard bound for fixpoint loops. A pass needing more iterations than this
  /// is not converging slowly, it is oscillating, and oscillation is a bug in
  /// the pass, not in the bound.
  static constexpr unsigned kFixpointLimit = 64;

 private:
  Observer* observer_;
  std::optional<ByteReader> image_;
  MemoryFacts memory_;
  const types::TypeDatabase* types_ = nullptr;
  const types::SyscallTable* syscalls_ = nullptr;
  NameAt names_;
  bool seal_ = false;
  std::function<void(const Discovery&)> discoverySink_;
  const analysis::EntryRegFacts* entryRegs_ = nullptr;
};

}  // namespace xdec::pass
