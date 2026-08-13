// See the header for what this recovers and why it does not need memory.
#include "xdec/analysis/dispatch_values.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

#include "xdec/analysis/image_eval.h"
#include "xdec/support/reader.h"
#include "xdec/support/result.h"

namespace xdec::analysis {

namespace {

/// Never maps anything: any load in `index`'s tree degrades that subtree to
/// top exactly the way an unmapped address would, rather than answering a
/// question about memory this analysis has no business asking. Everything
/// else -- constants, selects, the arithmetic and compares an opaque
/// predicate is built from -- ImageEval already evaluates without ever
/// calling this.
Result<void> neverMapped(uint64_t /*va*/, std::span<std::byte> /*out*/) {
  return err(DiagCode::UnmappedAddress, "dispatch-value recovery reads no memory");
}

}  // namespace

std::optional<DispatchValues> matchDispatchValues(const il::Function& function, il::ExprId index,
                                                   std::size_t targetCount) {
  if (targetCount == 0 || !index.valid()) {
    return std::nullopt;
  }
  ImageEval eval(function, neverMapped);
  const ValueSet indexSet = eval.eval(index);
  if (indexSet.isTop() || indexSet.values().size() != targetCount) {
    return std::nullopt;
  }

  DispatchValues result;
  result.values.assign(indexSet.values().begin(), indexSet.values().end());
  std::sort(result.values.begin(), result.values.end());

  if (targetCount == 2) {
    // Search the whole expression DAG -- not just the arm the top-level
    // condition happens to pick -- for the Select that actually splits the
    // two values apart. The outer clamp a bounds check adds (`bound < state
    // ? replacement : state`) has one arm that evaluates to a single value
    // outside this pair, so it will not match here and the search finds the
    // inner select instead: the one the obfuscator's own branch condition
    // feeds.
    std::vector<il::ExprId> stack{index};
    std::unordered_set<uint32_t> visited;
    while (!stack.empty()) {
      const il::ExprId id = stack.back();
      stack.pop_back();
      if (!visited.insert(id.index()).second) {
        continue;
      }
      const il::Expr& expr = function.expr(id);
      if (expr.op == il::ExprOp::Select) {
        const ValueSet trueSet = eval.eval(expr.operands[1]);
        const ValueSet falseSet = eval.eval(expr.operands[2]);
        if (!trueSet.isTop() && trueSet.values().size() == 1 && !falseSet.isTop() &&
            falseSet.values().size() == 1) {
          const uint64_t whenTrue = trueSet.values()[0];
          const uint64_t whenFalse = falseSet.values()[0];
          const bool matchesForward =
              whenTrue == result.values[0] && whenFalse == result.values[1];
          const bool matchesReverse =
              whenTrue == result.values[1] && whenFalse == result.values[0];
          if (matchesForward || matchesReverse) {
            result.condition = expr.operands[0];
            result.conditionTrueIsFirst = matchesForward;
            break;
          }
        }
      }
      for (unsigned operand = 0; operand < expr.operandCount; ++operand) {
        stack.push_back(expr.operands[operand]);
      }
    }
  }
  return result;
}

}  // namespace xdec::analysis
