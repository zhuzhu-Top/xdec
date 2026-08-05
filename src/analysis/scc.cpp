// Iterative Tarjan SCC over the reachable CFG.
#include "xdec/analysis/scc.h"

#include <algorithm>

namespace xdec::analysis {

Sccs Sccs::compute(const il::Function& function) {
  Sccs result;
  const std::size_t n = function.blockCount();
  result.membership_.assign(n, il::BlockId::kInvalidIndex);

  const il::BlockId entry = function.entryBlock();
  if (!entry.valid() || entry.asSize() >= n) {
    return result;
  }

  constexpr uint32_t kUnvisited = il::BlockId::kInvalidIndex;
  std::vector<uint32_t> index(n, kUnvisited);
  std::vector<uint32_t> lowlink(n, 0);
  std::vector<bool> onStack(n, false);
  std::vector<il::BlockId> tarjanStack;
  uint32_t nextIndex = 0;

  // Explicit DFS stack: (block, next successor to scan). Recursion depth here
  // would be the longest acyclic chain — precisely what obfuscators inflate.
  std::vector<std::pair<il::BlockId, std::size_t>> dfs;
  const auto push = [&](il::BlockId block) {
    index[block.asSize()] = nextIndex;
    lowlink[block.asSize()] = nextIndex;
    ++nextIndex;
    tarjanStack.push_back(block);
    onStack[block.asSize()] = true;
    dfs.emplace_back(block, 0);
  };
  push(entry);

  while (!dfs.empty()) {
    auto& [block, nextSucc] = dfs.back();
    const auto& successors = function.block(block).successors;
    if (nextSucc < successors.size()) {
      const il::BlockId succ = successors[nextSucc++];
      if (index[succ.asSize()] == kUnvisited) {
        push(succ);
      } else if (onStack[succ.asSize()]) {
        lowlink[block.asSize()] =
            std::min(lowlink[block.asSize()], index[succ.asSize()]);
      }
      continue;
    }

    // Done with `block`. Root of a component pops the stack through itself.
    dfs.pop_back();
    if (lowlink[block.asSize()] == index[block.asSize()]) {
      Component component;
      il::BlockId member;
      do {
        member = tarjanStack.back();
        tarjanStack.pop_back();
        onStack[member.asSize()] = false;
        component.blocks.push_back(member);
      } while (member != block);
      // A singleton is cyclic only when it loops to itself.
      if (component.blocks.size() == 1) {
        const auto& selfSuccs = function.block(component.blocks.front()).successors;
        component.cyclic = std::find(selfSuccs.begin(), selfSuccs.end(),
                                     component.blocks.front()) != selfSuccs.end();
      } else {
        component.cyclic = true;
      }
      for (const il::BlockId b : component.blocks) {
        result.membership_[b.asSize()] = static_cast<uint32_t>(result.components_.size());
      }
      result.components_.push_back(std::move(component));
    }
    // Propagate the lowlink to the parent now on top of the DFS stack.
    if (!dfs.empty()) {
      const il::BlockId parent = dfs.back().first;
      lowlink[parent.asSize()] =
          std::min(lowlink[parent.asSize()], lowlink[block.asSize()]);
    }
  }
  return result;
}

uint32_t Sccs::componentOf(il::BlockId block) const noexcept {
  return block.asSize() < membership_.size() ? membership_[block.asSize()]
                                             : il::BlockId::kInvalidIndex;
}

}  // namespace xdec::analysis
