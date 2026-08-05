#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include "xdec/support/result.h"

using namespace xdec;

namespace {

Result<int> parsePositive(int value) {
  if (value <= 0) {
    return err(DiagCode::OutOfRange, "value {} is not positive", value);
  }
  return value;
}

/// Exercises XDEC_TRY: the error must propagate with its diagnostic intact even
/// though the return type differs from the callee's.
Result<std::string> describe(int value) {
  XDEC_TRY(const int positive, parsePositive(value));
  return std::string{"positive:"} + std::to_string(positive);
}

Result<void> requireEven(int value) {
  if (value % 2 != 0) {
    return err(Diag{DiagCode::VerifyFailure, "odd value"}.at(0x1000));
  }
  return ok();
}

Result<int> doubleIfEven(int value) {
  XDEC_TRY_VOID(requireEven(value));
  return value * 2;
}

Result<std::unique_ptr<int>> makeMoveOnly(bool succeed) {
  if (!succeed) {
    return err(DiagCode::Internal, "refused");
  }
  return std::make_unique<int>(99);
}

}  // namespace

TEST_CASE("Result carries a value", "[result]") {
  auto result = parsePositive(5);
  REQUIRE(result);
  CHECK(result.hasValue());
  CHECK(result.value() == 5);
  CHECK(*result == 5);
}

TEST_CASE("Result carries a diagnostic", "[result]") {
  auto result = parsePositive(-1);
  REQUIRE_FALSE(result);
  CHECK_FALSE(result.hasValue());
  CHECK(result.error().code() == DiagCode::OutOfRange);
  CHECK(result.error().message() == "value -1 is not positive");
  CHECK_FALSE(result.error().hasAddress());
}

TEST_CASE("XDEC_TRY propagates across return types", "[result]") {
  auto good = describe(3);
  REQUIRE(good);
  CHECK(good.value() == "positive:3");

  auto bad = describe(0);
  REQUIRE_FALSE(bad);
  // The diagnostic must survive the conversion from Result<int> to
  // Result<std::string>, otherwise error provenance is lost at every layer.
  CHECK(bad.error().code() == DiagCode::OutOfRange);
  CHECK(bad.error().message() == "value 0 is not positive");
}

TEST_CASE("Result<void> distinguishes success from failure", "[result]") {
  auto good = requireEven(4);
  CHECK(good);
  CHECK(good.hasValue());

  auto bad = requireEven(3);
  REQUIRE_FALSE(bad);
  CHECK(bad.error().code() == DiagCode::VerifyFailure);
  CHECK(bad.error().hasAddress());
  CHECK(bad.error().address() == 0x1000);
}

TEST_CASE("XDEC_TRY_VOID propagates from Result<void>", "[result]") {
  auto good = doubleIfEven(6);
  REQUIRE(good);
  CHECK(good.value() == 12);

  auto bad = doubleIfEven(7);
  REQUIRE_FALSE(bad);
  CHECK(bad.error().message() == "odd value");
  CHECK(bad.error().address() == 0x1000);
}

TEST_CASE("Result holds move-only values", "[result]") {
  auto good = makeMoveOnly(true);
  REQUIRE(good);
  CHECK(*good.value() == 99);

  auto owned = std::move(good).value();
  REQUIRE(owned != nullptr);
  CHECK(*owned == 99);

  auto bad = makeMoveOnly(false);
  REQUIRE_FALSE(bad);
  CHECK(bad.error().message() == "refused");
}

TEST_CASE("valueOr substitutes a fallback", "[result]") {
  CHECK(parsePositive(8).valueOr(-1) == 8);
  CHECK(parsePositive(-8).valueOr(-1) == -1);
}

TEST_CASE("Diag accumulates notes and formats", "[result]") {
  Diag diag{DiagCode::DecodeFailure, "unknown encoding"};
  diag.at(0x123b98).note("while lifting block 4").note("while exploring function");

  CHECK(diag.notes().size() == 2);
  const std::string text = diag.format();
  CHECK(text.find("decode-failure") != std::string::npos);
  CHECK(text.find("unknown encoding") != std::string::npos);
  CHECK(text.find("0x123b98") != std::string::npos);
  CHECK(text.find("while lifting block 4") != std::string::npos);
}

TEST_CASE("DiagCode names are stable", "[result]") {
  CHECK(toString(DiagCode::Ok) == "ok");
  CHECK(toString(DiagCode::UnmappedAddress) == "unmapped-address");
  CHECK(toString(DiagCode::NotImplemented) == "not-implemented");
  CHECK(toString(DiagCode::VerifyFailure) == "verify-failure");
}
