// The tests read the real spec from `specs/`, not a copy.
//
// A fixture that drifts from the thing it stands in for is worse than no
// fixture: the checker would keep passing while the spec the lifter actually
// loads had rotted. Reading the file makes every check here a check on shipped
// content.
#pragma once

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "xdec/spec/parse.h"

#ifndef XDEC_SPEC_DIR
#error "XDEC_SPEC_DIR must name the directory holding the .xspec files"
#endif

namespace xdec::spec::testing {

[[nodiscard]] inline std::filesystem::path specDir() {
  return std::filesystem::path{XDEC_SPEC_DIR};
}

[[nodiscard]] inline std::string readSpec(std::string_view name) {
  const std::filesystem::path path = specDir() / name;
  std::ifstream file{path, std::ios::binary};
  if (!file) {
    FAIL("cannot open spec " << path.string());
  }
  std::ostringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

/// The root of the AArch64 spec. What the tests want is almost always the whole
/// thing rather than this one file, since the root holds the architecture and
/// nothing else; use `arm64Module` unless the test is about a single file.
[[nodiscard]] inline std::filesystem::path arm64SpecPath() {
  return specDir() / "arm64.xspec";
}

/// The AArch64 spec with every file it includes, parsed once per test binary.
///
/// Returned by reference and cached because parsing the whole spec is not free
/// and nothing here mutates it. A test that needs to modify a module should
/// parse its own text.
[[nodiscard]] inline const Module& arm64Module() {
  static const std::unique_ptr<Module> kModule = [] {
    auto parsed = parseSpecFile(arm64SpecPath());
    if (!parsed) {
      FAIL(parsed.error().format());
    }
    return std::move(parsed).value();
  }();
  return *kModule;
}

}  // namespace xdec::spec::testing
