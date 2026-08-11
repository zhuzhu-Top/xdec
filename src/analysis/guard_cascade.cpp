// See the header for the shape this looks for and why it is not a diamond.
#include "xdec/analysis/guard_cascade.h"

#include <array>
#include <utility>

namespace xdec::analysis {

namespace {

/// `block`'s own CondBranch condition and two targets, or nullopt when its
/// terminator is not a two-way branch at all (including a degenerate one
/// whose targets happen to coincide -- structure.cpp's own diamond code
/// treats that the same way, as nothing worth branching on).
[[nodiscard]] std::optional<std::pair<il::ExprId, std::array<il::BlockId, 2>>> condBranchOf(
    const il::Function& function, il::BlockId block) {
  const il::Block& info = function.block(block);
  if (info.empty()) {
    return std::nullopt;
  }
  const il::Op& terminator = function.op(info.terminator());
  if (terminator.code != il::OpCode::CondBranch) {
    return std::nullopt;
  }
  const auto targets = function.targets(terminator);
  const auto operands = function.operands(terminator);
  if (targets.size() != 2 || targets[0] == targets[1]) {
    return std::nullopt;
  }
  return std::make_pair(operands[0], std::array<il::BlockId, 2>{targets[0], targets[1]});
}

}  // namespace

std::optional<GuardCascadeShape> matchGuardCascade(const il::Function& function,
                                                    const PostDominators& postDominators,
                                                    il::BlockId head) {
  const auto outer = condBranchOf(function, head);
  if (!outer.has_value()) {
    return std::nullopt;
  }
  const auto& [outerCond, outerTargets] = *outer;

  const il::BlockId merge = postDominators.ipdom(head);
  if (!merge.valid() || merge == head) {
    return std::nullopt;
  }

  // Nothing about a CondBranch's operand order says which side is the
  // private guard and which is the shared exceptional case, so both
  // assignments are tried.
  for (const bool innerIsFirstTarget : {true, false}) {
    const il::BlockId innerHead = outerTargets[innerIsFirstTarget ? 0 : 1];
    const il::BlockId fallback = outerTargets[innerIsFirstTarget ? 1 : 0];
    if (innerHead == fallback || innerHead == head || fallback == head ||
        innerHead == merge || fallback == merge) {
      continue;
    }
    // The inner guard has to be this cascade's alone: claiming it into the
    // outer guard's own nested `if` would otherwise strand whatever else
    // jumps into it with no way to reach it anymore.
    const il::Block& innerBlock = function.block(innerHead);
    if (innerBlock.predecessors.size() != 1 || innerBlock.predecessors.front() != head) {
      continue;
    }
    const auto inner = condBranchOf(function, innerHead);
    if (!inner.has_value()) {
      continue;
    }
    const auto& [innerCond, innerTargets] = *inner;
    for (const bool successIsFirstTarget : {true, false}) {
      const il::BlockId success = innerTargets[successIsFirstTarget ? 0 : 1];
      const il::BlockId innerFallback = innerTargets[successIsFirstTarget ? 1 : 0];
      if (success != merge || innerFallback != fallback) {
        continue;
      }
      // The fallback is shared on purpose, not two different blocks that
      // happen to look alike: its only predecessors must be exactly these
      // two guards' own failure arms, or something outside this cascade
      // would need a label to reach it and printing it inline would lose
      // that entry point.
      const il::Block& fallbackBlock = function.block(fallback);
      if (fallbackBlock.predecessors.size() != 2) {
        continue;
      }
      bool sawOuter = false;
      bool sawInner = false;
      for (const il::BlockId pred : fallbackBlock.predecessors) {
        sawOuter = sawOuter || pred == head;
        sawInner = sawInner || pred == innerHead;
      }
      if (!sawOuter || !sawInner) {
        continue;
      }
      // The fallback must flow straight into the same merge the inner
      // guard's success arm reaches -- the fact that makes it safe to print
      // once, as a shared body, rather than as its own separate exit.
      if (fallbackBlock.successors.size() != 1 || fallbackBlock.successors.front() != merge) {
        continue;
      }
      return GuardCascadeShape{head,      innerHead, fallback,
                               merge,     outerCond, innerCond,
                               successIsFirstTarget};
    }
  }
  return std::nullopt;
}

}  // namespace xdec::analysis
