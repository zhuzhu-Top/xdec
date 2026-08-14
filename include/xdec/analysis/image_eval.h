// Image-backed expression evaluation: what concrete values can this
// expression take, given that memory is the binary image?
//
// The constant evaluator (il/ceval.h) answers "is this expression a
// compile-time constant". This evaluator answers a richer question with a
// bounded set: a jump table address is `select(cond, tableA, tableB)` over an
// unanalysable condition — not a constant, but not unknown either; it is one
// of two pointers, and both are in the image. That answer is exactly what
// indirect-branch resolution needs.
//
// The discipline:
//   - Bounded. Sets cap at kCap values; overflow degrades to top ("unknown"),
//     never to wrong answers. A resolver that sees top does not resolve.
//   - Undef-tolerant. Undef always evaluates to top; EntryReg does too unless
//     an EntryRegFacts binds it to a platform-fixed value (see
//     analysis/entry_reg.h), in which case it is that value, a singleton set,
//     same as any other constant. Top propagates by the rules of the
//     operation (top + 3 is top; select(top, a, b) is a ∪ b). Nothing throws,
//     nothing aborts.
//   - Memory is the image. Loads read through the ByteReader; an unmapped
//     address is top, never a zero-fill.
#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "xdec/analysis/entry_reg.h"
#include "xdec/il/function.h"
#include "xdec/support/reader.h"

namespace xdec::analysis {

/// A bounded set of concrete values, or top = "could be anything".
class ValueSet {
 public:
  static ValueSet top() { return ValueSet{}; }
  /// No possibilities at all (a phi with no inputs). Resolvers treat empty
  /// the same as top: not an answer.
  static ValueSet empty() {
    ValueSet set;
    set.top_ = false;
    return set;
  }
  static ValueSet one(uint64_t value) {
    ValueSet set = empty();
    set.values_.push_back(value);
    return set;
  }

  [[nodiscard]] bool isTop() const noexcept { return top_; }
  /// Singleton accessor; only valid when !isTop() and size() == 1.
  [[nodiscard]] std::span<const uint64_t> values() const noexcept { return values_; }

  /// Adds a value; exceeding the cap degrades the whole set to top.
  void insert(uint64_t value);
  /// set union; top wins.
  void unite(const ValueSet& other);

  /// Above this many possibilities a resolver stops enumerating. Sixteen
  /// covers real jump tables' fan-out here without inviting combinatorial
  /// blow-ups on widened cross-products.
  static constexpr std::size_t kCap = 16;

 private:
  bool top_ = true;  // default-constructed is top
  std::vector<uint64_t> values_;
};

class ImageEval {
 public:
  /// `entryRegs` is what turns an `EntryReg` leaf from top into a value where
  /// the platform, not the image, is the one that fixes it (dyld's leaked
  /// x21/x22, say -- see analysis/entry_reg.h). Absent (the default) keeps
  /// every EntryReg leaf top, exactly as before this parameter existed.
  ImageEval(const il::Function& function, ByteReader reader,
            const EntryRegFacts* entryRegs = nullptr)
      : function_(function), reader_(std::move(reader)), entryRegs_(entryRegs) {}

  /// The values `id` can take, within the set bound. Memoised per call site;
  /// phi cycles contribute the empty set at the re-entry point (the cyclic
  /// edge adds no information), which keeps loop phis at their seed values
  /// instead of degrading the whole phi to top.
  [[nodiscard]] ValueSet eval(il::ExprId id);

 private:
  [[nodiscard]] ValueSet evalValue(il::ValueId id);
  [[nodiscard]] ValueSet evalUnary(const il::Expr& expr);
  [[nodiscard]] ValueSet evalBinary(const il::Expr& expr);
  [[nodiscard]] ValueSet evalSelect(const il::Expr& expr);
  [[nodiscard]] ValueSet evalCast(const il::Expr& expr);
  [[nodiscard]] ValueSet evalEntryReg(const il::Expr& expr);
  [[nodiscard]] ValueSet loadFrom(const ValueSet& addresses, il::Type type);

  /// True when `id` is a bare `EntryReg` leaf -- a platform-leaked register
  /// read with no transform at all, as opposed to an expression merely
  /// *derived* from one (a cast, an add). See unionEntryRegAware for why the
  /// distinction matters.
  [[nodiscard]] bool isRawEntryReg(il::ExprId id) const;

  /// The union a phi or select's arms would get anyway, except that a bare
  /// EntryReg leaf sitting next to a genuinely computed arm is excluded
  /// first. A merge with one predecessor that never touched the register and
  /// another that assigned it a real value is not "the value is one of
  /// these two very different things" -- it is the untouched predecessor
  /// contributing nothing, the same shape resolve_indirect.cpp's own
  /// `tolerateDeadCombinations` already treats as a combination this call
  /// site cannot reach, one layer further up. When every arm is a bare
  /// EntryReg leaf (or there is only one arm), nothing is excluded: that is
  /// an ordinary entry-only merge, not a stale one.
  [[nodiscard]] ValueSet unionEntryRegAware(std::span<const il::ExprId> arms);

  /// Cross-product application with the cap as the budget: when |a|×|b|
  /// would exceed the cap, the answer is top rather than a partial set.
  template <class F>
  [[nodiscard]] ValueSet cross(const ValueSet& a, const ValueSet& b, F&& apply,
                               unsigned width);
  template <class F>
  [[nodiscard]] ValueSet map(const ValueSet& a, F&& apply, unsigned width);

  const il::Function& function_;
  ByteReader reader_;
  const EntryRegFacts* entryRegs_ = nullptr;
  std::unordered_map<il::ExprId, ValueSet> memo_;
  std::unordered_map<il::ValueId, ValueSet> valueMemo_;
  /// Re-entered evaluations (phi loops) contribute the empty set at the inner
  /// visit.
  std::unordered_map<il::ExprId, bool> active_;
  unsigned depth_ = 0;
  /// Recursion bound: deep substituted chains concede top, not the stack.
  static constexpr unsigned kMaxDepth = 256;
};

}  // namespace xdec::analysis
