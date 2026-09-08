// SymValueSet: the bounded value domain PathState computes with -- see
// sym_value.h for why this exists next to analysis::ValueSet rather than
// reusing it.
#include <catch2/catch_test_macros.hpp>

#include "xdec/analysis/sym_value.h"

using xdec::analysis::SymValueSet;

TEST_CASE("top and empty are distinct from any concrete set", "[analysis][sym-value]") {
  const SymValueSet top = SymValueSet::top();
  CHECK(top.isTop());

  const SymValueSet empty = SymValueSet::empty();
  CHECK_FALSE(empty.isTop());
  CHECK(empty.values().empty());

  const SymValueSet one = SymValueSet::one(0x1234);
  CHECK_FALSE(one.isTop());
  REQUIRE(one.values().size() == 1);
  CHECK(one.values()[0] == 0x1234);
}

TEST_CASE("insert de-duplicates and degrades to top past the cap",
          "[analysis][sym-value]") {
  SymValueSet set = SymValueSet::empty();
  set.insert(1);
  set.insert(1);
  REQUIRE(set.values().size() == 1);

  for (uint64_t value = 0; value < SymValueSet::kCap; ++value) {
    set.insert(value + 100);
  }
  // The pre-existing {1} plus kCap fresh values crosses the cap.
  CHECK(set.isTop());
}

TEST_CASE("unite is a set union, top absorbing", "[analysis][sym-value]") {
  SymValueSet a = SymValueSet::one(1);
  SymValueSet b = SymValueSet::one(2);
  a.unite(b);
  REQUIRE(!a.isTop());
  REQUIRE(a.values().size() == 2);

  SymValueSet c = SymValueSet::one(3);
  c.unite(SymValueSet::top());
  CHECK(c.isTop());

  SymValueSet d = SymValueSet::top();
  d.unite(SymValueSet::one(4));
  CHECK(d.isTop());
}
