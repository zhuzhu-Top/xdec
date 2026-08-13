// AnalysisCache: lazy, invalidatable storage for the CFG-derived analyses
// that get read more than once against the same, unchanging il::Function.
//
// Today's single decompileToC() call (xdec/decompile/emit.h) computes
// Dominators, PostDominators, natural loops and the StackFrame exactly once
// each, in order, and never revisits any of them -- there is nothing to
// cache yet on that path, and wiring this in there is a straight swap with
// no behaviour change (see renderToC()'s own use of it). The reason this
// exists anyway is every caller that is *not* that single sequential call:
// an interactive session holding a function across several queries, or a
// future `observe` that dumps more than one maturity checkpoint and would
// otherwise recompute dominators for each one. Both want "ask for it lazily,
// pay once", which is exactly this class's contract.
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "xdec/analysis/dispatch_region.h"
#include "xdec/analysis/dominators.h"
#include "xdec/analysis/loops.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/il/function.h"

namespace xdec::analysis {

/// How many times AnalysisCache actually ran each underlying analysis, as
/// opposed to how many times a caller *asked* for one -- the gap between the
/// two is exactly what caching buys. A benchmark or a regression test reads
/// this to confirm the cache did its job instead of quietly recomputing on
/// every access.
struct AnalysisCacheStats {
  unsigned dominatorsComputed = 0;
  unsigned postDominatorsComputed = 0;
  unsigned loopsComputed = 0;
  unsigned stackFrameComputed = 0;
  unsigned dispatchRegionsComputed = 0;
};

/// Binds to one Function for its whole lifetime -- a different function gets
/// its own cache, not a `retarget()`, so "which function does this
/// dominators tree describe" stays a type-level fact instead of a runtime
/// one a caller could get wrong.
///
/// This cache does not observe the function itself, so a pass rewriting it
/// in place is invisible to it: a caller that runs a pass reporting
/// pass::PassInfo::invalidates (see pass/pass.h) must call invalidate() with
/// those same tags afterwards, or a later accessor call hands back an
/// analysis of a CFG that no longer exists. Nothing here does that wiring
/// automatically -- xdec_pass has no dependency on xdec_analysis and this
/// class deliberately does not force one (see invalidate()'s own doc
/// comment for the tag vocabulary a caller bridges through).
class AnalysisCache {
 public:
  explicit AnalysisCache(const il::Function& function) noexcept : function_(&function) {}

  [[nodiscard]] const Dominators& dominators() const;
  [[nodiscard]] const PostDominators& postDominators() const;
  /// Depends on dominators() (see loops.h's naturalLoops), computed through
  /// this same cache so a caller who only ever asks for loops() still pays
  /// for dominators once, not twice.
  [[nodiscard]] const std::vector<NaturalLoop>& loops() const;
  [[nodiscard]] const StackFrame& stackFrame() const;
  /// See analysis::findDispatchRegions. Independent of dominators/loops --
  /// it reads jump-table and clamp shapes off the function's own
  /// expressions and edges, not off either tree -- so it is invalidated by
  /// "cfg" alone, not by "dominators".
  [[nodiscard]] const std::vector<DispatchRegion>& dispatchRegions() const;

  /// Drops whichever cached analyses `tags` names, so the next accessor call
  /// recomputes them from the function as it looks right now. Tags share
  /// pass::PassInfo::invalidates' own vocabulary ("cfg", "dominators", "scc"
  /// today -- see e.g. passes/cfg_finalize.cpp):
  ///
  ///   "cfg" or "dominators" -- dominators(), postDominators() and loops()
  ///                            (loops is derived from dominators, so it
  ///                            goes stale with it)
  ///   "cfg" or "stack"      -- stackFrame() ("stack" is reserved: no
  ///                            builtin pass declares it today, since every
  ///                            stack-affecting pass runs before the point
  ///                            StackFrame is ever computed)
  ///   "cfg" or "dispatch"   -- dispatchRegions() ("dispatch" is reserved
  ///                            the same way "stack" is: no builtin pass
  ///                            declares it today)
  ///
  /// An empty `tags` (the default) invalidates everything, for a caller that
  /// does not know or does not trust which specific tag applies.
  void invalidate(std::span<const std::string_view> tags = {});

  [[nodiscard]] const AnalysisCacheStats& stats() const noexcept { return stats_; }

 private:
  const il::Function* function_;
  mutable std::optional<Dominators> dominators_;
  mutable std::optional<PostDominators> postDominators_;
  mutable std::optional<std::vector<NaturalLoop>> loops_;
  mutable std::optional<StackFrame> stackFrame_;
  mutable std::optional<std::vector<DispatchRegion>> dispatchRegions_;
  mutable AnalysisCacheStats stats_;
};

}  // namespace xdec::analysis
