// matchJumpTable: the two table families, and honest misses.
#include <catch2/catch_test_macros.hpp>

#include "il/il_test_support.h"
#include "xdec/analysis/jump_table.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::JumpTable;
using xdec::analysis::matchJumpTable;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::Type;

namespace {

struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {
    entry = function.createBlock(0x1000);
    function.setEntryBlock(entry);
  }

  ExprId i64(uint64_t value) { return function.constant(Type::integer(64), value); }
  /// The index is opaque to the matcher; an undef stands in for "whatever
  /// the dispatcher computed".
  ExprId anyIndex() { return function.undefined(Type::integer(64)); }
  ExprId load64(ExprId address) {
    return function.valueRef(
        function.appendLoad(entry, 0x1000, Type::integer(64), address));
  }
  ExprId load32(ExprId address) {
    return function.valueRef(
        function.appendLoad(entry, 0x1000, Type::integer(32), address));
  }

  Function function;
  BlockId entry;
};

TEST_CASE("a pointer table: load(base + index*8)", "[analysis][jump-table]") {
  Fixture f;
  const ExprId address = f.function.binary(
      ExprOp::Add, f.i64(0x30b7f0),
      f.function.binary(ExprOp::Shl, f.anyIndex(), f.i64(3)));
  const auto table = matchJumpTable(f.function, f.load64(address));
  REQUIRE(table.has_value());
  CHECK(table->base == 0x30b7f0);
  CHECK(table->stride == 8);
  CHECK(table->entryBits == 64);
  CHECK(!table->signedOffsets);
}

TEST_CASE("a bare global pointer: load(base) with contiguous stride",
          "[analysis][jump-table]") {
  Fixture f;
  const auto table = matchJumpTable(f.function, f.load64(f.i64(0x12345)));
  REQUIRE(table.has_value());
  CHECK(table->base == 0x12345);
  CHECK(table->stride == 8);
}

TEST_CASE("an offset table: anchor + sext(load32(base + index*4))",
          "[analysis][jump-table]") {
  Fixture f;
  const ExprId address = f.function.binary(
      ExprOp::Add, f.function.binary(ExprOp::Mul, f.anyIndex(), f.i64(4)),
      f.i64(0x50534));
  const ExprId offset =
      f.function.cast(ExprOp::SExt, Type::integer(64), f.load32(address));
  const ExprId target = f.function.binary(ExprOp::Add, offset, f.i64(0xa65b8));
  const auto table = matchJumpTable(f.function, target);
  REQUIRE(table.has_value());
  CHECK(table->base == 0x50534);
  CHECK(table->stride == 4);
  CHECK(table->entryBits == 32);
  CHECK(table->signedOffsets);
  CHECK(table->anchor == 0xa65b8);
}

TEST_CASE("a packed small-entry table: anchor + (zext(load16) << 2), casts unwrapped",
          "[analysis][jump-table]") {
  Fixture f;
  // load16[(index << 1) + 0x505c8], doubled zext left by the lifter.
  const ExprId address = f.function.binary(
      ExprOp::Add, f.function.binary(ExprOp::Shl, f.anyIndex(), f.i64(1)),
      f.i64(0x505c8));
  const ExprId loaded16 = f.function.valueRef(
      f.function.appendLoad(f.entry, 0x1000, Type::integer(16), address));
  const ExprId wide = f.function.cast(
      ExprOp::ZExt, Type::integer(64),
      f.function.cast(ExprOp::ZExt, Type::integer(32), loaded16));
  const ExprId target = f.function.binary(
      ExprOp::Add, f.function.binary(ExprOp::Shl, wide, f.i64(2)), f.i64(0xa7458));
  const auto table = matchJumpTable(f.function, target);
  REQUIRE(table.has_value());
  CHECK(table->base == 0x505c8);
  CHECK(table->stride == 2);
  CHECK(table->entryBits == 16);
  CHECK(table->relative);
  CHECK(!table->signedOffsets);
  CHECK(table->anchor == 0xa7458);
  CHECK(table->offsetShift == 2);
}

TEST_CASE("a data-dependent address is not a table", "[analysis][jump-table]") {
  Fixture f;
  const ExprId address =
      f.function.binary(ExprOp::Add, f.anyIndex(), f.anyIndex());
  CHECK(!matchJumpTable(f.function, f.load64(address)).has_value());
  CHECK(!matchJumpTable(f.function, f.anyIndex()).has_value());
}

}  // namespace
