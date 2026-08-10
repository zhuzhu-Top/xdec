// reachableBlocks (see the header for why this exists alongside Dominators).
#include "xdec/analysis/reachability.h"

#include <vector>

namespace xdec::analysis {

std::unordered_set<il::BlockId> reachableBlocks(const il::Function& function) {
  std::unordered_set<il::BlockId> visited;
  if (!function.hasBlock(function.entryBlock())) {
    return visited;
  }
  std::vector<il::BlockId> stack{function.entryBlock()};
  visited.insert(function.entryBlock());
  while (!stack.empty()) {
    const il::BlockId current = stack.back();
    stack.pop_back();
    for (const il::BlockId successor : function.block(current).successors) {
      if (visited.insert(successor).second) {
        stack.push_back(successor);
      }
    }
  }
  return visited;
}

}  // namespace xdec::analysis
