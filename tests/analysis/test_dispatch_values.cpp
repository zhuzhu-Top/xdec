// matchDispatchValues: recovering a resolved table dispatch's real values
// (and, for the two-target case, the original boolean) from the index's own
// constant/select structure -- no memory involved.
#include <catch2/catch_test_macros.hpp>

#include "il/il_test_support.h"
#include "xdec/analysis/dispatch_values.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::DispatchValues;
using xdec::analysis::matchDispatchValues;
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
  ExprId entryReg(std::string_view name) {
    return function.entryReg(function.registers().find(name));
  }

  Function function;
  BlockId entry;
};

}  // namespace

TEST_CASE("a select over two constants recovers the values and the condition",
          "[analysis][dispatch-values]") {
  Fixture f;
  const ExprId cond = f.function.binary(ExprOp::CmpNe, f.entryReg("x0"), f.i64(0));
  const ExprId index = f.function.select(cond, f.i64(0x20f), f.i64(0x1ca));

  const auto result = matchDispatchValues(f.function, index, 2);
  REQUIRE(result.has_value());
  REQUIRE(result->values.size() == 2);
  CHECK(result->values[0] == 0x1ca);
  CHECK(result->values[1] == 0x20f);
  REQUIRE(result->condition.valid());
  CHECK(result->condition == cond);
  // 0x20f is the larger value, so it is values[1]/targets[1]: the true arm
  // (0x20f) is not the first (smaller) one.
  CHECK(!result->conditionTrueIsFirst);
}

TEST_CASE("a clamp select wrapping the real split still finds the inner condition",
          "[analysis][dispatch-values]") {
  Fixture f;
  const ExprId cond = f.function.binary(ExprOp::CmpEq, f.entryReg("x0"), f.i64(0));
  const ExprId inner = f.function.select(cond, f.i64(0x10), f.i64(0x20));
  // An out-of-range clamp whose replacement arm is unreachable for this
  // particular pair of values -- both 0x10 and 0x20 are within bound, so a
  // memory-free evaluator that (like image_eval) decides the compare instead
  // of blindly unioning both arms still narrows to exactly {0x10, 0x20}.
  const ExprId bound = f.function.binary(ExprOp::CmpLtS, f.i64(0x2cc), inner);
  const ExprId clamped = f.function.select(bound, f.i64(0x213), inner);

  const auto result = matchDispatchValues(f.function, clamped, 2);
  REQUIRE(result.has_value());
  REQUIRE(result->values.size() == 2);
  CHECK(result->values[0] == 0x10);
  CHECK(result->values[1] == 0x20);
  REQUIRE(result->condition.valid());
  CHECK(result->condition == cond);
  CHECK(result->conditionTrueIsFirst);
}

TEST_CASE("a nested select over three constants recovers ascending values with no condition",
          "[analysis][dispatch-values]") {
  Fixture f;
  const ExprId cond1 = f.function.binary(ExprOp::CmpNe, f.entryReg("x0"), f.i64(0));
  const ExprId cond2 = f.function.binary(ExprOp::CmpEq, f.entryReg("x1"), f.i64(0));
  const ExprId index =
      f.function.select(cond1, f.i64(0x30), f.function.select(cond2, f.i64(0x10), f.i64(0x20)));

  const auto result = matchDispatchValues(f.function, index, 3);
  REQUIRE(result.has_value());
  REQUIRE(result->values.size() == 3);
  CHECK(result->values[0] == 0x10);
  CHECK(result->values[1] == 0x20);
  CHECK(result->values[2] == 0x30);
  // No single Select splits exactly two of the three values apart; the
  // two-target-only condition search does not apply here.
  CHECK(!result->condition.valid());
}

TEST_CASE("a load anywhere in the index is not reconstructable from the IL alone",
          "[analysis][dispatch-values]") {
  Fixture f;
  const ExprId cond = f.function.binary(ExprOp::CmpNe, f.entryReg("x0"), f.i64(0));
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1000, Type::integer(64), f.i64(0x5000));
  const ExprId index = f.function.select(cond, f.i64(0x10), f.function.valueRef(loaded));

  CHECK(!matchDispatchValues(f.function, index, 2).has_value());
}

TEST_CASE("a value count that does not match the target count is not a match",
          "[analysis][dispatch-values]") {
  Fixture f;
  const ExprId cond = f.function.binary(ExprOp::CmpNe, f.entryReg("x0"), f.i64(0));
  const ExprId index = f.function.select(cond, f.i64(0x10), f.i64(0x20));

  CHECK(!matchDispatchValues(f.function, index, 3).has_value());
}
