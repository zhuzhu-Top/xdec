#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "spec/spec_test_support.h"
#include "xdec/il/interp.h"
#include "xdec/il/printer.h"
#include "xdec/spec/compile.h"
#include "xdec/spec/engine.h"

namespace {

[[nodiscard]] const xdec::spec::SpecEngine& engine() {
  static const std::unique_ptr<xdec::spec::SpecEngine> kEngine = [] {
    auto loaded = xdec::spec::loadSpecFile(xdec::spec::testing::arm64SpecPath());
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
  return engine().decode(bytes, 0x1000);
}

[[nodiscard]] std::string ruleOf(uint32_t word) {
  const xdec::spec::DecodedInsn instruction = decode(word);
  REQUIRE(instruction.valid);
  return engine().program().instructions[instruction.instruction].name;
}

}  // namespace

TEST_CASE("baseline vector FP rules preserve lane operations", "[spec][vector-fp]") {
  CHECK(ruleOf(0x0e22d420) == "neon_fadd_2s_4s");  // fadd v0.2s, v1.2s, v2.2s
  CHECK(ruleOf(0x4e22d420) == "neon_fadd_2s_4s");  // fadd v0.4s, v1.4s, v2.4s
  CHECK(ruleOf(0x4e62d420) == "neon_fadd_2d");     // fadd v0.2d, v1.2d, v2.2d
  CHECK(ruleOf(0x0ea2d420) == "neon_fsub_2s_4s");  // fsub v0.2s, v1.2s, v2.2s
  CHECK(ruleOf(0x4ee2d420) == "neon_fsub_2d");     // fsub v0.2d, v1.2d, v2.2d
  CHECK(ruleOf(0x6e22dc20) == "neon_fmul_2s_4s");  // fmul v0.4s, v1.4s, v2.4s
  CHECK(ruleOf(0x6e62dc20) == "neon_fmul_2d");     // fmul v0.2d, v1.2d, v2.2d
  CHECK(ruleOf(0x2e22fc20) == "neon_fdiv_2s_4s");  // fdiv v0.2s, v1.2s, v2.2s
  CHECK(ruleOf(0x6e62fc20) == "neon_fdiv_2d");     // fdiv v0.2d, v1.2d, v2.2d
  CHECK(ruleOf(0x6ea1f820) == "neon_fsqrt_2s_4s"); // fsqrt v0.4s, v1.4s
  CHECK(ruleOf(0x6ee1f820) == "neon_fsqrt_2d");    // fsqrt v0.2d, v1.2d
  CHECK(ruleOf(0x0e22e420) == "neon_fcmeq_2s_4s");// fcmeq v0.2s, v1.2s, v2.2s
  CHECK(ruleOf(0x4e62e420) == "neon_fcmeq_2d");   // fcmeq v0.2d, v1.2d, v2.2d
  CHECK(ruleOf(0x4ea0f820) == "neon_fabs_2s_4s");  // fabs v0.4s, v1.4s
  CHECK(ruleOf(0x6ee0f820) == "neon_fneg_2d");     // fneg v0.2d, v1.2d

  const xdec::spec::DecodedInsn instruction = decode(0x4e22d420);
  xdec::il::Function function{engine().program().arch, engine().program().registers, 0x1000};
  const xdec::il::BlockId block = function.createBlock(0x1000);
  function.setEntryBlock(block);
  xdec::spec::LiftSite site{.function = &function,
                            .block = block,
                            .address = 0x1000,
                            .blockAt = [&](uint64_t) { return block; }};
  REQUIRE(engine().elaborate(instruction, site));

  const std::string il = xdec::il::print(function);
  CHECK(il.find("read q1") != std::string::npos);
  CHECK(il.find("read q2") != std::string::npos);
  CHECK(il.find("write q0") != std::string::npos);
  // One scalar float add per 32-bit lane: the lane mapping must not collapse
  // a vector operation into a whole-register integer addition.
  std::size_t additions = 0;
  for (std::size_t at = il.find("fadd:f32"); at != std::string::npos;
       at = il.find("fadd:f32", at + 1)) {
    ++additions;
  }
  CHECK(additions == 4);

  function.appendReturn(block, 0x1004);
  xdec::il::Interpreter interpreter{function};
  const auto reg = [&](std::string_view name) {
    const xdec::il::RegId id = function.registers().find(name);
    REQUIRE(id.valid());
    return id;
  };
  // [1, 2, 3, 4] + [10, 20, 30, 40], packed low lane first.
  interpreter.writeRegister(reg("q1"), {0x400000003f800000, 0x4080000040400000});
  interpreter.writeRegister(reg("q2"), {0x41a0000041200000, 0x4220000041f00000});
  REQUIRE(interpreter.runBlock(block).stop == xdec::il::ExecStop::Return);
  const xdec::il::ConcreteValue result = interpreter.readRegister(reg("q0"));
  CHECK(result.lo == 0x41b0000041300000);
  CHECK(result.hi == 0x4230000042040000);
}
