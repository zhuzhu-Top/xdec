// EntryRegFacts: platform-fixed bindings for leaked entry registers (see
// analysis/entry_reg.h).
#include <catch2/catch_test_macros.hpp>

#include "xdec/analysis/entry_reg.h"

using xdec::analysis::EntryRegBinding;
using xdec::analysis::EntryRegFacts;

TEST_CASE("an unbound register resolves to nothing, same as before this existed",
          "[analysis][entry-reg]") {
  EntryRegFacts facts;
  CHECK(facts.empty());
  CHECK_FALSE(facts.resolve("x22").has_value());
  CHECK(facts.bindingFor("x22") == nullptr);
}

TEST_CASE("a literal binding resolves outright", "[analysis][entry-reg]") {
  EntryRegFacts facts;
  facts.setBinding("x28", EntryRegBinding::fromLiteral(0));
  CHECK_FALSE(facts.empty());
  const auto resolved = facts.resolve("x28");
  REQUIRE(resolved.has_value());
  CHECK(*resolved == 0);
}

TEST_CASE("a base-plus-offset binding resolves only once its companion's base is known",
          "[analysis][entry-reg]") {
  EntryRegFacts facts;
  facts.setBinding("x22", EntryRegBinding::fromBase("dyld", 0x68310));
  CHECK_FALSE(facts.resolve("x22").has_value());  // companion never opened

  facts.setCompanionBase("dyld", 0x104fe0000);
  const auto resolved = facts.resolve("x22");
  REQUIRE(resolved.has_value());
  CHECK(*resolved == 0x104fe0000 + 0x68310);
}
