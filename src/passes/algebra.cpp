// simplifyAlgebra: the local rule tier and the walk that drives it (see the
// header for the tiers; the multi-node shapes live in algebra_idioms.cpp).
#include "algebra.h"

#include <unordered_map>

#include "xdec/il/ceval.h"
#include "xdec/support/bits.h"
#include "xdec/support/log.h"

#include "algebra_idioms.h"

namespace xdec::passes {

// Walk statistics for simplifyAlgebra. Set XDEC_LOG=algebra=debug.
XDEC_DEFINE_LOG_CATEGORY(algebraLog, "algebra")

namespace {

/// Small predicates the rules read better through.
struct Pred {
  il::Function& function;

  [[nodiscard]] bool isConst(il::ExprId id, uint64_t value) const {
    uint64_t actual = 0;
    return function.asConstant(id, actual) && actual == value;
  }
  [[nodiscard]] bool isZero(il::ExprId id) const { return isConst(id, 0); }
  [[nodiscard]] bool isAllOnes(il::ExprId id) const {
    const il::Type type = function.expr(id).type;
    if (!type.isScalarInteger()) {
      return false;
    }
    const uint64_t allOnes =
        type.bits() >= 64 ? ~uint64_t{0} : zeroExtend(~uint64_t{0}, type.bits());
    return isConst(id, allOnes);
  }
  [[nodiscard]] bool constant(il::ExprId id, uint64_t& out) const {
    return function.asConstant(id, out);
  }
};

/// The rewriter: bottom-up with memoization, children simplified before
/// their parent sees them, local rules per node applied to a fixpoint.
class Algebra {
 public:
  explicit Algebra(il::Function& function) : function_(function), pred_{function} {}

  struct Stats {
    std::size_t entries = 0;
    std::size_t memoHits = 0;
    std::size_t interned = 0;
  };

  [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

  [[nodiscard]] il::ExprId simplify(il::ExprId id, unsigned depth = 0) {
    ++stats_.entries;
    if (const auto found = memo_.find(id); found != memo_.end()) {
      ++stats_.memoHits;
      return found->second;
    }
    // Deep substituted chains outgrow any call stack: past the bound, leave
    // the node alone rather than overflow. Simplification skipped is IL kept.
    if (depth >= kMaxDepth) {
      return id;
    }
    // By value: rebuilding interns into the pool, which dangles references.
    const il::Expr expr = function_.expr(id);
    il::ExprId result = id;
    if (expr.operandCount > 0) {
      il::Expr rebuilt = expr;
      bool changed = false;
      for (unsigned index = 0; index < expr.operandCount; ++index) {
        const il::ExprId next = simplify(expr.operands[index], depth + 1);
        changed |= next != expr.operands[index];
        rebuilt.operands[index] = next;
      }
      if (changed) {
        result = function_.intern(rebuilt);
        ++stats_.interned;
      }
      // The node's own rules, to a fixpoint: one rewrite may enable the next
      // (an MBA match leaves an add; the add may reassociate).
      result = rewriteToFixpoint(result);
    }
    memo_.emplace(id, result);
    return result;
  }

 private:
  // -- the rule loop ----------------------------------------------------------

  [[nodiscard]] il::ExprId rewriteToFixpoint(il::ExprId id) {
    for (unsigned round = 0; round < kMaxRounds; ++round) {
      const il::ExprId next = rewriteOnce(id);
      if (next == id) {
        return id;
      }
      id = next;
    }
    return id;  // a rule cycle would spin forever; the cap keeps it a missed rule
  }

  [[nodiscard]] il::ExprId rewriteOnce(il::ExprId id) {
    const il::Expr expr = function_.expr(id);  // by value, as ever

    // Fully constant nodes fold. foldConstants does this at Local level, but
    // SSA substitution builds NEW trees after that pass ran; folding here is
    // what lets the identities below see rebuilt constants (trunc(const 0),
    // then and(x, 0), and the chain they unlock). Integers only: a boolean or
    // flags node has no constant form to fold into.
    if (expr.operandCount > 0 && expr.op != il::ExprOp::Const &&
        expr.type.isScalarInteger() && expr.type.bits() <= 64) {
      il::ConcreteValue value;
      if (il::tryEvalConst(function_, id, value)) {
        return function_.constant(expr.type, value.lo);
      }
    }

    switch (expr.op) {
      case il::ExprOp::Add: return rewriteAdd(expr);
      case il::ExprOp::Sub: return rewriteSub(expr);
      case il::ExprOp::Mul: return rewriteMul(expr);
      case il::ExprOp::And: return rewriteAnd(expr);
      case il::ExprOp::Or: return rewriteOr(expr);
      case il::ExprOp::Xor: return rewriteXor(expr);
      case il::ExprOp::Not: return rewriteNot(expr);
      case il::ExprOp::Neg: return rewriteNeg(expr);
      case il::ExprOp::Shl:
      case il::ExprOp::ShrU:
      case il::ExprOp::ShrS:
      case il::ExprOp::RotR:
      case il::ExprOp::RotL: return rewriteShift(expr);
      case il::ExprOp::CmpEq:
      case il::ExprOp::CmpNe:
      case il::ExprOp::CmpLtU:
      case il::ExprOp::CmpLeU:
      case il::ExprOp::CmpLtS:
      case il::ExprOp::CmpLeS: return rewriteCompare(expr, id);
      case il::ExprOp::Select: return rewriteSelect(expr);
      case il::ExprOp::FlagCond: return rewriteFlagCond(expr);
      case il::ExprOp::ZExt:
      case il::ExprOp::SExt:
      case il::ExprOp::Trunc:
      case il::ExprOp::Extract: return rewriteCast(expr, id);
      default: return id;
    }
  }

  [[nodiscard]] il::ExprId unchanged(const il::Expr& expr) const {
    // The node as it stands: re-interned only if children moved.
    return function_.intern(expr);
  }

  // -- commutative normalisation ----------------------------------------------
  //
  // Const migrates right, so every rule below matches one shape instead of
  // two. Returns true when the operands were swapped.

  void normaliseCommutative(il::Expr& expr) const {
    const bool leftConst = function_.expr(expr.operands[0]).op == il::ExprOp::Const;
    const bool rightConst = function_.expr(expr.operands[1]).op == il::ExprOp::Const;
    if (leftConst && !rightConst) {
      std::swap(expr.operands[0], expr.operands[1]);
    }
  }

  // -- arithmetic ---------------------------------------------------------------

  [[nodiscard]] il::ExprId rewriteAdd(il::Expr expr) {
    normaliseCommutative(expr);
    const il::ExprId x = expr.operands[0];
    const il::ExprId y = expr.operands[1];
    uint64_t k = 0;

    if (const il::ExprId mba = matchMbaAdd(function_, expr); mba.valid()) {
      return mba;
    }
    // MBA inverted: ~x + 1 → -x.
    if (pred_.isConst(y, 1) && function_.expr(x).op == il::ExprOp::Not) {
      return function_.unary(il::ExprOp::Neg, function_.expr(x).operands[0]);
    }
    if (pred_.isZero(y)) {
      return x;
    }
    // A negation on either side of an add is a subtraction, and reads as one:
    // `(-x) + k` is `k - x`. Obfuscators emit the negated form constantly, and
    // it is also what the shift-and-negate MBA rewrites leave behind.
    for (const int arm : {0, 1}) {
      const il::ExprId negated = expr.operands[arm];
      if (function_.expr(negated).op == il::ExprOp::Neg) {
        return function_.binary(il::ExprOp::Sub, expr.operands[arm ^ 1],
                                function_.expr(negated).operands[0]);
      }
    }
    // (x + k1) + k2 → x + (k1+k2), and the sub variant.
    if (pred_.constant(y, k) && function_.expr(x).operandCount == 2) {
      const il::Expr inner = function_.expr(x);
      uint64_t k1 = 0;
      if ((inner.op == il::ExprOp::Add || inner.op == il::ExprOp::Sub) &&
          pred_.constant(inner.operands[1], k1)) {
        const uint64_t sum =
            inner.op == il::ExprOp::Add ? k1 + k : k - k1;  // sub(x,k1)+k2 = x+(k2-k1)
        const uint64_t folded = zeroExtend(sum, expr.type.bits());
        return function_.binary(il::ExprOp::Add, inner.operands[0],
                                function_.constant(expr.type, folded));
      }
      // (k1 - x) + k2 → (k1+k2) - x: the constant-on-the-left subtraction,
      // which commutative normalisation cannot reach because subtraction does
      // not commute. Chains of these are how a single `seed - x` ends up spread
      // across four nested nodes.
      if (inner.op == il::ExprOp::Sub && pred_.constant(inner.operands[0], k1)) {
        return function_.binary(
            il::ExprOp::Sub,
            function_.constant(expr.type, zeroExtend(k1 + k, expr.type.bits())),
            inner.operands[1]);
      }
    }
    return unchanged(expr);
  }

  [[nodiscard]] il::ExprId rewriteSub(il::Expr expr) {
    const il::ExprId x = expr.operands[0];
    const il::ExprId y = expr.operands[1];
    uint64_t k = 0;

    if (const il::ExprId mba = matchMbaSub(function_, expr); mba.valid()) {
      return mba;
    }
    if (pred_.isZero(y)) {
      return x;
    }
    if (x == y) {
      return function_.constant(expr.type, 0);
    }
    // Subtracting a negation is adding, and zero minus something is its
    // negation. Both spellings arise from the same place: an obfuscator that
    // rewrote an add as `a + (-b)` and then folded one side to zero.
    if (function_.expr(y).op == il::ExprOp::Neg) {
      return function_.binary(il::ExprOp::Add, x, function_.expr(y).operands[0]);
    }
    if (pred_.isZero(x)) {
      return function_.unary(il::ExprOp::Neg, y);
    }
    // (x - k1) - k2 → x - (k1+k2); (x + k1) - k2 → x + (k1-k2);
    // (k1 - x) - k2 → (k1-k2) - x.
    if (pred_.constant(y, k) && function_.expr(x).operandCount == 2) {
      const il::Expr inner = function_.expr(x);
      uint64_t k1 = 0;
      if (inner.op == il::ExprOp::Sub && pred_.constant(inner.operands[1], k1)) {
        return function_.binary(il::ExprOp::Sub, inner.operands[0],
                                function_.constant(expr.type, zeroExtend(k1 + k, expr.type.bits())));
      }
      if (inner.op == il::ExprOp::Add && pred_.constant(inner.operands[1], k1)) {
        return function_.binary(il::ExprOp::Add, inner.operands[0],
                                function_.constant(expr.type, zeroExtend(k1 - k, expr.type.bits())));
      }
      if (inner.op == il::ExprOp::Sub && pred_.constant(inner.operands[0], k1)) {
        return function_.binary(il::ExprOp::Sub,
                                function_.constant(expr.type, zeroExtend(k1 - k, expr.type.bits())),
                                inner.operands[1]);
      }
    }
    // k1 - (x ± k2) → (k1∓k2) - x. The mirror of the rules above, for the side
    // subtraction's non-commutativity leaves unreachable to normalisation.
    if (pred_.constant(x, k) && function_.expr(y).operandCount == 2) {
      const il::Expr inner = function_.expr(y);
      uint64_t k2 = 0;
      if (inner.op == il::ExprOp::Sub && pred_.constant(inner.operands[1], k2)) {
        return function_.binary(il::ExprOp::Sub,
                                function_.constant(expr.type, zeroExtend(k + k2, expr.type.bits())),
                                inner.operands[0]);
      }
      if (inner.op == il::ExprOp::Add && pred_.constant(inner.operands[1], k2)) {
        return function_.binary(il::ExprOp::Sub,
                                function_.constant(expr.type, zeroExtend(k - k2, expr.type.bits())),
                                inner.operands[0]);
      }
    }
    return unchanged(expr);
  }

  [[nodiscard]] il::ExprId rewriteMul(il::Expr expr) {
    normaliseCommutative(expr);
    const il::ExprId x = expr.operands[0];
    const il::ExprId y = expr.operands[1];
    if (pred_.isZero(y)) {
      return function_.constant(expr.type, 0);
    }
    if (pred_.isConst(y, 1)) {
      return x;
    }
    uint64_t k2 = 0;
    if (pred_.constant(y, k2) && function_.expr(x).op == il::ExprOp::Mul) {
      const il::Expr inner = function_.expr(x);
      uint64_t k1 = 0;
      if (pred_.constant(inner.operands[1], k1)) {
        return function_.binary(il::ExprOp::Mul, inner.operands[0],
                                function_.constant(expr.type, zeroExtend(k1 * k2, expr.type.bits())));
      }
    }
    return unchanged(expr);
  }

  // -- bitwise ------------------------------------------------------------------

  [[nodiscard]] il::ExprId rewriteAnd(il::Expr expr) {
    normaliseCommutative(expr);
    const il::ExprId x = expr.operands[0];
    const il::ExprId y = expr.operands[1];
    if (pred_.isZero(y)) {
      return function_.constant(expr.type, 0);
    }
    if (pred_.isAllOnes(y) || x == y) {
      return x;
    }
    uint64_t k2 = 0;
    if (pred_.constant(y, k2) && function_.expr(x).op == il::ExprOp::And) {
      const il::Expr inner = function_.expr(x);
      uint64_t k1 = 0;
      if (pred_.constant(inner.operands[1], k1)) {
        return function_.binary(il::ExprOp::And, inner.operands[0],
                                function_.constant(expr.type, k1 & k2));
      }
    }
    // Opaque-predicate fuel: (v*(v±1)) & 1 is always zero — a product of
    // consecutive integers is even. Flattened dispatchers hide their state
    // constants behind this identity.
    if (pred_.constant(y, k2) && k2 == 1 && consecutiveProduct(x)) {
      return function_.constant(expr.type, 0);
    }
    // A mask over a rotate or a shift: the rotate loses the half the mask
    // discards, and the mask itself goes when it covers everything the shift
    // could have produced. Order matters — the rotate rewrite leaves a masked
    // shift, which the second rule then strips.
    if (const il::ExprId shift = matchMaskedRotate(function_, expr); shift.valid()) {
      return shift;
    }
    if (const il::ExprId shift = matchMaskedShift(function_, expr); shift.valid()) {
      return shift;
    }
    // The errno idiom's `hi` half, once fold.cpp's lazy-flag folding has
    // turned `cset hi` into `(sum <u a) & (sum != 0)`.
    if (const il::ExprId carry = matchCarryCompare(function_, expr); carry.valid()) {
      return carry;
    }
    return unchanged(expr);
  }

  /// v*(v±1) with hash-consed operand equality: the classic even-product
  /// opaque identity, in either association the obfuscators emit.
  [[nodiscard]] bool consecutiveProduct(il::ExprId id) const {
    const il::Expr& expr = function_.expr(id);
    if (expr.op != il::ExprOp::Mul) {
      return false;
    }
    for (const int arm : {0, 1}) {
      const il::Expr& factor = function_.expr(expr.operands[arm]);
      if (factor.op != il::ExprOp::Sub && factor.op != il::ExprOp::Add) {
        continue;
      }
      uint64_t k = 0;
      if (pred_.constant(factor.operands[1], k) && k == 1 &&
          factor.operands[0] == expr.operands[arm ^ 1]) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] il::ExprId rewriteOr(il::Expr expr) {
    normaliseCommutative(expr);
    const il::ExprId x = expr.operands[0];
    const il::ExprId y = expr.operands[1];
    if (pred_.isZero(y) || x == y) {
      return x;
    }
    if (pred_.isAllOnes(y)) {
      return y;
    }
    // The sign-extension idiom: an or of a spread sign bit and a masked field.
    if (const il::ExprId sext = matchSignExtend(function_, expr); sext.valid()) {
      return sext;
    }
    if (const il::ExprId mba = matchMbaOr(function_, expr); mba.valid()) {
      return mba;
    }
    // The errno idiom's `ls` half, the negation of the `hi` half above.
    if (const il::ExprId carry = matchCarryCompareOr(function_, expr); carry.valid()) {
      return carry;
    }
    return unchanged(expr);
  }

  [[nodiscard]] il::ExprId rewriteXor(il::Expr expr) {
    normaliseCommutative(expr);
    const il::ExprId x = expr.operands[0];
    const il::ExprId y = expr.operands[1];
    if (pred_.isZero(y)) {
      return x;
    }
    if (x == y) {
      return function_.constant(expr.type, 0);
    }
    if (pred_.isAllOnes(y)) {
      return function_.unary(il::ExprOp::Not, x);
    }
    if (const il::ExprId mba = matchMbaXor(function_, expr); mba.valid()) {
      return mba;
    }
    return unchanged(expr);
  }

  [[nodiscard]] il::ExprId rewriteNot(il::Expr expr) {
    const il::Expr& inner = function_.expr(expr.operands[0]);
    if (inner.op == il::ExprOp::Not) {
      return inner.operands[0];
    }
    // ~(-x) → x - 1. Two's complement: `~v` is `-v - 1`, so negating first and
    // complementing after cancels the negation and leaves the bias. This is the
    // other half of the `~x + 1 → -x` rule, and samples chain the two — a
    // `seed - x` written as `(seed - ~(-x)) - 1` is three nodes of nothing.
    if (inner.op == il::ExprOp::Neg) {
      return function_.binary(il::ExprOp::Sub, inner.operands[0],
                              function_.constant(expr.type, 1));
    }
    return unchanged(expr);
  }

  [[nodiscard]] il::ExprId rewriteNeg(il::Expr expr) {
    const il::Expr& inner = function_.expr(expr.operands[0]);
    if (inner.op == il::ExprOp::Neg) {
      return inner.operands[0];
    }
    // -(~x) → x + 1, the same identity read the other way round.
    if (inner.op == il::ExprOp::Not) {
      return function_.binary(il::ExprOp::Add, inner.operands[0],
                              function_.constant(expr.type, 1));
    }
    return unchanged(expr);
  }

  [[nodiscard]] il::ExprId rewriteShift(il::Expr expr) {
    // A shift or rotate by zero is filler; obfuscators sprinkle them freely.
    if (pred_.isZero(expr.operands[1])) {
      return expr.operands[0];
    }
    return unchanged(expr);
  }

  // -- select and casts -----------------------------------------------------------

  [[nodiscard]] il::ExprId rewriteSelect(il::Expr expr) {
    uint64_t condition = 0;
    if (pred_.constant(expr.operands[0], condition)) {
      return expr.operands[condition != 0 ? 1 : 2];
    }
    if (expr.operands[1] == expr.operands[2]) {
      return expr.operands[1];
    }
    return unchanged(expr);
  }

  /// Flag conditions over bundles whose contents are known: a literal NZCV
  /// constant, and a logical operation's cleared C/V bits. This is what
  /// collapses an opaque predicate's flag select into its constant arm.
  [[nodiscard]] il::ExprId rewriteFlagCond(il::Expr expr) {
    const il::Expr& def = function_.expr(expr.operands[0]);
    if (def.op != il::ExprOp::FlagDef) {
      return unchanged(expr);
    }
    const auto code = static_cast<il::ConditionCode>(expr.immediate);
    const il::FlagOp flagOp = il::flagDefOp(def.immediate);
    uint64_t nzcv = 0;
    if (flagOp == il::FlagOp::Const && pred_.constant(def.operands[0], nzcv)) {
      return function_.constant(expr.type, conditionValue(code, nzcv) ? 1 : 0);
    }
    if (flagOp == il::FlagOp::Logical) {
      // A logical flag write clears C and V.
      if (code == il::ConditionCode::CarrySet || code == il::ConditionCode::Overflow) {
        return function_.constant(expr.type, 0);
      }
      if (code == il::ConditionCode::CarryClear ||
          code == il::ConditionCode::NoOverflow) {
        return function_.constant(expr.type, 1);
      }
    }
    return unchanged(expr);
  }

  /// The NZCV truth table, mirroring the emitter's copy.
  [[nodiscard]] static bool conditionValue(il::ConditionCode code, uint64_t nzcv) {
    const bool n = (nzcv >> 3) & 1, z = (nzcv >> 2) & 1, c = (nzcv >> 1) & 1,
               v = nzcv & 1;
    switch (code) {
      case il::ConditionCode::Equal: return z;
      case il::ConditionCode::NotEqual: return !z;
      case il::ConditionCode::CarrySet: return c;
      case il::ConditionCode::CarryClear: return !c;
      case il::ConditionCode::Negative: return n;
      case il::ConditionCode::NonNegative: return !n;
      case il::ConditionCode::Overflow: return v;
      case il::ConditionCode::NoOverflow: return !v;
      case il::ConditionCode::UnsignedGreater: return c && !z;
      case il::ConditionCode::UnsignedLessEqual: return !c || z;
      case il::ConditionCode::SignedGreaterEqual: return n == v;
      case il::ConditionCode::SignedLess: return n != v;
      case il::ConditionCode::SignedGreater: return !z && n == v;
      case il::ConditionCode::SignedLessEqual: return z || n != v;
      case il::ConditionCode::Always: return true;
      case il::ConditionCode::Never:
      case il::ConditionCode::Count: return false;
    }
    return false;
  }

  /// Comparisons are not normalised commutatively — swapping their operands
  /// changes which way an ordered one points — so the one rule here is written
  /// to look at both sides itself.
  [[nodiscard]] il::ExprId rewriteCompare(const il::Expr& expr, il::ExprId id) {
    if (const il::ExprId narrowed = matchShiftedCompare(function_, expr);
        narrowed.valid()) {
      return narrowed;
    }
    if (const il::ExprId cancelled = matchCancelledSubtrahend(function_, expr);
        cancelled.valid()) {
      return cancelled;
    }
    return id;
  }

  [[nodiscard]] il::ExprId rewriteCast(il::Expr expr, il::ExprId id) {
    const il::ExprId source = expr.operands[0];
    const il::Type sourceType = function_.expr(source).type;
    switch (expr.op) {
      case il::ExprOp::ZExt:
      case il::ExprOp::SExt:
        // Widening from the same width is no cast at all.
        if (sourceType == expr.type) {
          return source;
        }
        break;
      case il::ExprOp::Trunc:
        if (sourceType == expr.type) {
          return source;
        }
        // trunc(zext(x)) → x when the round trip is width-exact.
        if (function_.expr(source).op == il::ExprOp::ZExt) {
          const il::ExprId inner = function_.expr(source).operands[0];
          if (function_.expr(inner).type == expr.type) {
            return inner;
          }
        }
        break;
      case il::ExprOp::Extract:
        // extract(x, 0) at a narrower width is a trunc by another name, and
        // naming it so lets the trunc rules above see it.
        if (expr.immediate == 0 && expr.type.bits() < sourceType.bits()) {
          return function_.cast(il::ExprOp::Trunc, expr.type, source);
        }
        break;
      default:
        break;
    }
    return id;
  }

  static constexpr unsigned kMaxRounds = 8;
  /// Recursion bound for tree depth (see simplify's guard).
  static constexpr unsigned kMaxDepth = 512;

  il::Function& function_;
  Pred pred_;
  Stats stats_;
  std::unordered_map<il::ExprId, il::ExprId> memo_;
};

}  // namespace

il::ExprId simplifyAlgebra(il::Function& function, il::ExprId id) {
  return Algebra(function).simplify(id);
}

bool simplifyAlgebra(il::Function& function) {
  const std::size_t exprBefore = function.exprCount();
  Algebra algebra(function);
  bool changed = false;
  std::size_t opsTouched = 0;
  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      const auto operands = function.operands(function.op(opId));
      if (operands.empty()) {
        continue;
      }
      std::vector<il::ExprId> rewritten(operands.begin(), operands.end());
      bool touched = false;
      for (il::ExprId& operand : rewritten) {
        const il::ExprId next = algebra.simplify(operand);
        touched |= next != operand;
        operand = next;
      }
      if (touched) {
        function.setOperands(opId, rewritten);
        changed = true;
        ++opsTouched;
      }
    }
  }
  const Algebra::Stats& stats = algebra.stats();
  XDEC_LOG_DEBUG(
      algebraLog(),
      "{} op(s) touched, {} simplify walk(s) ({} memo hit(s)), {} intern(s), {} -> {} expr(s)",
      opsTouched, stats.entries, stats.memoHits, stats.interned, exprBefore, function.exprCount());
  return changed;
}

}  // namespace xdec::passes
