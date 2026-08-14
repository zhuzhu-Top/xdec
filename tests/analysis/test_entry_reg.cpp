// EntryRegFacts: platform-fixed bindings for leaked entry registers, and the
// sidecar file that can override them per binary (see analysis/entry_reg.h).
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "xdec/analysis/entry_reg.h"

using xdec::analysis::discoverEntrySidecar;
using xdec::analysis::EntryRegBinding;
using xdec::analysis::EntryRegFacts;
using xdec::analysis::loadEntrySidecar;

namespace {

[[nodiscard]] std::filesystem::path tempDir(const char* name) {
  const auto dir =
      std::filesystem::temp_directory_path() / (std::string{"xdec-entry-reg-"} + name);
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

void write(const std::filesystem::path& path, std::string_view text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

}  // namespace

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

TEST_CASE("discoverEntrySidecar finds '<binary>.entry.json' next to the binary, not elsewhere",
          "[analysis][entry-reg]") {
  const auto dir = tempDir("discover");
  const auto binary = dir / "absd";
  write(binary, "not really a binary");
  CHECK_FALSE(discoverEntrySidecar(binary).has_value());

  write(dir / "absd.entry.json", "{}");
  const auto found = discoverEntrySidecar(binary);
  REQUIRE(found.has_value());
  CHECK(*found == dir / "absd.entry.json");
  std::filesystem::remove_all(dir);
}

TEST_CASE("loadEntrySidecar parses literals (hex or numeric) and companions",
          "[analysis][entry-reg]") {
  const auto dir = tempDir("parse");
  const auto sidecar = dir / "absd.entry.json";
  write(sidecar, R"({
    "literal": { "x28": "0x0", "x19": 12345 },
    "companions": [
      { "name": "dyld", "path": "C:/path/to/dyld", "base": "0x104f78000" }
    ]
  })");

  const auto loaded = loadEntrySidecar(sidecar);
  REQUIRE(loaded.hasValue());
  CHECK(loaded->literals.at("x28") == 0);
  CHECK(loaded->literals.at("x19") == 12345);
  REQUIRE(loaded->companions.size() == 1);
  CHECK(loaded->companions[0].name == "dyld");
  CHECK(loaded->companions[0].path == std::filesystem::path{"C:/path/to/dyld"});
  REQUIRE(loaded->companions[0].runtimeBase.has_value());
  CHECK(*loaded->companions[0].runtimeBase == 0x104f78000u);
  std::filesystem::remove_all(dir);
}

TEST_CASE("loadEntrySidecar reports a malformed literal instead of silently dropping it",
          "[analysis][entry-reg]") {
  const auto dir = tempDir("bad-literal");
  const auto sidecar = dir / "absd.entry.json";
  write(sidecar, R"({ "literal": { "x28": "not a number" } })");

  const auto loaded = loadEntrySidecar(sidecar);
  CHECK_FALSE(loaded.hasValue());
  std::filesystem::remove_all(dir);
}
