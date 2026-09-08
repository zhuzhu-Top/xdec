// PathExplorer: a bounded, path-sensitive forward walk of one function,
// looking for what analysis::ImageEval's whole-function value sets and
// analysis/index_bound.h's structural proofs cannot see -- see
// docs/23-path-eval.md for the full picture and passes/resolve_indirect.cpp
// for where this plugs in as a third, last-resort candidate source.
//
// The two things a single concrete path knows that a whole-function analysis
// does not:
//
//   - A phi resolves to the value on the *one* edge this path took, not the
//     union of every edge (PathState::bindPhi). A canary check's two arms
//     can leave a merged register very different values, and ImageEval's
//     union answers "both are possible here", which is true of the function
//     but not of either arm alone.
//   - A load may read a value this path's own Store put there
//     (PathState::recordStore/loadFrom), not just the image's bytes. By the
//     time resolve-indirect runs, ssa-optimize and stack-prop have already
//     turned every *provably* local store/load round trip into a plain SSA
//     value -- so a Store/Load pair still standing here is exactly the case
//     those passes could not settle without knowing which edge got taken,
//     which is this class's whole reason to exist.
//
// What this does not attempt: interprocedural reasoning (a Call's effect on
// memory or registers is never modelled -- see PathInterpreter), and no SMT.
// Both are real gaps for some obfuscated dispatchers (see docs/22-dyld-
// shared-cache.md's own sealed branch), left as future work rather than
// guessed at.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "xdec/analysis/entry_reg.h"
#include "xdec/il/function.h"
#include "xdec/support/reader.h"

namespace xdec::analysis {

struct PathExploreOptions {
  /// Off makes PathExplorer::explore() a no-op and targetsFor() always
  /// nullopt -- the same "absent costs nothing" contract every other
  /// optional fact in this pipeline has (see analysis/entry_reg.h).
  bool enabled = true;
  /// Off (the default) skips whichever arm of a stack-canary check looks
  /// structurally like the mismatch path (see isLikelyCanaryFailArm in the
  /// .cpp) -- that arm runs only after undefined behaviour has already
  /// started, so a target only reachable through it is not part of the
  /// program's real control flow. On explores both arms regardless, for a
  /// caller specifically investigating that path (as docs/22's own sealed
  /// branch discussion does).
  bool exploreCanaryFailPaths = false;
  /// Hard cap on how many block-steps the whole exploration may spend across
  /// every forked continuation combined -- the safety backstop against a
  /// function with many independent conditionals (see the "structural
  /// argument deserves a hard backstop anyway" reasoning decompile/driver.h
  /// already uses for its own round cap).
  unsigned maxPaths = 64;
  /// Per-continuation bound: a single walk from the entry that has taken
  /// this many block-steps is abandoned, regardless of the shared budget
  /// above. Guards against one pathological continuation spending the whole
  /// budget on a function no real dispatcher looks like.
  unsigned maxStepsPerPath = 4096;
  /// A continuation revisiting the same block more than this many times is
  /// dropped -- the loop-unrolling bound this exploration is willing to pay
  /// for rather than follow a natural loop indefinitely.
  unsigned maxBlockRevisits = 2;
};

/// One bounded, path-sensitive walk of `function` from its entry, run once
/// (see explore()) and queried per branch afterwards.
class PathExplorer {
 public:
  PathExplorer(const il::Function& function, ByteReader reader, const EntryRegFacts* entryRegs,
              PathExploreOptions options);

  /// Runs the walk. Idempotent and lazy in spirit -- callers on the common
  /// case (tableCandidates or valueSetCandidates already answered every
  /// branch) never need to call this at all, so resolve-indirect only does
  /// on the first branch that actually needs it.
  void explore();

  /// The union of every target this walk's paths proved could reach
  /// `branchVa`, filtered exactly as valueSetCandidates is (zero is not a
  /// target, an unreadable address is retried through the shared-cache
  /// tagged-pointer decoder) -- so a caller can treat this like one more
  /// value-set source. Nullopt when no explored path resolved a finite
  /// target set for this branch, indistinguishable from "resolve() was never
  /// called" only in that both mean the same thing to a caller: nothing to
  /// add.
  [[nodiscard]] const std::vector<uint64_t>* targetsFor(uint64_t branchVa) const;

  [[nodiscard]] unsigned pathsExplored() const noexcept { return pathsExplored_; }

 private:
  void recordIndirectTarget(uint64_t branchVa, const class SymValueSet& targetSet);
  [[nodiscard]] bool readable(uint64_t va) const;

  const il::Function* function_;
  ByteReader reader_;
  const EntryRegFacts* entryRegs_;
  PathExploreOptions options_;
  bool explored_ = false;
  unsigned pathsExplored_ = 0;
  std::unordered_map<uint64_t, std::vector<uint64_t>> results_;
  /// A canary check's OpId index -> the successor BlockId that looks like
  /// its mismatch arm (see isLikelyCanaryFailArm). Absent entries mean no
  /// arm looked distinguishable enough to call -- both stay explorable.
  std::unordered_map<uint32_t, il::BlockId> canaryFailSuccessor_;
};

}  // namespace xdec::analysis
