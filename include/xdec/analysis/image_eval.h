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
//   - Undef-tolerant. Undef and EntryReg evaluate to top, and top propagates
//     by the rules of the operation (top + 3 is top; select(top, a, b) is
//     a ∪ b). Nothing throws, nothing aborts.
//   - Memory is the image. Loads read through the ByteReader; an unmapped
//     address is top, never a zero-fill.
#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

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
  ImageEval(const il::Function& function, ByteReader reader)
      : function_(function), reader_(std::move(reader)) {}

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
  [[nodiscard]] ValueSet loadFrom(const ValueSet& addresses, il::Type type);

  /// Cross-product application with the cap as the budget: when |a|×|b|
  /// would exceed the cap, the answer is top rather than a partial set.
  template <class F>
  [[nodiscard]] ValueSet cross(const ValueSet& a, const ValueSet& b, F&& apply,
                               unsigned width);
  template <class F>
  [[nodiscard]] ValueSet map(const ValueSet& a, F&& apply, unsigned width);

  const il::Function& function_;
  ByteReader reader_;
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
