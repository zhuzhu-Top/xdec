// Strongly connected components of the reachable CFG, Tarjan's algorithm.
//
// SCCs are the unit cyclic structure is measured in: a loop is a cyclic SCC,
// reducibility is a property of SCC entries, and the structurer collapses
// SCCs. The implementation is iterative because a flattened function's block
// count is exactly where a recursive DFS would meet the stack limit.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "xdec/il/function.h"

namespace xdec::analysis {

class Sccs {
 public:
  struct Component {
    /// Member blocks, in no meaningful order.
    std::vector<il::BlockId> blocks;
    /// More than one block, or a single block with an edge to itself.
    bool cyclic = false;
  };

  /// Components over blocks reachable from the entry, in reverse topological
  /// order: a component's successors appear before the component itself, so
  /// consumers walking the list meet a structure's parts before its entry.
  [[nodiscard]] static Sccs compute(const il::Function& function);

  [[nodiscard]] std::span<const Component> components() const noexcept { return components_; }
  /// The component a block belongs to, or kInvalidIndex when unreachable.
  [[nodiscard]] uint32_t componentOf(il::BlockId block) const noexcept;

 private:
  std::vector<Component> components_;
  std::vector<uint32_t> membership_;  // block index -> component index
};

}  // namespace xdec::analysis
