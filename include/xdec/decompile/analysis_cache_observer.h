// Bridges pass::Manager's per-pass report to AnalysisCache::invalidate().
//
// AnalysisCache's own doc comment (see analysis/analysis_cache.h) is explicit
// that nothing wires this automatically: "xdec_pass has no dependency on
// xdec_analysis and this class deliberately does not force one". That
// boundary is real and stays real -- this header is the one place allowed to
// straddle it, because xdec_decompile already depends on both libraries.
//
// Today's single decompileToC() call (decompile/emit.h) never needs this: it
// builds a fresh AnalysisCache after the pass pipeline has already finished,
// so there is no earlier read for a later pass to invalidate. The caller this
// exists for is the one AnalysisCache's own header names -- an interactive
// session, or a multi-round structuring pass (see J2's own planning doc),
// that holds one cache across more than one Manager::run()/runTo() call
// against the same function.
#pragma once

#include "xdec/analysis/analysis_cache.h"
#include "xdec/pass/manager.h"

namespace xdec::decompile {

/// Forwards each pass's declared PassInfo::invalidates tags into `cache`
/// once that pass actually changes the function -- an unchanged pass leaves
/// every analysis it read still valid, so it is not asked to invalidate
/// anything. A pass that changes the function without declaring what it
/// invalidates is out of contract, so the fallback there is the same
/// invalidate() falls back to on its own -- drop everything -- because a
/// stale cache used to structure the function again is worse than a slow
/// one that recomputes.
class AnalysisCacheObserver final : public pass::Observer {
 public:
  explicit AnalysisCacheObserver(analysis::AnalysisCache& cache) noexcept : cache_(&cache) {}

  void afterPass(const pass::Pass& pass, const il::Function& function,
                const pass::RunStats& stats) override;

 private:
  analysis::AnalysisCache* cache_;
};

}  // namespace xdec::decompile
