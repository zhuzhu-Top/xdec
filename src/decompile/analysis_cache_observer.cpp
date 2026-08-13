#include "xdec/decompile/analysis_cache_observer.h"

#include <string_view>
#include <vector>

namespace xdec::decompile {

void AnalysisCacheObserver::afterPass(const pass::Pass& pass, const il::Function& function,
                                      const pass::RunStats& stats) {
  (void)function;
  if (!stats.changed) {
    return;
  }
  const std::vector<std::string>& invalidates = pass.info().invalidates;
  const std::vector<std::string_view> tags(invalidates.begin(), invalidates.end());
  cache_->invalidate(tags);
}

}  // namespace xdec::decompile
