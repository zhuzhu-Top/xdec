#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "spec/spec_test_support.h"
#include "xdec/il/printer.h"
#include "xdec/spec/compile.h"
#include "xdec/spec/engine.h"

namespace {

[[nodiscard]] const xdec::spec::SpecEngine& scalarFpEngine() {
  static const std::unique_ptr<xdec::spec::SpecEngine> kEngine = [] {
    auto loaded = xdec::spec::loadSpecFile(
        xdec::spec::testing::specDir().parent_path() / "tests/spec/arm64-scalar-fp.xspec");
    if (!loaded) {
      FAIL(loaded.error().format());
    }
    return std::move(loaded).value();
  }();
  return *kEngine;
}

[[nodiscard]] xdec::spec::DecodedInsn decode(uint32_t word) {
  const std::array bytes = {static_cast<std::byte>(word & 0xff),
                            static_cast<std::byte>((word >> 8) & 0xff),
                            static_cast<std::byte>((word >> 16) & 0xff),
                            static_cast<std::byte>((word >> 24) & 0xff)};
  return scalarFpEngine().decode(bytes, 0x1000);
}

[[nodiscard]] std::string ruleOf(uint32_t word) {
  const xdec::spec::DecodedInsn instruction = decode(word);
  REQUIRE(instruction.valid);
  return scalarFpEngine().program().instructions[instruction.instruction].name;
}

}  // namespace

TEST_CASE("scalar FP rules decode and elaborate typed IL", "[spec][scalar-fp]") {
  CHECK(ruleOf(0x1e222820) == "fadd_scalar");       // fadd s0, s1, s2
  CHECK(ruleOf(0x1e622820) == "fadd_scalar");       // fadd d0, d1, d2
  CHECK(ruleOf(0x1e202008) == "fcmp_zero_scalar");  // fcmp s0, #0.0
  CHECK(ruleOf(0x1e220020) == "scvtf_scalar");      // scvtf s0, w1
  CHECK(ruleOf(0x9e790020) == "fcvtzu_scalar");     // fcvtzu x0, d1
  CHECK(ruleOf(0x1e260020) == "fmov_gpr_from_fp");  // fmov w0, s1

  const auto instruction = decode(0x1e222820);
  CHECK(scalarFpEngine().disassemble(instruction) == "fadd s0, s1, s2");

  xdec::il::Function function{scalarFpEngine().program().arch,
                              scalarFpEngine().program().registers, 0x1000};
  const xdec::il::BlockId block = function.createBlock(0x1000);
  function.setEntryBlock(block);
  xdec::spec::LiftSite site{.function = &function,
                            .block = block,
                            .address = 0x1000,
                            .blockAt = [&](uint64_t) { return block; }};
  REQUIRE(scalarFpEngine().elaborate(instruction, site));

  const std::string il = xdec::il::print(function);
  CHECK(il.find("fadd:f32") != std::string::npos);
  CHECK(il.find("read s1") != std::string::npos);
  CHECK(il.find("write s0") != std::string::npos);
}
