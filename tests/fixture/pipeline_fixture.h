// The shared plumbing an integration test needs to drive decompileToC() (see
// xdec/decompile/emit.h) over a hand-assembled program: the arm64 spec engine,
// a builtin-pass registry, and a flat byte-addressable memory. Every test file
// that wanted "the real pipeline, not a hand-built il::Function" used to
// re-type these three things itself (see tests/decompile/test_driver.cpp);
// this is that boilerplate extracted once, so an integration test is free to
// read like a test of decompileToC's contract instead of a test of assembling
// a fixture.
//
// What this deliberately does not do: subprocess the CLI. A test here calls
// decompileToC() the same way cmd_pipeline.cpp's `decompile` command does,
// in-process, so a failure points straight at the library call that broke
// rather than at a captured stdout diff.
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "xdec/decompile/emit.h"
#include "xdec/il/function.h"
#include "xdec/passes/builtin.h"
#include "xdec/spec/compile.h"
#include "xdec/spec/engine.h"

#include "../spec/spec_test_support.h"

namespace xdec::testing {

/// The AArch64 spec, compiled once per test binary. decompile()/decompileToC()
/// take a SpecEngine (not the parsed Module spec::testing::arm64Module()
/// already caches), so this is its own cache rather than a rebuild of that one
/// -- compiling is not free either, and nothing here mutates the result.
[[nodiscard]] inline const spec::SpecEngine& arm64Engine() {
  static const std::unique_ptr<spec::SpecEngine> kEngine = [] {
    auto loaded = spec::loadSpecFile(spec::testing::arm64SpecPath());
    if (!loaded) {
      FAIL(loaded.error().format());
    }
    return std::move(loaded).value();
  }();
  return *kEngine;
}

/// A registry with every builtin pass registered: what every decompile() /
/// decompileToC() call needs, fresh per test so one test's plugin or pass
/// ordering experiment (were one ever added here) cannot leak into another's.
[[nodiscard]] inline pass::Registry builtinRegistry() {
  pass::Registry registry;
  passes::registerBuiltinPasses(registry);
  return registry;
}

/// Instruction words plus table qwords; reads of anything else are unmapped.
/// The same minimal shape tests/decompile/test_driver.cpp assembles by hand,
/// kept here so a new integration test does not have to redefine it.
class FlatProgram {
 public:
  void putInsn(uint64_t va, uint32_t word) { insns_[va] = word; }
  void putQword(uint64_t va, uint64_t value) { qwords_[va] = value; }

  [[nodiscard]] ByteReader reader() const {
    return [this](uint64_t va, std::span<std::byte> out) -> Result<void> {
      if (out.size() == 4) {
        if (const auto found = insns_.find(va); found != insns_.end()) {
          for (unsigned i = 0; i < 4; ++i) {
            out[i] = static_cast<std::byte>((found->second >> (i * 8)) & 0xff);
          }
          return ok();
        }
      }
      if (out.size() <= 8) {
        if (const auto found = qwords_.find(va); found != qwords_.end()) {
          for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = static_cast<std::byte>((found->second >> (i * 8)) & 0xff);
          }
          return ok();
        }
      }
      return err(DiagCode::BadFormat, "unmapped");
    };
  }

 private:
  std::map<uint64_t, uint32_t> insns_;
  std::map<uint64_t, uint64_t> qwords_;
};

/// Runs the whole lift-to-C pipeline over `reader`/`entry`, the same call
/// cmd_pipeline.cpp's `decompile` command makes. The driver target is forced
/// to Vars regardless of what `options.driver.target` came in set to --
/// emission reads the recovered call arity off Vars IL (see
/// decompile::DecompileToCOptions), so anything short of it would make this
/// helper's very name a lie. A test that wants a different target belongs
/// against decompile() directly, not this convenience wrapper.
[[nodiscard]] inline Result<decompile::DecompileToCResult> decompileToCFromBinary(
    const ByteReader& reader, uint64_t entry, decompile::DecompileToCOptions options = {}) {
  options.driver.target = il::Maturity::Vars;
  pass::Registry registry = builtinRegistry();
  return decompile::decompileToC(arm64Engine(), reader, entry, registry, options);
}

}  // namespace xdec::testing
