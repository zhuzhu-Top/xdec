// Dominator and post-dominator trees over the IL CFG.
//
// Both are computed with the iterative Cooper-Harvey-Kennedy algorithm over a
// reverse post-order, the standard choice for flow graphs that are mostly
// reducible: each pass is linear, and reducible graphs converge in a couple of
// passes. The dominance frontier (Cytron's formulation) is included because
// SSA construction is the whole point of having this tree.
//
// Scope conventions, stated once here rather than re-derived at every call
// site:
//
//   - Dominators covers blocks reachable from the entry. An unreachable block
//     is reported as dominating only itself, which keeps queries total without
//     pretending the tree says anything about dead code.
//   - PostDominators covers blocks from which an exit is reachable, where an
//     exit is any block with no successors (returns, unresolved indirect
//     branches, external stubs). A block trapped in an endless loop therefore
//     post-dominates only itself. Exits all sit directly under the virtual
//     exit root, reported as an invalid BlockId.
#pragma once

#include <cstdint>
#include <set>
#include <span>
#include <vector>

#include "xdec/il/function.h"

namespace xdec::analysis {

class Dominators {
 public:
  [[nodiscard]] static Dominators compute(const il::Function& function);

  /// Blocks reachable from the entry, in reverse post-order. Most analyses
  /// iterate this order; it is exposed so they share one computation of it.
  [[nodiscard]] std::span<const il::BlockId> rpo() const noexcept { return rpo_; }

  [[nodiscard]] bool reachable(il::BlockId block) const noexcept;
  /// Invalid for the entry and for unreachable blocks.
  [[nodiscard]] il::BlockId idom(il::BlockId block) const noexcept;
  /// Dom-tree depth: 0 at the entry, -1 when unreachable.
  [[nodiscard]] int depth(il::BlockId block) const noexcept;

  /// Whether `a` dominates `b` (reflexive). Blocks outside the reachable set
  /// dominate only themselves.
  [[nodiscard]] bool dominates(il::BlockId a, il::BlockId b) const noexcept;
  [[nodiscard]] bool strictlyDominates(il::BlockId a, il::BlockId b) const noexcept {
    return a != b && dominates(a, b);
  }

  /// Immediate children in the dominator tree.
  [[nodiscard]] std::span<const il::BlockId> children(il::BlockId block) const noexcept;
  /// Cytron's dominance frontier: where `block`'s dominance ends. Drives
  /// phi placement in SSA construction.
  [[nodiscard]] const std::set<il::BlockId>& frontier(il::BlockId block) const noexcept;

 private:
  std::vector<il::BlockId> rpo_;
  std::vector<uint32_t> rpoIndex_;  // block index -> position, kInvalidIndex unreachable
  std::vector<il::BlockId> idom_;   // entry's idom is the entry itself
  std::vector<int> depth_;
  std::vector<std::vector<il::BlockId>> children_;
  std::vector<std::set<il::BlockId>> frontier_;
};

class PostDominators {
 public:
  [[nodiscard]] static PostDominators compute(const il::Function& function);

  /// Whether some exit is reachable from `block`. Blocks in endless loops are
  /// outside the tree.
  [[nodiscard]] bool reachesExit(il::BlockId block) const noexcept;
  /// Immediate post-dominator, or invalid when the block sits directly under
  /// the virtual exit root (every exit does) or is outside the tree.
  [[nodiscard]] il::BlockId ipdom(il::BlockId block) const noexcept;

  /// Whether `a` post-dominates `b` (reflexive): every path from `b` to an
  /// exit passes through `a`.
  [[nodiscard]] bool postDominates(il::BlockId a, il::BlockId b) const noexcept;

 private:
  std::vector<il::BlockId> ipdom_;  // invalid = virtual exit root / outside the tree
  std::vector<int> depth_;          // 0 at exits, -1 outside the tree
};

}  // namespace xdec::analysis
