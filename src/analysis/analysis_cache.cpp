#include "xdec/analysis/analysis_cache.h"

#include <algorithm>

namespace xdec::analysis {

namespace {
[[nodiscard]] bool namesAnyOf(std::span<const std::string_view> tags, std::string_view a,
                              std::string_view b) {
  return std::any_of(tags.begin(), tags.end(), [&](std::string_view tag) { return tag == a || tag == b; });
}
}  // namespace

const Dominators& AnalysisCache::dominators() const {
  if (!dominators_) {
    dominators_ = Dominators::compute(*function_);
    ++stats_.dominatorsComputed;
  }
  return *dominators_;
}

const PostDominators& AnalysisCache::postDominators() const {
  if (!postDominators_) {
    postDominators_ = PostDominators::compute(*function_);
    ++stats_.postDominatorsComputed;
  }
  return *postDominators_;
}

const std::vector<NaturalLoop>& AnalysisCache::loops() const {
  if (!loops_) {
    loops_ = naturalLoops(*function_, dominators());
    ++stats_.loopsComputed;
  }
  return *loops_;
}

const StackFrame& AnalysisCache::stackFrame() const {
  if (!stackFrame_) {
    stackFrame_ = StackFrame::compute(*function_);
    ++stats_.stackFrameComputed;
  }
  return *stackFrame_;
}

const std::vector<DispatchRegion>& AnalysisCache::dispatchRegions() const {
  if (!dispatchRegions_) {
    dispatchRegions_ = findDispatchRegions(*function_);
    ++stats_.dispatchRegionsComputed;
  }
  return *dispatchRegions_;
}

void AnalysisCache::invalidate(std::span<const std::string_view> tags) {
  const bool all = tags.empty();
  if (all || namesAnyOf(tags, "cfg", "dominators")) {
    dominators_.reset();
    postDominators_.reset();
    loops_.reset();
  }
  if (all || namesAnyOf(tags, "cfg", "stack")) {
    stackFrame_.reset();
  }
  if (all || namesAnyOf(tags, "cfg", "dispatch")) {
    dispatchRegions_.reset();
  }
}

}  // namespace xdec::analysis
