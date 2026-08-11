// A quantified, IL-level snapshot of the redundant-intermediate-variable
// shapes the emit layer's own folding passes target -- see
// docs/09-expression-reuse.md's taxonomy (shapes F through J) and
// docs/14-emit-redundancy.md's framework overview.
//
// Deliberately IL-level, not text-level: this counts what stack_load_fold.h
// and stack_store_fold.h can prove from the IL and VariableTable alone,
// independent of how CContext or the emitter's own per-scope CSE end up
// spelling the result. That is the half a phase's own before/after is
// measured against here; the complementary text-level counts (`_cseN = ...`
// line counts, and the like, which are an emitted-scope fact these analyses
// do not have) come from counting the printed .c file instead (see
// tools/emit_metrics.ps1) -- the two are meant to be read side by side, not
// merged into one number.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "xdec/analysis/atomic_exclusive.h"
#include "xdec/analysis/load_inline.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/analysis/stack_load_fold.h"
#include "xdec/analysis/stack_store_fold.h"
#include "xdec/analysis/variables.h"
#include "xdec/il/function.h"

namespace xdec::analysis {

struct EmitRedundancyReport {
  /// Loads whose address classifies as a stack slot.
  std::size_t stackLoads = 0;
  /// Of those, how many findFoldableStackLoads resolves (shape F).
  std::size_t stackLoadsFolded = 0;
  /// Loads whose address classifies as Global or Other (an
  /// argument-plus-offset chain, most often, or a fixed image address).
  std::size_t memoryLoads = 0;
  /// Of those, how many findFoldableMemoryLoads resolves (shape G).
  std::size_t memoryLoadsFolded = 0;
  /// Stores whose address classifies as a stack slot.
  std::size_t stackStores = 0;
  /// Of those, how many findDeadStackStores resolves (shape H1).
  std::size_t stackStoresDead = 0;
  /// Recovered locals with zero Load readers anywhere in the function --
  /// write-only stack slots, the variable-level fact behind shape H1's
  /// dead stores. Not the same count as `stackStoresDead`: one such local
  /// can have several Stores, and this counts the local once.
  std::size_t writeOnlyLocals = 0;
  /// Registers a flattening dispatcher's hub/merge relay carries across the
  /// state machine (see analysis::LiveRegisterFrame) -- shape J. `nullopt`
  /// when the function has no resolved dispatcher shape at all (every
  /// non-flattened function, and most flattened ones outside their own
  /// dispatch loop); present-and-zero means a dispatcher exists but keeps
  /// nothing alive across it worth folding.
  std::optional<std::size_t> dispatcherRelaySlots;
  /// Of those, how many `unanimousPassthroughSlots` proves every handler
  /// that reaches the merge leaves unchanged -- the relay's save/restore
  /// pair is then contributing nothing `classifyHandlerExit`'s own per-case
  /// check would not already tell a reader. Shape J is listed here for
  /// completeness of the taxonomy, not as a fold this file performs: see
  /// docs/09-expression-reuse.md section J.
  std::optional<std::size_t> dispatcherRelayUnneededSlots;

  [[nodiscard]] std::string format() const;
};

/// Every CContext prescan gathered into one place, in the exact dependency
/// order CContext's own constructor always applied them in: each fold's
/// "does anything besides a dead op read this" check reads `deadOps` as it
/// stood after the folds before it, so the order below is load-bearing, not
/// stylistic (see docs/16 and the architecture plan's own risk note on this
/// constructor). A caller with nothing to add starts `seedDeadOps` empty;
/// CContext seeds it with `collectDeadOps`'s jump-table finds, which have to
/// be visible to every scan here, not just folded in afterwards.
struct EmitRedundancyPrep {
  std::unordered_set<uint32_t> deadOps;
  std::unordered_map<uint32_t, ExclusiveLoad> exclusiveLoads;
  std::unordered_map<uint32_t, ExclusiveStore> exclusiveStores;
  /// Every candidate findFoldableStackLoads names, whether or not
  /// `stackLoadFilter` (see below) accepts it -- a caller that rejects one
  /// must be able to tell it apart from one it applied, since only an
  /// applied fold's op is safe to treat as dead. Check `appliedStackLoads`
  /// for that distinction.
  std::unordered_map<uint32_t, FoldableStackLoad> foldableStackLoads;
  /// The subset of `foldableStackLoads`' keys `stackLoadFilter` accepted
  /// (or, with no filter supplied, every key) -- these, and only these, are
  /// the ones folded into `deadOps` before findFoldableMemoryLoads and
  /// findDeadStackStores run.
  std::unordered_set<uint32_t> appliedStackLoads;
  std::unordered_map<uint32_t, FoldableMemoryLoad> foldableMemoryLoads;
  std::unordered_set<uint32_t> deadStackStores;
  /// Stack deltas every Store in `deadStackStores` writes to (see
  /// findDeadStackStores: a delta with several dead stores is dead as a
  /// whole slot, never store-by-store).
  std::unordered_set<int64_t> deadLocalStackDeltas;
};

/// Decides whether a stack-load fold candidate is actually usable.
/// CContext's own answer is "stackSlotLvalue returned non-empty text",
/// something only the emit layer (which alone knows about typed
/// variables/the binder) can answer -- this analysis has no text to offer
/// an opinion on, so a caller with nothing better passes no filter at all,
/// which accepts every candidate `findFoldableStackLoads` names.
using StackLoadFilter = std::function<bool(uint32_t opIndex, const FoldableStackLoad&)>;

[[nodiscard]] EmitRedundancyPrep prepareEmitRedundancy(
    const il::Function& function, const StackFrame& frame, const VariableTable& variables,
    std::unordered_set<uint32_t> seedDeadOps = {}, const StackLoadFilter& stackLoadFilter = {});

[[nodiscard]] EmitRedundancyReport analyzeEmitRedundancy(const il::Function& function,
                                                         const StackFrame& frame,
                                                         const VariableTable& variables);

}  // namespace xdec::analysis
