#include <catch2/catch_test_macros.hpp>

#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include "xdec/support/handle.h"

using namespace xdec;

namespace {

struct ExprTag;
struct BlockTag;
using ExprId = Handle<ExprTag>;
using BlockId = Handle<BlockTag>;

struct Expr {
  int opcode = 0;
  std::string note;
};

}  // namespace

TEST_CASE("handles default to invalid", "[handle]") {
  ExprId handle;
  CHECK_FALSE(handle.valid());
  CHECK_FALSE(static_cast<bool>(handle));
  CHECK(handle == ExprId::invalid());

  const ExprId zero{0};
  // Index zero is a perfectly good handle; only the sentinel is invalid.
  CHECK(zero.valid());
  CHECK(zero.index() == 0);
  CHECK(zero != handle);
}

TEST_CASE("handle families are distinct types", "[handle]") {
  // The whole point of the Tag parameter: an ExprId must not convert to a
  // BlockId, so a mixed-up index cannot compile.
  STATIC_REQUIRE_FALSE(std::is_convertible_v<ExprId, BlockId>);
  STATIC_REQUIRE_FALSE(std::is_constructible_v<BlockId, ExprId>);
  // And a raw integer must not silently become a handle.
  STATIC_REQUIRE_FALSE(std::is_convertible_v<uint32_t, ExprId>);
  STATIC_REQUIRE(std::is_constructible_v<ExprId, uint32_t>);
}

TEST_CASE("handles are ordered and hashable", "[handle]") {
  CHECK(ExprId{1} < ExprId{2});
  CHECK_FALSE(ExprId{2} < ExprId{1});

  std::unordered_set<ExprId> seen;
  seen.insert(ExprId{3});
  seen.insert(ExprId{3});
  seen.insert(ExprId{4});
  CHECK(seen.size() == 2);
  CHECK(seen.contains(ExprId{3}));
  CHECK_FALSE(seen.contains(ExprId{5}));
}

TEST_CASE("HandleVector stores and addresses elements", "[handle]") {
  HandleVector<ExprId, Expr> exprs;
  CHECK(exprs.empty());
  CHECK(exprs.nextHandle() == ExprId{0});

  const ExprId first = exprs.emplace(Expr{1, "add"});
  const ExprId second = exprs.emplace(Expr{2, "sub"});
  CHECK(first == ExprId{0});
  CHECK(second == ExprId{1});
  CHECK(exprs.size() == 2);
  CHECK(exprs[first].note == "add");
  CHECK(exprs[second].note == "sub");

  exprs[first].note = "mul";
  CHECK(exprs[first].note == "mul");
}

TEST_CASE("HandleVector handles survive growth", "[handle]") {
  HandleVector<ExprId, Expr> exprs;
  std::vector<ExprId> handles;
  for (int index = 0; index < 1000; ++index) {
    handles.push_back(exprs.emplace(Expr{index, std::to_string(index)}));
  }
  // The reason for indices over pointers: reallocation cannot invalidate these.
  for (int index = 0; index < 1000; ++index) {
    CHECK(exprs[handles[static_cast<std::size_t>(index)]].opcode == index);
  }
}

TEST_CASE("HandleVector reports containment", "[handle]") {
  HandleVector<ExprId, Expr> exprs;
  const ExprId only = exprs.emplace(Expr{7, "only"});
  CHECK(exprs.contains(only));
  CHECK_FALSE(exprs.contains(ExprId{1}));
  CHECK_FALSE(exprs.contains(ExprId::invalid()));

  CHECK(exprs.tryGet(only) != nullptr);
  CHECK(exprs.tryGet(ExprId{99}) == nullptr);
  CHECK(exprs.tryGet(ExprId::invalid()) == nullptr);
}

TEST_CASE("HandleVector iterates handles and values", "[handle]") {
  HandleVector<ExprId, Expr> exprs;
  for (int index = 0; index < 5; ++index) {
    exprs.emplace(Expr{index, ""});
  }

  int expected = 0;
  for (const Expr& expr : exprs) {
    CHECK(expr.opcode == expected++);
  }
  CHECK(expected == 5);

  uint32_t expectedIndex = 0;
  for (const ExprId handle : exprs.handles()) {
    CHECK(handle.index() == expectedIndex);
    CHECK(exprs[handle].opcode == static_cast<int>(expectedIndex));
    ++expectedIndex;
  }
  CHECK(expectedIndex == 5);
}
