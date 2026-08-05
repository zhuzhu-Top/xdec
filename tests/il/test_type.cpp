#include <catch2/catch_test_macros.hpp>

#include "xdec/il/maturity.h"
#include "xdec/il/type.h"

using xdec::il::Maturity;
using xdec::il::Type;
using xdec::il::TypeKind;

TEST_CASE("types describe machine values only", "[il][type]") {
  SECTION("integers carry a width and a single lane") {
    const Type type = Type::integer(64);
    CHECK(type.kind() == TypeKind::Int);
    CHECK(type.bits() == 64);
    CHECK(type.lanes() == 1);
    CHECK(type.isScalarInteger());
    CHECK(type.valid());
  }

  SECTION("a comparison result is a one-bit integer, not a separate kind") {
    CHECK(Type::boolean() == Type::integer(1));
    CHECK(Type::boolean().isBoolean());
    CHECK_FALSE(Type::integer(8).isBoolean());
  }

  SECTION("flags are opaque and have no width") {
    const Type flags = Type::flags();
    CHECK(flags.isFlags());
    CHECK(flags.bits() == 0);
    CHECK(flags.valid());
    // Laziness depends on this: nothing can treat a flag bundle as an integer by
    // accident.
    CHECK_FALSE(flags.isInteger());
  }

  SECTION("integer and float vectors are distinguishable") {
    const Type ints = Type::intVector(32, 4);
    const Type floats = Type::floatVector(32, 4);
    CHECK(ints != floats);
    CHECK(ints.bits() == 128);
    CHECK(floats.bits() == 128);
    CHECK(ints.isVector());
    CHECK(floats.isVector());
  }

  SECTION("widths that are not powers of two are legal") {
    // Bitfield extraction produces these routinely.
    CHECK(Type::integer(13).valid());
    CHECK(Type::integer(13).bits() == 13);
  }
}

TEST_CASE("type spellings round-trip", "[il][type]") {
  const Type cases[] = {Type::voidType(),       Type::integer(1),        Type::integer(8),
                        Type::integer(64),      Type::integer(13),       Type::floating(32),
                        Type::floating(64),     Type::flags(),           Type::intVector(8, 16),
                        Type::intVector(32, 4), Type::floatVector(64, 2)};

  for (const Type type : cases) {
    Type parsed;
    REQUIRE(Type::parse(type.toString(), parsed));
    CHECK(parsed == type);
    CHECK(parsed.toString() == type.toString());
  }

  SECTION("spellings are the expected ones") {
    CHECK(Type::integer(64).toString() == "i64");
    CHECK(Type::floating(32).toString() == "f32");
    CHECK(Type::flags().toString() == "flags");
    CHECK(Type::intVector(32, 4).toString() == "i32x4");
    CHECK(Type::floatVector(64, 2).toString() == "f64x2");
    CHECK(Type::voidType().toString() == "void");
  }

  SECTION("nonsense is rejected rather than guessed at") {
    Type parsed;
    CHECK_FALSE(Type::parse("", parsed));
    CHECK_FALSE(Type::parse("i", parsed));
    CHECK_FALSE(Type::parse("i0", parsed));
    CHECK_FALSE(Type::parse("q64", parsed));
    CHECK_FALSE(Type::parse("i32x", parsed));
    CHECK_FALSE(Type::parse("i32x1", parsed));  // one lane is a scalar
    CHECK_FALSE(Type::parse("i99999", parsed));
  }
}

TEST_CASE("maturity levels are ordered and round-trip", "[il][maturity]") {
  const Maturity levels[] = {Maturity::Lifted,   Maturity::Local,      Maturity::Cfg,
                             Maturity::Ssa,      Maturity::Resolved,   Maturity::Optimized,
                             Maturity::Vars,     Maturity::Structured, Maturity::Typed};

  for (const Maturity level : levels) {
    Maturity parsed = Maturity::Lifted;
    REQUIRE(parseMaturity(toString(level), parsed));
    CHECK(parsed == level);
    CHECK_FALSE(describe(level).empty());
  }

  SECTION("the ordering is what pass requirements are compared against") {
    CHECK(static_cast<unsigned>(Maturity::Lifted) < static_cast<unsigned>(Maturity::Cfg));
    CHECK(static_cast<unsigned>(Maturity::Cfg) < static_cast<unsigned>(Maturity::Ssa));
    CHECK(static_cast<unsigned>(Maturity::Ssa) < static_cast<unsigned>(Maturity::Typed));
  }

  SECTION("an unknown level name fails") {
    Maturity parsed = Maturity::Lifted;
    CHECK_FALSE(parseMaturity("polished", parsed));
  }
}
