#include "xdec/passes/algebra_rules.h"

#include <array>

#include "xdec/il/ceval.h"
#include "xdec/support/bits.h"

namespace xdec::passes {

namespace {

[[nodiscard]] uint64_t splitmix64(uint64_t& state) noexcept {
  uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

}  // namespace

Result<void> AlgebraRuleRegistry::registerVerifiedRule(
    std::string name, unsigned arity, unsigned bits,
    const AlgebraSampleBuilderFn& sampleBuilder, AlgebraRewriteFn rewrite) {
  if (name.empty() || !sampleBuilder || !rewrite || arity == 0 ||
      (bits != 8 && bits != 16 && bits != 32 && bits != 64)) {
    return err(DiagCode::Internal, "invalid algebra rule registration '{}'", name);
  }

  static constexpr std::array<uint64_t, 8> kBoundary{
      0ULL, 1ULL, 2ULL, 3ULL, 0x7fffffffULL, 0x80000000ULL, 0xffffffffULL, ~uint64_t{0}};
  uint64_t prng = 0x517cc1b727220a95ULL;
  const il::Type ty = il::Type::integer(bits);

  for (unsigned round = 0; round < 32; ++round) {
    il::RegisterFile rf;
    il::Function fn(Arch::AArch64, rf, 0x1000);
    std::vector<il::ExprId> vars;
    vars.reserve(arity);
    for (unsigned i = 0; i < arity; ++i) {
      const uint64_t raw = round < kBoundary.size()
                               ? kBoundary[(round + i) % kBoundary.size()]
                               : splitmix64(prng);
      vars.push_back(fn.constant(ty, zeroExtend(raw, bits)));
    }
    const il::ExprId before = sampleBuilder(fn, vars);
    if (!before.valid() || fn.expr(before).type != ty) {
      return err(DiagCode::Internal, "algebra rule '{}' sampleBuilder produced invalid expr",
                 name);
    }
    il::ConcreteValue beforeVal{};
    if (!il::tryEvalConst(fn, before, beforeVal)) {
      return err(DiagCode::Internal,
                 "algebra rule '{}' sample expression could not be constant-evaluated", name);
    }
    const il::ExprId after = rewrite(fn, before);
    if (!after.valid()) {
      return err(DiagCode::VerifyFailure,
                 "algebra rule '{}' failed to match its own sample expression", name);
    }
    if (fn.expr(after).type != ty) {
      return err(DiagCode::VerifyFailure,
                 "algebra rule '{}' changed expression type from {} to {}",
                 name, ty.toString(), fn.expr(after).type.toString());
    }
    il::ConcreteValue afterVal{};
    if (!il::tryEvalConst(fn, after, afterVal)) {
      return err(DiagCode::VerifyFailure,
                 "algebra rule '{}' rewrite could not be evaluated by oracle", name);
    }
    if (beforeVal.lo != afterVal.lo) {
      return err(DiagCode::VerifyFailure,
                 "algebra rule '{}' failed oracle equivalence on vector #{}: {:#x} != {:#x}",
                 name, round, afterVal.lo, beforeVal.lo);
    }
  }

  rules_.push_back(RegisteredAlgebraRule{std::move(name), std::move(rewrite)});
  return {};
}

il::ExprId AlgebraRuleRegistry::applyAll(il::Function& function, il::ExprId expr) const {
  for (const auto& rule : rules_) {
    if (rule.rewrite) {
      const il::ExprId next = rule.rewrite(function, expr);
      if (next.valid() && next != expr &&
          function.expr(next).type == function.expr(expr).type) {
        return next;
      }
    }
  }
  return expr;
}

AlgebraRuleRegistry& AlgebraRuleRegistry::global() {
  static AlgebraRuleRegistry instance;
  return instance;
}

}  // namespace xdec::passes
