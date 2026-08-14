// Constant folding and lazy-flag condition folding (see transform.h).
#include "transform.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

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

/// Distributes a FlagCond through a flags-typed phi that `foldFlagConditions`
/// cannot see through directly: its pattern is `flagcond(flagdef ...)`, and a
/// bundle that reaches its test by crossing a real CFG merge -- two blocks
/// each setting flags their own way before a shared test -- arrives as
/// `flagcond(phi(flagdef ..., flagdef ...))` instead, and stays the
/// `/*flagcond*/0` stub even though every edge into the merge is individually
/// foldable.
///
/// This distributes the *test* across the merge rather than trying to prove
/// the bundle itself is one value: one boolean phi, synthesized once per
/// (flags phi, condition code) pair and cached so every FlagCond that shares
/// the pair shares the phi, whose inputs are each edge's own flags rewritten
/// independently (recursing through further phis for a merge of merges). An
/// edge whose flags still cannot be resolved -- a load, a call result, a flag
/// bundle from another architecture register -- keeps its own honest FlagCond
/// there rather than costing every other, resolvable edge the same opacity.
/// A loop-carried flags phi's back edge, which refers to the phi being built,
/// is exactly such an edge: nothing upstream can resolve it without
/// re-executing the loop, so it is left as a FlagCond over that edge's value.
class FlagPhiDistributor {
 public:
  explicit FlagPhiDistributor(il::Function& function) : function_(function) {}

  /// `flags` denotes a flags-typed expression; returns the boolean `code`
  /// denotes, or an invalid id when it cannot be resolved from here.
  [[nodiscard]] il::ExprId resolve(il::ExprId flags, il::ConditionCode code) {
    return resolveRec(flags, code, 0);
  }

 private:
  struct Key {
    uint32_t phiValue;
    uint8_t code;
    friend bool operator==(const Key&, const Key&) = default;
  };
  struct KeyHash {
    std::size_t operator()(const Key& key) const noexcept {
      return (static_cast<std::size_t>(key.phiValue) << 8) | key.code;
    }
  };

  // Generous but finite: memoization already bounds the total number of
  // synthesized phis to (flags phis) x (condition codes), so this only guards
  // against a pathologically long non-cyclic merge-of-merges chain.
  static constexpr int kMaxDepth = 64;

  [[nodiscard]] il::ExprId resolveRec(il::ExprId flags, il::ConditionCode code, int depth) {
    const il::Expr expr = function_.expr(flags);
    if (expr.op == il::ExprOp::FlagDef) {
      return FlagConditionRewrite(function_, expr).rewrite(code);
    }
    if (expr.op != il::ExprOp::Value || depth >= kMaxDepth) {
      return il::ExprId{};
    }
    const auto value = il::ValueId{static_cast<uint32_t>(expr.immediate)};
    const il::ValueInfo& info = function_.value(value);
    if (!info.definition.valid() || function_.op(info.definition).code != il::OpCode::Phi) {
      return il::ExprId{};
    }
    const Key key{value.index(), static_cast<uint8_t>(code)};
    if (const auto found = cache_.find(key); found != cache_.end()) {
      return function_.valueRef(found->second);
    }
    const il::OpId phiOp = info.definition;
    const std::vector<il::ExprId> incoming(function_.operands(function_.op(phiOp)).begin(),
                                           function_.operands(function_.op(phiOp)).end());
    // A loop-carried flags phi's back edge names this same value: nothing
    // upstream can resolve it without re-executing the loop, so that edge's
    // arm could only ever be an honest FlagCond over `flags` again -- the
    // exact expression this call exists to replace. A phi built around one
    // arm that is just its own trigger restated is not distributing
    // anything; it is relabelling the original FlagCond. And unlike the memo
    // above (scoped to this one call), nothing about that relabelling
    // survives to the *next* ssa-optimize iteration -- a fresh
    // FlagPhiDistributor, its cache empty, meets the same unresolved
    // FlagCond (now one arm of the phi this call is about to build) and
    // builds another phi around it, forever: the fixed +1 phi and its whole
    // operand list per iteration this comment exists to stop. Refusing
    // before any phi is created is what leaves the original FlagCond alone
    // -- and alone, unlike its ever-lengthening replacement, it is already a
    // fixpoint.
    if (std::find(incoming.begin(), incoming.end(), flags) != incoming.end()) {
      return il::ExprId{};
    }
    // Cached before recursing: a merge of merges can reach the same (phi,
    // code) pair by more than one path, and a second arrival must find this
    // phi here rather than recurse into building it again.
    const il::ValueId newPhi =
        function_.prependPhi(info.block, function_.op(phiOp).va, il::Type::integer(1));
    cache_.emplace(key, newPhi);

    std::vector<il::ExprId> rewritten;
    rewritten.reserve(incoming.size());
    for (const il::ExprId in : incoming) {
      // Not a direct self-reference (ruled out above already) but still
      // unresolvable on its own terms -- a load, a call result, a flags
      // value from a register this rewrite does not track, or a deeper
      // phi with its own loop-carried edge. That costs only this one arm
      // its own honest FlagCond, not every other arm the phi merges.
      const il::ExprId arm = resolveRec(in, code, depth + 1);
      rewritten.push_back(arm.valid() ? arm : function_.flagCondition(in, code));
    }
    function_.setOperands(function_.value(newPhi).definition, rewritten);
    return function_.valueRef(newPhi);
  }

  il::Function& function_;
  std::unordered_map<Key, il::ValueId, KeyHash> cache_;
};

/// Same shape as foldFlagConditionsRec, extended with the phi case above.
il::ExprId distributeFlagCondRec(il::Function& function, il::ExprId id,
                                 std::unordered_map<il::ExprId, il::ExprId>& memo,
                                 FlagPhiDistributor& distributor) {
  if (const auto found = memo.find(id); found != memo.end()) {
    return found->second;
  }
  const il::Expr expr = function.expr(id);
  il::Expr rebuilt = expr;
  bool changed = false;
  for (unsigned index = 0; index < expr.operandCount; ++index) {
    const il::ExprId folded =
        distributeFlagCondRec(function, expr.operands[index], memo, distributor);
    changed |= folded != expr.operands[index];
    rebuilt.operands[index] = folded;
  }
  il::ExprId result = changed ? function.intern(rebuilt) : id;

  if (expr.op == il::ExprOp::FlagCond) {
    const il::Expr& cond = function.expr(result);
    const il::Expr& flags = function.expr(cond.operands[0]);
    if (flags.op != il::ExprOp::FlagDef) {
      if (const il::ExprId resolved = distributor.resolve(
              cond.operands[0], static_cast<il::ConditionCode>(cond.immediate));
          resolved.valid()) {
        result = resolved;
      }
    }
  }
  memo.emplace(id, result);
  return result;
}

/// Runs distributeFlagCondRec over every op operand. A separate driver from
/// foldOperands below (rather than reusing it) because prependPhi mutates the
/// block whose ops foldOperands' own loop is walking live; this one is used
/// nowhere else, so it owns its snapshot-first traversal outright instead of
/// making every foldOperands caller pay for a hazard only this one has.
bool distributeFlagCondThroughPhi(il::Function& function) {
  FlagPhiDistributor distributor(function);
  std::unordered_map<il::ExprId, il::ExprId> memo;
  bool changed = false;
  for (const il::BlockId blockId : function.blockHandles()) {
    // Snapshotted: prependPhi below inserts into a block's op list, which
    // would otherwise invalidate this loop mid-walk when that block is the
    // one being visited.
    const std::vector<il::OpId> ops(function.block(blockId).ops.begin(),
                                    function.block(blockId).ops.end());
    for (const il::OpId opId : ops) {
      const auto operands = function.operands(function.op(opId));
      if (operands.empty()) {
        continue;
      }
      std::vector<il::ExprId> rewritten(operands.begin(), operands.end());
      bool opChanged = false;
      for (il::ExprId& operand : rewritten) {
        const il::ExprId next = distributeFlagCondRec(function, operand, memo, distributor);
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
  const bool direct = foldOperands(function, [](il::Function& f, il::ExprId id) {
    return foldFlagConditions(f, id);
  });
  // Order does not matter for correctness -- the two patterns are disjoint,
  // matched on what the flags operand's own expression is -- but running the
  // cheap direct rewrite first means fewer FlagCond nodes are still standing
  // when the phi walk below looks for them.
  const bool distributed = distributeFlagCondThroughPhi(function);
  return direct || distributed;
}

}  // namespace xdec::passes
