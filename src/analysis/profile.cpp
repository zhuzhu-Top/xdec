// Obfuscation profiling (see the header for the contract).
#include "xdec/analysis/profile.h"

#include <format>
#include <unordered_map>

#include "xdec/analysis/scc.h"

namespace xdec::analysis {

namespace {

/// An expression tree earns the MBA label when it mixes the arithmetic and
/// bitwise families AND nests deep enough that a human would not have written
/// it. Honest code does `a + b` or `a & mask`; obfuscated code does
/// `((x^y) + 2*(x&y)) ^ rotl(~x, 7)` four levels deep. Memoized over the
/// hash-consed pool: the same DAG-sharing that makes the pool compact would
/// make a naive walk exponential.
class MbaScan {
 public:
  explicit MbaScan(const il::Function& function) : function_(function) {}

  [[nodiscard]] bool looksMba(il::ExprId id) {
    if (const auto found = memo_.find(id); found != memo_.end()) {
      return found->second;
    }
    // Cycle guard before recursion: the pool is acyclic by construction, but
    // the memo must be primed to keep shared subtrees linear.
    const bool result = compute(id);
    memo_.emplace(id, result);
    return result;
  }

 private:
  [[nodiscard]] bool compute(il::ExprId id) {
    const il::Expr& expr = function_.expr(id);
    const il::ExprCategory category = il::info(expr.op).category;
    const bool arithmetic = category == il::ExprCategory::IntArithmetic;
    const bool bitwise = category == il::ExprCategory::IntBitwise ||
                         category == il::ExprCategory::IntShift;
    bool childArithmetic = false;
    bool childBitwise = false;
    bool anyNested = false;
    for (unsigned index = 0; index < expr.operandCount; ++index) {
      const il::Expr& child = function_.expr(expr.operands[index]);
      const il::ExprCategory childCategory = il::info(child.op).category;
      childArithmetic = childArithmetic || childCategory == il::ExprCategory::IntArithmetic;
      childBitwise = childBitwise || childCategory == il::ExprCategory::IntBitwise ||
                      childCategory == il::ExprCategory::IntShift;
      anyNested = anyNested || child.operandCount > 0;
    }
    // The mix, with something underneath it: arithmetic over bitwise children
    // or the reverse, at least two levels of it.
    if (((arithmetic && childBitwise) || (bitwise && childArithmetic)) && anyNested) {
      return true;
    }
    for (unsigned index = 0; index < expr.operandCount; ++index) {
      if (function_.expr(expr.operands[index]).operandCount > 0 &&
          looksMba(expr.operands[index])) {
        return true;
      }
    }
    return false;
  }

  const il::Function& function_;
  std::unordered_map<il::ExprId, bool> memo_;
};

}  // namespace

bool ObfuscationProfile::likelyFlattened() const noexcept {
  // Most of the function flowing back into one unresolved dispatcher. The
  // thresholds are conservative: honest switch tables see a handful of
  // predecessors, not the majority of the function.
  return dispatcherFanIn >= 8 && dispatcherFanIn * 2 >= blocks;
}

bool ObfuscationProfile::likelyMba() const noexcept {
  return mbaExpressions >= 4;
}

std::string ObfuscationProfile::format() const {
  return std::format(
      "{} block(s), largest cycle {}, dispatcher fan-in {} | indirect branches {} ({} "
      "unresolved), indirect calls {} | {} MBA expression(s){}{}",
      blocks, largestScc, dispatcherFanIn, indirectBranches, unresolvedIndirect,
      indirectCalls, mbaExpressions, likelyFlattened() ? " | FLATTENED" : "",
      likelyMba() ? " | MBA" : "");
}

ObfuscationProfile profile(const il::Function& function) {
  ObfuscationProfile result;
  result.blocks = static_cast<uint32_t>(function.blockCount());
  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      if (op.code == il::OpCode::IndirectBranch) {
        ++result.indirectBranches;
        if (function.targets(op).empty()) {
          ++result.unresolvedIndirect;
          result.dispatcherFanIn = std::max(
              result.dispatcherFanIn,
              static_cast<uint32_t>(function.block(blockId).predecessors.size()));
        }
      } else if (op.code == il::OpCode::Call) {
        const auto operands = function.operands(op);
        if (!operands.empty() && function.expr(operands[0]).op != il::ExprOp::Const) {
          ++result.indirectCalls;
        }
      }
    }
  }

  const Sccs sccs = Sccs::compute(function);
  for (const Sccs::Component& component : sccs.components()) {
    result.largestScc =
        std::max(result.largestScc, static_cast<uint32_t>(component.blocks.size()));
  }

  MbaScan scan{function};
  for (const il::ExprId id : function.exprHandles()) {
    const il::Expr& expr = function.expr(id);
    if (expr.operandCount > 0 && scan.looksMba(id)) {
      ++result.mbaExpressions;
    }
  }
  return result;
}

}  // namespace xdec::analysis
