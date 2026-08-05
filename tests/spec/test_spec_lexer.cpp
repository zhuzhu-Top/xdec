// The lexer's job is to be boring and exact. These check the two places it is
// not obvious: numeric literals and the escapes an asm template depends on.
#include <catch2/catch_test_macros.hpp>

#include "xdec/spec/parse.h"

using xdec::spec::Expr;
using xdec::spec::ExprKind;
using xdec::spec::parseExpression;

namespace {

[[nodiscard]] uint64_t evalLiteral(std::string_view text) {
  auto parsed = parseExpression(text, "<test>");
  REQUIRE(parsed);
  REQUIRE(parsed.value()->kind == ExprKind::Integer);
  return parsed.value()->integer;
}

}  // namespace

TEST_CASE("numeric literals are read in every base", "[spec][lexer]") {
  CHECK(evalLiteral("0") == 0);
  CHECK(evalLiteral("42") == 42);
  CHECK(evalLiteral("0x1f") == 0x1f);
  CHECK(evalLiteral("0X1F") == 0x1f);
  CHECK(evalLiteral("0b1011") == 0b1011);

  SECTION("underscores group digits without changing the value") {
    CHECK(evalLiteral("0b1101_0110") == 0xd6);
    CHECK(evalLiteral("0xdead_beef") == 0xdeadbeef);
    CHECK(evalLiteral("1_000_000") == 1000000);
  }

  SECTION("a literal running into an identifier is a typo, not juxtaposition") {
    CHECK_FALSE(parseExpression("12abc", "<test>"));
    CHECK_FALSE(parseExpression("0x", "<test>"));
    CHECK_FALSE(parseExpression("0b", "<test>"));
  }
}

TEST_CASE("operators are matched longest first", "[spec][lexer]") {
  // `>>>` must not read as `>>` followed by `>`, and `<=u` must not read as
  // `<=` followed by an identifier.
  auto shift = parseExpression("a >>> b", "<test>");
  REQUIRE(shift);
  CHECK(shift.value()->kind == ExprKind::Binary);
  CHECK(shift.value()->binaryOp == xdec::spec::BinaryOp::ShrS);

  auto compare = parseExpression("a <=u b", "<test>");
  REQUIRE(compare);
  CHECK(compare.value()->binaryOp == xdec::spec::BinaryOp::LessEqualU);
}

TEST_CASE("comments are trivia", "[spec][lexer]") {
  auto lineComment = parseExpression("1 + // ignored\n 2", "<test>");
  REQUIRE(lineComment);
  CHECK(lineComment.value()->kind == ExprKind::Binary);

  auto blockComment = parseExpression("1 /* ignored */ + 2", "<test>");
  REQUIRE(blockComment);
  CHECK(blockComment.value()->kind == ExprKind::Binary);
}

TEST_CASE("precedence follows the documented table", "[spec][lexer]") {
  // `a + b * c` must group as `a + (b * c)`.
  auto expr = parseExpression("a + b * c", "<test>");
  REQUIRE(expr);
  const Expr& root = *expr.value();
  REQUIRE(root.kind == ExprKind::Binary);
  CHECK(root.binaryOp == xdec::spec::BinaryOp::Add);
  CHECK(root.args[1]->binaryOp == xdec::spec::BinaryOp::Mul);

  // Shifts bind tighter than comparisons, which is what makes
  // `sf << 5 == 32` mean `(sf << 5) == 32`.
  auto mixed = parseExpression("sf << 5 == 32", "<test>");
  REQUIRE(mixed);
  CHECK(mixed.value()->binaryOp == xdec::spec::BinaryOp::Equal);
  CHECK(mixed.value()->args[0]->binaryOp == xdec::spec::BinaryOp::Shl);
}

TEST_CASE("an unterminated construct fails rather than guessing", "[spec][lexer]") {
  CHECK_FALSE(parseExpression("\"unterminated", "<test>"));
  CHECK_FALSE(parseExpression("(1 + 2", "<test>"));
  CHECK_FALSE(parseExpression("1 +", "<test>"));
  CHECK_FALSE(parseExpression("@", "<test>"));
}
