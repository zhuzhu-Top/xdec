// Cooper-Harvey-Kennedy dominators and their post-dominator mirror.
#include "xdec/analysis/dominators.h"

#include <algorithm>

namespace xdec::analysis {

namespace {

constexpr uint32_t kUnreachable = il::BlockId::kInvalidIndex;

/// The CHK meet: walks both fingers up the idom chain until they agree.
/// `parent` must return the block's idom, with the root answering itself.
template <class Parent, class Order>
il::BlockId intersect(il::BlockId a, il::BlockId b, Parent&& parent, Order&& order) {
  while (a != b) {
    while (order(a) > order(b)) {
      a = parent(a);
    }
    while (order(b) > order(a)) {
      b = parent(b);
    }
  }
  return a;
}

/// The meet for a post-dominator forest: the chain tops out at the virtual
/// exit root (an invalid BlockId), which a plain order comparison cannot
/// rank. Fingers that meet there meet at the root. One parent step per turn,
/// so the validity guard sees the root before any order lookup does.
template <class Parent, class Order>
il::BlockId intersectForest(il::BlockId a, il::BlockId b, Parent&& parent, Order&& order) {
  while (a != b) {
    if (!a.valid() || !b.valid()) {
      return il::BlockId{};
    }
    if (order(a) > order(b)) {
      a = parent(a);
    } else {
      b = parent(b);
    }
  }
  return a;
}

}  // namespace

Dominators Dominators::compute(const il::Function& function) {
  Dominators tree;
  tree.rpo_ = function.reversePostOrder();
  const std::size_t n = function.blockCount();
  tree.rpoIndex_.assign(n, kUnreachable);
  tree.idom_.assign(n, il::BlockId{});
  tree.depth_.assign(n, -1);
  tree.children_.resize(n);
  tree.frontier_.resize(n);
  for (uint32_t i = 0; i < tree.rpo_.size(); ++i) {
    tree.rpoIndex_[tree.rpo_[i].asSize()] = i;
  }
  if (tree.rpo_.empty()) {
    return tree;
  }

  const auto order = [&tree](il::BlockId b) { return tree.rpoIndex_[b.asSize()]; };
  const il::BlockId entry = tree.rpo_.front();
  tree.idom_[entry.asSize()] = entry;
  tree.depth_[entry.asSize()] = 0;

  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t i = 1; i < tree.rpo_.size(); ++i) {
      const il::BlockId block = tree.rpo_[i];
      il::BlockId newIdom{};
      for (const il::BlockId pred : function.block(block).predecessors) {
        if (!tree.idom_[pred.asSize()].valid()) {
          continue;  // unreachable, or a back edge not yet processed
        }
        newIdom = newIdom.valid()
                      ? intersect(pred, newIdom,
                                  [&tree](il::BlockId b) { return tree.idom_[b.asSize()]; },
                                  order)
                      : pred;
      }
      if (newIdom.valid() && tree.idom_[block.asSize()] != newIdom) {
        tree.idom_[block.asSize()] = newIdom;
        changed = true;
      }
    }
  }

  // RPO guarantees idoms are already final here, so depth and the child lists
  // fall out of one pass.
  for (std::size_t i = 1; i < tree.rpo_.size(); ++i) {
    const il::BlockId block = tree.rpo_[i];
    const il::BlockId parent = tree.idom_[block.asSize()];
    tree.depth_[block.asSize()] = tree.depth_[parent.asSize()] + 1;
    tree.children_[parent.asSize()].push_back(block);
  }

  // Cytron's frontier: at a join, walk each predecessor up to (but excluding)
  // the join's idom; everything on that climb has the join in its frontier.
  for (const il::BlockId join : tree.rpo_) {
    const auto& preds = function.block(join).predecessors;
    if (preds.size() < 2) {
      continue;
    }
    for (const il::BlockId pred : preds) {
      if (!tree.reachable(pred)) {
        continue;
      }
      il::BlockId runner = pred;
      while (runner != tree.idom_[join.asSize()]) {
        tree.frontier_[runner.asSize()].insert(join);
        runner = tree.idom_[runner.asSize()];
      }
    }
  }
  return tree;
}

bool Dominators::reachable(il::BlockId block) const noexcept {
  return block.asSize() < rpoIndex_.size() && rpoIndex_[block.asSize()] != kUnreachable;
}

il::BlockId Dominators::idom(il::BlockId block) const noexcept {
  if (!reachable(block) || block == rpo_.front()) {
    return il::BlockId{};
  }
  return idom_[block.asSize()];
}

int Dominators::depth(il::BlockId block) const noexcept {
  return block.asSize() < depth_.size() ? depth_[block.asSize()] : -1;
}

bool Dominators::dominates(il::BlockId a, il::BlockId b) const noexcept {
  if (!reachable(a) || !reachable(b)) {
    return a == b;
  }
  const il::BlockId entry = rpo_.front();
  il::BlockId finger = b;
  while (finger != a && finger != entry) {
    finger = idom_[finger.asSize()];
  }
  return finger == a;
}

std::span<const il::BlockId> Dominators::children(il::BlockId block) const noexcept {
  return children_[block.asSize()];
}

const std::set<il::BlockId>& Dominators::frontier(il::BlockId block) const noexcept {
  return frontier_[block.asSize()];
}

PostDominators PostDominators::compute(const il::Function& function) {
  PostDominators tree;
  const std::size_t n = function.blockCount();
  tree.ipdom_.assign(n, il::BlockId{});
  tree.depth_.assign(n, -1);

  // Exits are blocks without successors; the reverse graph's roots.
  std::vector<il::BlockId> exits;
  for (const il::BlockId block : function.reversePostOrder()) {
    if (function.block(block).successors.empty()) {
      exits.push_back(block);
    }
  }
  if (exits.empty()) {
    return tree;
  }

  // Reverse post-order of the reverse graph, from every exit at once: DFS over
  // predecessors, post-order, reversed. Blocks that cannot reach an exit never
  // appear and keep their default "outside the tree" entries.
  std::vector<il::BlockId> postorder;
  std::vector<bool> visited(n, false);
  for (const il::BlockId exit : exits) {
    if (visited[exit.asSize()]) {
      continue;
    }
    std::vector<std::pair<il::BlockId, std::size_t>> stack{{exit, 0}};
    visited[exit.asSize()] = true;
    while (!stack.empty()) {
      auto& [block, next] = stack.back();
      const auto& preds = function.block(block).predecessors;
      if (next < preds.size()) {
        const il::BlockId pred = preds[next++];
        if (!visited[pred.asSize()]) {
          visited[pred.asSize()] = true;
          stack.emplace_back(pred, 0);
        }
      } else {
        postorder.push_back(block);
        stack.pop_back();
      }
    }
  }
  std::vector<il::BlockId> rpo(postorder.rbegin(), postorder.rend());

  std::vector<uint32_t> order(n, kUnreachable);
  for (uint32_t i = 0; i < rpo.size(); ++i) {
    order[rpo[i].asSize()] = i;
  }
  const auto orderOf = [&order](il::BlockId b) { return order[b.asSize()]; };
  std::vector<bool> isExit(n, false);
  for (const il::BlockId exit : exits) {
    isExit[exit.asSize()] = true;
    tree.depth_[exit.asSize()] = 0;  // ipdom stays invalid: directly under the root
  }

  const auto parent = [&tree](il::BlockId b) { return tree.ipdom_[b.asSize()]; };
  bool changed = true;
  while (changed) {
    changed = false;
    for (const il::BlockId block : rpo) {
      if (isExit[block.asSize()]) {
        continue;
      }
      // Meet over forward successors = predecessors of the reverse graph.
      il::BlockId newIpdom{};
      bool first = true;
      for (const il::BlockId succ : function.block(block).successors) {
        if (order[succ.asSize()] == kUnreachable) {
          continue;  // successor cannot reach an exit either
        }
        if (first) {
          newIpdom = succ;
          first = false;
        } else {
          newIpdom = intersectForest(newIpdom, succ, parent, orderOf);
          if (!newIpdom.valid()) {
            break;  // met at the virtual root; cannot get lower
          }
        }
      }
      if (newIpdom != tree.ipdom_[block.asSize()]) {
        tree.ipdom_[block.asSize()] = newIpdom;
        changed = true;
      }
    }
  }

  for (const il::BlockId block : rpo) {
    const il::BlockId parentBlock = tree.ipdom_[block.asSize()];
    if (parentBlock.valid()) {
      tree.depth_[block.asSize()] = tree.depth_[parentBlock.asSize()] + 1;
    } else if (tree.depth_[block.asSize()] < 0) {
      tree.depth_[block.asSize()] = 0;  // directly under the virtual root
    }
  }
  return tree;
}

bool PostDominators::reachesExit(il::BlockId block) const noexcept {
  return block.asSize() < depth_.size() && depth_[block.asSize()] >= 0;
}

il::BlockId PostDominators::ipdom(il::BlockId block) const noexcept {
  if (!reachesExit(block)) {
    return il::BlockId{};
  }
  return ipdom_[block.asSize()];
}

bool PostDominators::postDominates(il::BlockId a, il::BlockId b) const noexcept {
  if (!reachesExit(a) || !reachesExit(b)) {
    return a == b;
  }
  il::BlockId finger = b;
  while (finger != a) {
    const il::BlockId next = ipdom_[finger.asSize()];
    if (!next.valid()) {
      return false;  // walked into the virtual exit root without meeting `a`
    }
    finger = next;
  }
  return true;
}

}  // namespace xdec::analysis
