// Constant folding and lazy-flag condition folding (see transform.h).
#include "transform.h"

#include <unordered_map>

#include "xdec/il/ceval.h"

namespace xdec::passes {

namespace {

il::ExprId foldConstantsRec(il::Function& function, il::ExprId id,
                            std::unordered_map<il::ExprId, il::ExprId>& memo) {
  if (const auto found = memo.find(id); found != memo.end()) {
    return found->second;
  }
  // By value, not by reference: the recursion interns into the expression
  // pool, which can reallocate it — a held reference would dangle.
  const il::Expr expr = function.expr(id);

  // Leaves never change.
  if (expr.operandCount == 0) {
    memo.emplace(id, id);
    return id;
  }

  il::Expr rebuilt = expr;
  bool changed = false;
  for (unsigned index = 0; index < expr.operandCount; ++index) {
    const il::ExprId folded = foldConstantsRec(function, expr.operands[index], memo);
    changed |= folded != expr.operands[index];
    rebuilt.operands[index] = folded;
  }

  il::ExprId result = changed ? function.intern(rebuilt) : id;
  // A flags-typed result has no Const representation; leave the bundle.
  if (rebuilt.type.isFlags()) {
    memo.emplace(id, result);
    return result;
  }
  if (il::ConcreteValue value; il::tryEvalConst(function, result, value)) {
    if (value.hi == 0) {
      result = function.constant(rebuilt.type, value.lo);
    }
    // Widest constants the pool cannot spell yet stay as computed trees.
  }
  memo.emplace(id, result);
  return result;
}

/// The FlagCond rewrite tables. `a`/`b` are the FlagDef's operands, already
/// folded; helpers build into the function's hash-consed pool, so repeats are
/// free. The FlagDef is held by value for the same reason foldConstantsRec
/// copies: building the replacement interns, and pool growth dangles
/// references.
class FlagConditionRewrite {
 public:
  FlagConditionRewrite(il::Function& function, const il::Expr& flagDef)
      : function_(function), flagDef_(flagDef) {}

  [[nodiscard]] il::ExprId rewrite(il::ConditionCode code) {
    switch (il::flagDefOp(flagDef_.immediate)) {
      case il::FlagOp::Sub:
        return rewriteSub(code);
      case il::FlagOp::Add:
        return rewriteAdd(code);
      case il::FlagOp::Logical:
        return rewriteLogical(code);
      case il::FlagOp::Const:
      case il::FlagOp::AddCarry:
      case il::FlagOp::SubCarry:
      case il::FlagOp::Count:
        return onlyAlways(code);
    }
    return il::ExprId{};
  }

 private:
  [[nodiscard]] il::ExprId onlyAlways(il::ConditionCode code) {
    switch (code) {
      case il::ConditionCode::Always:
        return function_.boolean(true);
      case il::ConditionCode::Never:
        return function_.boolean(false);
      default:
        return il::ExprId{};
    }
  }

  // subs a, b — the cmp family. Every condition maps to a plain compare.
  [[nodiscard]] il::ExprId rewriteSub(il::ConditionCode code) {
    const il::ExprId a = flagDef_.operands[0];
    const il::ExprId b = flagDef_.operands[1];
    const il::Type type = il::Type::integer(il::flagDefWidth(flagDef_.immediate));
    const auto diff = [&] { return function_.binary(il::ExprOp::Sub, a, b); };
    const auto zero = [&] { return function_.constant(type, 0); };
    switch (code) {
      case il::ConditionCode::Equal: return function_.binary(il::ExprOp::CmpEq, a, b);
      case il::ConditionCode::NotEqual: return function_.binary(il::ExprOp::CmpNe, a, b);
      case il::ConditionCode::CarrySet: return function_.binary(il::ExprOp::CmpLeU, b, a);
      case il::ConditionCode::CarryClear: return function_.binary(il::ExprOp::CmpLtU, a, b);
      case il::ConditionCode::UnsignedGreater:
        return function_.binary(il::ExprOp::CmpLtU, b, a);
      case il::ConditionCode::UnsignedLessEqual:
        return function_.binary(il::ExprOp::CmpLeU, a, b);
      case il::ConditionCode::SignedGreaterEqual:
        return function_.binary(il::ExprOp::CmpLeS, b, a);
      case il::ConditionCode::SignedLess: return function_.binary(il::ExprOp::CmpLtS, a, b);
      case il::ConditionCode::SignedGreater:
        return function_.binary(il::ExprOp::CmpLtS, b, a);
      case il::ConditionCode::SignedLessEqual:
        return function_.binary(il::ExprOp::CmpLeS, a, b);
      case il::ConditionCode::Negative:
        return function_.binary(il::ExprOp::CmpLtS, diff(), zero());
      case il::ConditionCode::NonNegative:
        return function_.binary(il::ExprOp::CmpLeS, zero(), diff());
      case il::ConditionCode::Always: return function_.boolean(true);
      case il::ConditionCode::Never: return function_.boolean(false);
      default: return il::ExprId{};  // Overflow/NoOverflow need the bundle
    }
  }

  // adds a, b — the cmn family. Carry tests compare the sum against an
  // operand (carry out of a+b <=> sum <u a); overflow tests stay lazy.
  [[nodiscard]] il::ExprId rewriteAdd(il::ConditionCode code) {
    const il::ExprId a = flagDef_.operands[0];
    const il::ExprId b = flagDef_.operands[1];
    const il::Type type = il::Type::integer(il::flagDefWidth(flagDef_.immediate));
    const auto sum = [&] { return function_.binary(il::ExprOp::Add, a, b); };
    const auto zero = [&] { return function_.constant(type, 0); };
    switch (code) {
      case il::ConditionCode::Equal: return function_.binary(il::ExprOp::CmpEq, sum(), zero());
      case il::ConditionCode::NotEqual:
        return function_.binary(il::ExprOp::CmpNe, sum(), zero());
      case il::ConditionCode::CarrySet:
        return function_.binary(il::ExprOp::CmpLtU, sum(), a);
      case il::ConditionCode::CarryClear:
        return function_.binary(il::ExprOp::CmpLeU, a, sum());
      case il::ConditionCode::UnsignedGreater: {
        // C && !Z
        return function_.binary(il::ExprOp::And,
                                function_.binary(il::ExprOp::CmpLtU, sum(), a),
                                function_.binary(il::ExprOp::CmpNe, sum(), zero()));
      }
      case il::ConditionCode::UnsignedLessEqual: {
        // !C || Z
        return function_.binary(il::ExprOp::Or,
                                function_.binary(il::ExprOp::CmpLeU, a, sum()),
                                function_.binary(il::ExprOp::CmpEq, sum(), zero()));
      }
      case il::ConditionCode::Negative:
        return function_.binary(il::ExprOp::CmpLtS, sum(), zero());
      case il::ConditionCode::NonNegative:
        return function_.binary(il::ExprOp::CmpLeS, zero(), sum());
      case il::ConditionCode::Always: return function_.boolean(true);
      case il::ConditionCode::Never: return function_.boolean(false);
      default: return il::ExprId{};
    }
  }

  // ands/tst: N and Z from the result, C and V cleared — so every condition
  // has a closed form over the result value.
  [[nodiscard]] il::ExprId rewriteLogical(il::ConditionCode code) {
    const il::ExprId r = flagDef_.operands[0];
    const il::Type type = il::Type::integer(il::flagDefWidth(flagDef_.immediate));
    const auto zero = [&] { return function_.constant(type, 0); };
    const auto eq = [&] { return function_.binary(il::ExprOp::CmpEq, r, zero()); };
    const auto ne = [&] { return function_.binary(il::ExprOp::CmpNe, r, zero()); };
    const auto neg = [&] { return function_.binary(il::ExprOp::CmpLtS, r, zero()); };
    const auto pos = [&] { return function_.binary(il::ExprOp::CmpLeS, zero(), r); };
    switch (code) {
      case il::ConditionCode::Equal: return eq();
      case il::ConditionCode::NotEqual: return ne();
      case il::ConditionCode::Negative: return neg();
      case il::ConditionCode::NonNegative: return pos();
      case il::ConditionCode::CarrySet: return function_.boolean(false);
      case il::ConditionCode::CarryClear: return function_.boolean(true);
      case il::ConditionCode::Overflow: return function_.boolean(false);
      case il::ConditionCode::NoOverflow: return function_.boolean(true);
      case il::ConditionCode::UnsignedGreater: return function_.boolean(false);
      case il::ConditionCode::UnsignedLessEqual: return eq();
      case il::ConditionCode::SignedGreaterEqual: return pos();
      case il::ConditionCode::SignedLess: return neg();
      case il::ConditionCode::SignedGreater:
        return function_.binary(il::ExprOp::And, ne(), pos());
      case il::ConditionCode::SignedLessEqual:
        return function_.binary(il::ExprOp::Or, eq(), neg());
      case il::ConditionCode::Always: return function_.boolean(true);
      case il::ConditionCode::Never: return function_.boolean(false);
      default: return il::ExprId{};
    }
  }

  il::Function& function_;
  const il::Expr flagDef_;
};

il::ExprId foldFlagConditionsRec(il::Function& function, il::ExprId id,
                                 std::unordered_map<il::ExprId, il::ExprId>& memo) {
  if (const auto found = memo.find(id); found != memo.end()) {
    return found->second;
  }
  // By value: interning below can grow the pool (see foldConstantsRec).
  const il::Expr expr = function.expr(id);

  il::Expr rebuilt = expr;
  bool changed = false;
  for (unsigned index = 0; index < expr.operandCount; ++index) {
    const il::ExprId folded = foldFlagConditionsRec(function, expr.operands[index], memo);
    changed |= folded != expr.operands[index];
    rebuilt.operands[index] = folded;
  }
  il::ExprId result = changed ? function.intern(rebuilt) : id;

  // The pattern: flagcond(flagdef ...). Anything else stays as rebuilt.
  if (expr.op == il::ExprOp::FlagCond) {
    const il::Expr& cond = function.expr(result);
    const il::Expr& flags = function.expr(cond.operands[0]);
    if (flags.op == il::ExprOp::FlagDef) {
      if (const il::ExprId rewritten =
              FlagConditionRewrite(function, flags)
                  .rewrite(static_cast<il::ConditionCode>(cond.immediate));
          rewritten.valid()) {
        result = rewritten;
      }
    }
  }
  memo.emplace(id, result);
  return result;
}

/// Applies a per-operand rewrite to every op of every block. Shared driver
/// for the two folds above.
template <class Rewrite>
bool foldOperands(il::Function& function, Rewrite&& rewrite) {
  bool changed = false;
  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      const auto operands = function.operands(function.op(opId));
      if (operands.empty()) {
        continue;
      }
      std::vector<il::ExprId> rewritten(operands.begin(), operands.end());
      bool opChanged = false;
      for (il::ExprId& operand : rewritten) {
        const il::ExprId next = rewrite(function, operand);
        opChanged |= next != operand;
        operand = next;
      }
      if (opChanged) {
        function.setOperands(opId, rewritten);
        changed = true;
      }
    }
  }
  return changed;
}

}  // namespace

il::ExprId foldConstants(il::Function& function, il::ExprId id) {
  std::unordered_map<il::ExprId, il::ExprId> memo;
  return foldConstantsRec(function, id, memo);
}

il::ExprId foldFlagConditions(il::Function& function, il::ExprId id) {
  std::unordered_map<il::ExprId, il::ExprId> memo;
  return foldFlagConditionsRec(function, id, memo);
}

bool foldConstants(il::Function& function) {
  return foldOperands(function, [](il::Function& f, il::ExprId id) {
    return foldConstants(f, id);
  });
}

bool foldFlagConditions(il::Function& function) {
  return foldOperands(function, [](il::Function& f, il::ExprId id) {
    return foldFlagConditions(f, id);
  });
}

}  // namespace xdec::passes
