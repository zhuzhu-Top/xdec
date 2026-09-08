// SymValueSet: the bounded value domain path-level execution computes with.
//
// analysis::ImageEval (image_eval.h) answers "what can this expression be,
// given that memory is the binary image" -- a single-shot, whole-function
// question with no notion of *where* execution is or what it has already
// done. PathExplorer (path_explorer.h) asks a different question: "walking
// this concrete edge of the CFG, having already executed these stores, what
// can this register/expression be here" -- and that needs its own value
// domain, because a path's memory is not just the image (see
// PathState::loadFrom in path_state.h).
//
// The domain itself is the same bounded-set-or-top discipline ImageEval's
// ValueSet established (the cap, the overflow-to-top rule, the
// undef-tolerant reading of an unbound leaf): a jump table selected between
// on two paths is a two-element set on each path individually, the same way
// it is a two-element union across both in ImageEval. Keeping a distinct type
// here, rather than reusing ValueSet directly, is deliberate: a path-local
// value's "I don't know" can later gain a *reason* (an unbound argument
// register versus an arithmetic overflow past the cap) that ValueSet's
// undifferentiated top has no room for, and that distinction is exactly the
// kind of thing a resolver -- not this domain -- would want to act on. No
// such reason exists yet (both degrade to top identically below), so nothing
// here is speculative; the type boundary just keeps that future free without
// touching ImageEval, which has no such distinction to make.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace xdec::analysis {

/// A bounded set of concrete values a path-local expression can hold, or top
/// ("could be anything"). Semantics mirror analysis::ValueSet exactly (same
/// cap, same overflow rule) -- see the header comment for why this is not
/// just that type.
class SymValueSet {
 public:
  static SymValueSet top() { return SymValueSet{}; }
  /// No possibilities at all (an unbound phi edge, say). Callers treat empty
  /// the same as top: not an answer.
  static SymValueSet empty() {
    SymValueSet set;
    set.top_ = false;
    return set;
  }
  static SymValueSet one(uint64_t value) {
    SymValueSet set = empty();
    set.values_.push_back(value);
    return set;
  }

  [[nodiscard]] bool isTop() const noexcept { return top_; }
  [[nodiscard]] std::span<const uint64_t> values() const noexcept { return values_; }

  /// Adds a value; exceeding the cap degrades the whole set to top.
  void insert(uint64_t value);
  /// Set union; top wins.
  void unite(const SymValueSet& other);

  /// Same cap as analysis::ValueSet, for the same reason (see its own
  /// comment): wide enough for a real table's fan-out, narrow enough to
  /// refuse a combinatorial blow-up rather than enumerate it.
  static constexpr std::size_t kCap = 16;

 private:
  bool top_ = true;  // default-constructed is top
  std::vector<uint64_t> values_;
};

}  // namespace xdec::analysis
