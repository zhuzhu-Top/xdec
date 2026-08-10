// See the header for the shape this looks for and why.
#include "xdec/analysis/dispatcher_shape.h"

#include <algorithm>
#include <map>

namespace xdec::analysis {

namespace {
// A shape this loose would fire on coincidence -- three cases that happen to
// jump to the same place is not a dispatcher's shared tail. This high, only a
// real compiler- or obfuscator-generated epilogue clears it.
constexpr double kMergeSupportThreshold = 0.8;
}  // namespace

std::optional<DispatcherShape> matchDispatcherShape(const il::Function& function,
                                                     il::BlockId dispatch,
                                                     std::span<const il::BlockId> targets) {
  if (targets.size() < 3) {
    return std::nullopt;  // not worth a shared-tail search below a real dispatch
  }
  std::map<il::BlockId, unsigned> votes;
  for (const il::BlockId handler : targets) {
    const il::Block& block = function.block(handler);
    if (block.predecessors.size() != 1 || block.predecessors.front() != dispatch) {
      continue;  // reached from elsewhere too: not a private handler either way
    }
    if (block.successors.size() != 1) {
      continue;  // returns outright, or branches on its own -- not this shape
    }
    ++votes[block.successors.front()];
  }
  if (votes.empty()) {
    return std::nullopt;
  }
  const auto best = std::max_element(
      votes.begin(), votes.end(),
      [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });
  if (static_cast<double>(best->second) <
      kMergeSupportThreshold * static_cast<double>(targets.size())) {
    return std::nullopt;
  }
  const il::BlockId merge = best->first;
  if (merge == dispatch) {
    return std::nullopt;
  }
  // The tail must itself end in one plain jump -- that jump is the loop's
  // back edge, and a tail with a branch of its own would mean the "shared
  // epilogue" is actually still deciding something, which is not this shape.
  const il::Block& mergeBlock = function.block(merge);
  if (mergeBlock.successors.size() != 1) {
    return std::nullopt;
  }
  const il::BlockId hub = mergeBlock.successors.front();
  if (hub == merge || hub == dispatch) {
    return std::nullopt;
  }
  return DispatcherShape{dispatch, merge, hub};
}

}  // namespace xdec::analysis
