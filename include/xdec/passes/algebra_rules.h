// Oracle-verified algebraic / MBA rewrite rule registry.
//
// External plugins and scripts can register additional algebraic identities,
// but every candidate rule MUST pass randomized + boundary concrete evaluation
// against `il::tryEvalConst` before entering the active rule set; any rule that
// produces even a 1-bit discrepancy is rejected with `DiagCode::VerifyFailure`.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "xdec/il/function.h"
#include "xdec/support/result.h"

namespace xdec::passes {

using AlgebraRewriteFn = std::function<il::ExprId(il::Function&, il::ExprId)>;
using AlgebraSampleBuilderFn =
    std::function<il::ExprId(il::Function&, const std::vector<il::ExprId>& vars)>;

struct RegisteredAlgebraRule {
  std::string name;
  AlgebraRewriteFn rewrite;
};

class AlgebraRuleRegistry {
 public:
  /// Validates `rewrite` on a sample expression built by `sampleBuilder` over
  /// `arity` symbolic variables at bit width `bits` using `il::tryEvalConst`
  /// across boundary and pseudo-random concrete bindings. Rejects unsound rules.
  [[nodiscard]] Result<void> registerVerifiedRule(
      std::string name, unsigned arity, unsigned bits,
      const AlgebraSampleBuilderFn& sampleBuilder, AlgebraRewriteFn rewrite);

  /// Applies all registered rules to `expr` inside `function`.
  [[nodiscard]] il::ExprId applyAll(il::Function& function, il::ExprId expr) const;

  [[nodiscard]] std::size_t size() const noexcept { return rules_.size(); }
  void clear() { rules_.clear(); }

  /// Process-wide verified rule registry consulted by `simplifyAlgebra`.
  [[nodiscard]] static AlgebraRuleRegistry& global();

 private:
  std::vector<RegisteredAlgebraRule> rules_;
};

}  // namespace xdec::passes
