// Symbolic integers exist so that width polymorphism can be proved rather than
// assumed. These check the two properties the checker relies on: folding is
// aggressive enough that two spellings of one width are one node, and equality
// is never claimed where it has not been shown.
#include <catch2/catch_test_macros.hpp>

#include "xdec/spec/symint.h"

using xdec::spec::SymId;
using xdec::spec::SymOp;
using xdec::spec::SymPool;

TEST_CASE("literals fold", "[spec][symint]") {
  SymPool pool;
  const SymId thirtyTwo = pool.constant(32);
  const SymId one = pool.constant(1);

  const SymId shifted = pool.binary(SymOp::Shl, thirtyTwo, one);
  uint64_t value = 0;
  REQUIRE(pool.asConstant(shifted, value));
  CHECK(value == 64);

  // The whole point: `32 << 1` and `64` are the same node, so a rule declaring
  // `bits(32 << sf)` under `sf == 1` matches a concrete `bits(64)`.
  CHECK(shifted == pool.constant(64));
}

TEST_CASE("structure is shared", "[spec][symint]") {
  SymPool pool;
  const SymId sf = pool.symbol("sf");
  const SymId first = pool.binary(SymOp::Shl, pool.constant(32), sf);
  const SymId second = pool.binary(SymOp::Shl, pool.constant(32), sf);
  CHECK(first == second);
  CHECK(pool.provablyEqual(first, second));

  SECTION("different symbols are different nodes") {
    const SymId other = pool.binary(SymOp::Shl, pool.constant(32), pool.symbol("sz"));
    CHECK(first != other);
    CHECK_FALSE(pool.provablyEqual(first, other));
  }

  SECTION("commutative operands are ordered canonically") {
    const SymId a = pool.symbol("a");
    const SymId b = pool.symbol("b");
    CHECK(pool.binary(SymOp::Add, a, b) == pool.binary(SymOp::Add, b, a));
    // Subtraction is not commutative and must not be reordered.
    CHECK(pool.binary(SymOp::Sub, a, b) != pool.binary(SymOp::Sub, b, a));
  }
}

TEST_CASE("identities fold without knowing the symbol", "[spec][symint]") {
  SymPool pool;
  const SymId x = pool.symbol("x");
  CHECK(pool.binary(SymOp::Add, x, pool.constant(0)) == x);
  CHECK(pool.binary(SymOp::Mul, x, pool.constant(1)) == x);
  CHECK(pool.binary(SymOp::Shl, x, pool.constant(0)) == x);
  CHECK(pool.binary(SymOp::Sub, x, x) == pool.constant(0));
  CHECK(pool.binary(SymOp::Equal, x, x) == pool.constant(1));

  SECTION("but a non-identity is left alone") {
    CHECK_FALSE(pool.isConstant(pool.binary(SymOp::Add, x, pool.constant(1))));
    // `x - 0` folds, `0 - x` does not.
    CHECK(pool.binary(SymOp::Sub, pool.constant(0), x) != x);
  }
}

TEST_CASE("division by zero is left unfolded", "[spec][symint]") {
  SymPool pool;
  // Inventing a value here would let a spec bug through as a plausible width.
  const SymId quotient = pool.binary(SymOp::DivU, pool.constant(8), pool.constant(0));
  CHECK_FALSE(pool.isConstant(quotient));
}

TEST_CASE("an unknown is never equal, even to itself", "[spec][symint]") {
  SymPool pool;
  const SymId unknown = pool.unknown();
  CHECK(pool.isUnknown(unknown));
  CHECK_FALSE(pool.provablyEqual(unknown, unknown));

  // Unknown is contagious: anything derived from an unmodelled value is itself
  // unmodelled, rather than being silently treated as equal to something.
  CHECK(pool.isUnknown(pool.binary(SymOp::Add, unknown, pool.constant(1))));
  CHECK(pool.isUnknown(pool.unary(SymOp::Not, unknown)));
}

TEST_CASE("substitution refolds", "[spec][symint]") {
  SymPool pool;
  const SymId sf = pool.symbol("sf");
  const SymId width = pool.binary(SymOp::Shl, pool.constant(32), sf);

  const SymId wide = pool.substitute(width, "sf", 1);
  uint64_t value = 0;
  REQUIRE(pool.asConstant(wide, value));
  CHECK(value == 64);

  const SymId narrow = pool.substitute(width, "sf", 0);
  REQUIRE(pool.asConstant(narrow, value));
  CHECK(value == 32);

  SECTION("an unrelated symbol is untouched") {
    CHECK(pool.substitute(width, "sz", 1) == width);
  }

  SECTION("substitution reaches into nested nodes") {
    const SymId nested =
        pool.binary(SymOp::Add, pool.binary(SymOp::Mul, sf, pool.constant(8)),
                    pool.constant(2));
    REQUIRE(pool.asConstant(pool.substitute(nested, "sf", 3), value));
    CHECK(value == 26);
  }
}

TEST_CASE("select folds on a known condition", "[spec][symint]") {
  SymPool pool;
  const SymId a = pool.constant(32);
  const SymId b = pool.constant(64);
  CHECK(pool.select(pool.constant(1), a, b) == a);
  CHECK(pool.select(pool.constant(0), a, b) == b);
  // Identical arms make the condition irrelevant.
  CHECK(pool.select(pool.symbol("c"), a, a) == a);
  CHECK_FALSE(pool.isConstant(pool.select(pool.symbol("c"), a, b)));
}

TEST_CASE("printing is readable", "[spec][symint]") {
  SymPool pool;
  const SymId width = pool.binary(SymOp::Shl, pool.constant(32), pool.symbol("sf"));
  CHECK(pool.toString(width) == "(32 << sf)");
  CHECK(pool.toString(pool.constant(64)) == "64");
  CHECK(pool.toString(pool.unknown()) == "?");
}
