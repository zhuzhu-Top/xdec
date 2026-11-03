#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
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

[[nodiscard]] xdec::il::Function lift(uint32_t word) {
  const xdec::spec::DecodedInsn instruction = decode(word);
  REQUIRE(instruction.valid);

  xdec::il::Function function{engine().program().arch,
                              engine().program().registers, 0x1000};
  const xdec::il::BlockId block = function.createBlock(0x1000);
  function.setEntryBlock(block);
  xdec::spec::LiftSite site{.function = &function,
                            .block = block,
                            .address = 0x1000,
                            .blockAt = [&](uint64_t) { return block; }};
  REQUIRE(engine().elaborate(instruction, site));
  function.appendReturn(block, 0x1004);
  return function;
}

[[nodiscard]] xdec::il::RegId reg(const xdec::il::Function& function,
                                  std::string_view name) {
  const xdec::il::RegId id = function.registers().find(name);
  REQUIRE(id.valid());
  return id;
}

[[nodiscard]] xdec::il::ConcreteValue stridedBytes(unsigned first) {
  xdec::il::ConcreteValue value{};
  for (unsigned lane = 0; lane < 16; ++lane) {
    const uint64_t byte = first + lane * 4;
    if (lane < 8) {
      value.lo |= byte << (lane * 8);
    } else {
      value.hi |= byte << ((lane - 8) * 8);
    }
  }
  return value;
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

TEST_CASE("LD4 decodes arbitrary wrapped register lists and de-interleaves bytes",
          "[spec][vector-fp]") {
  CHECK(ruleOf(0x4c400230) == "ld4_16b");  // ld4 {v16-v19}.16b, [x17]
  CHECK(ruleOf(0x4c4000be) == "ld4_16b");  // ld4 {v30,v31,v0,v1}.16b, [x5]

  xdec::il::Function function = lift(0x4c4000be);
  xdec::il::ExecMemory memory;
  std::array<std::byte, 64> bytes{};
  for (unsigned i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::byte>(i);
  }
  memory.seed(0x2000, bytes);

  xdec::il::Interpreter interpreter{function, &memory};
  interpreter.writeRegister(reg(function, "x5"), {0x2000, 0});
  REQUIRE(interpreter.runBlock(function.entryBlock()).stop ==
          xdec::il::ExecStop::Return);
  CHECK(interpreter.readRegister(reg(function, "q30")) == stridedBytes(0));
  CHECK(interpreter.readRegister(reg(function, "q31")) == stridedBytes(1));
  CHECK(interpreter.readRegister(reg(function, "q0")) == stridedBytes(2));
  CHECK(interpreter.readRegister(reg(function, "q1")) == stridedBytes(3));
}

TEST_CASE("SHLL and SHLL2 decode all widths and select the correct source half",
          "[spec][vector-fp]") {
  CHECK(ruleOf(0x2e213b1a) == "neon_shll_lanes");   // shll.8h
  CHECK(ruleOf(0x2e613b1a) == "neon_shll_lanes");   // shll.4s
  CHECK(ruleOf(0x2ea13b1a) == "neon_shll_lanes");   // shll.2d
  CHECK(ruleOf(0x6e213b18) == "neon_shll2_lanes");  // shll2.8h
  CHECK(ruleOf(0x6e613b18) == "neon_shll2_lanes");  // shll2.4s
  CHECK(ruleOf(0x6ea13b18) == "neon_shll2_lanes");  // shll2.2d

  SECTION("SHLL widens the low half") {
    xdec::il::Function function = lift(0x2e613b1a);
    xdec::il::Interpreter interpreter{function};
    interpreter.writeRegister(
        reg(function, "q24"),
        {0xffff800000020001, 0x4444333322221111});
    REQUIRE(interpreter.runBlock(function.entryBlock()).stop ==
            xdec::il::ExecStop::Return);
    CHECK(interpreter.readRegister(reg(function, "q26")) ==
          xdec::il::ConcreteValue{0x0002000000010000,
                                  0xffff000080000000});
  }

  SECTION("SHLL2 widens the high half even when source and destination alias") {
    xdec::il::Function function = lift(0x6e613b18);
    xdec::il::Interpreter interpreter{function};
    interpreter.writeRegister(
        reg(function, "q24"),
        {0x9999888877776666, 0xffff800000020001});
    REQUIRE(interpreter.runBlock(function.entryBlock()).stop ==
            xdec::il::ExecStop::Return);
    CHECK(interpreter.readRegister(reg(function, "q24")) ==
          xdec::il::ConcreteValue{0x0002000000010000,
                                  0xffff000080000000});
  }
}

TEST_CASE("fairplayd vector families no longer fall through SIMD catch-alls",
          "[spec][vector-fp][coverage]") {
  CHECK(ruleOf(0x4e181d7e) == "neon_ins_gpr_d");    // mov v30.d[1], x11
  CHECK(ruleOf(0x0e200a04) == "neon_rev64");        // rev64 v4.8b, v16.8b
  CHECK(ruleOf(0x4e219402) == "neon_mla_lanes");    // mla v2.16b, v0.16b, v1.16b
  CHECK(ruleOf(0x6ea494a2) == "neon_mls_lanes");    // mls v2.4s, v5.4s, v4.4s
  CHECK(ruleOf(0x2ea3c045) == "neon_umull_lanes");  // umull v5.2d, v2.2s, v3.2s
  CHECK(ruleOf(0x6ea3c05c) == "neon_umull2_lanes"); // umull2 v28.2d, v2.4s, v3.4s
  CHECK(ruleOf(0x4e9d599d) == "neon_uzp2");         // uzp2 v29.4s, v12.4s, v29.4s
  CHECK(ruleOf(0x6f3f16d5) == "neon_usra_lanes");  // usra v21.4s, v22.4s, #1
  CHECK(ruleOf(0x0f08a463) == "neon_sshll_lanes"); // sshll v3.8h, v3.8b, #0
  CHECK(ruleOf(0x4f10a464) == "neon_sshll2_lanes");// sshll2 v4.4s, v3.8h, #0
  CHECK(ruleOf(0x2e623062) == "neon_usubw_lanes"); // usubw v2.4s, v3.4s, v2.4h
  CHECK(ruleOf(0x6e623084) == "neon_usubw2_lanes");// usubw2 v4.4s, v4.4s, v2.8h
  CHECK(ruleOf(0x2e223463) == "neon_cmhi_lanes");  // cmhi v3.8b, v3.8b, v2.8b
  CHECK(ruleOf(0x6ee53c05) == "neon_cmhs_lanes");  // cmhs v5.2d, v0.2d, v5.2d
  CHECK(ruleOf(0x7ee43f7b) == "neon_cmhs_scalar_d");// cmhs d27, d27, d4
  CHECK(ruleOf(0x4eb1b800) == "neon_addv");        // addv s0, v0.4s
  CHECK(ruleOf(0x6e20bb7b) == "neon_neg_lanes");   // neg v27.16b, v27.16b
  CHECK(ruleOf(0xce062de8) == "neon_eor3");        // eor3 v8.16b, v15, v6, v11
  CHECK(ruleOf(0xce2001aa) == "neon_bcax");        // bcax v10.16b, v13, v0, v0
  CHECK(ruleOf(0xce835adf) == "neon_xar");         // xar v31.2d, v22, v3, #22
  CHECK(ruleOf(0x0dea688f) == "ld4_single_16");     // ld4 {v15-v18}.h[1], [x4], x10
  CHECK(ruleOf(0x4cd401d2) == "ld4_16b");           // ld4 {v18-v21}.16b, [x14], x20
  CHECK(ruleOf(0x4ccc06c2) == "ld4_8h");            // ld4 {v2-v5}.8h, [x22], x12
  CHECK(ruleOf(0x4cc80a7c) == "ld4_4s");            // ld4 {v28-v31}.4s, [x19], x8
  CHECK(ruleOf(0x0c0081b2) == "st2_multiple");      // st2 {v18,v19}.8b, [x13]
  CHECK(ruleOf(0x4c008214) == "st2_multiple");      // st2 {v20,v21}.16b, [x16]
  CHECK(ruleOf(0x4c000164) == "st4_multiple");      // st4 {v4-v7}.16b, [x11]
  CHECK(ruleOf(0x0db20cca) == "st2_single_8");      // st2 {v10,v11}.b[3], [x6], x18
  CHECK(ruleOf(0x0db223b9) == "st4_single_8");      // st4 {v25-v28}.b[0], [x29], x18
}

TEST_CASE("fairplayd vector coverage rules execute lane-accurate semantics",
          "[spec][vector-fp][coverage]") {
  SECTION("general INS writes one D lane and preserves the other") {
    xdec::il::Function function = lift(0x4e181d7e);
    xdec::il::Interpreter interpreter{function};
    interpreter.writeRegister(reg(function, "q30"),
                              {0x0123456789abcdef, 0xfedcba9876543210});
    interpreter.writeRegister(reg(function, "x11"),
                              {0x1122334455667788, 0});
    REQUIRE(interpreter.runBlock(function.entryBlock()).stop ==
            xdec::il::ExecStop::Return);
    CHECK(interpreter.readRegister(reg(function, "q30")) ==
          xdec::il::ConcreteValue{0x0123456789abcdef,
                                  0x1122334455667788});
  }

  SECTION("UMULL and UMULL2 select low and high source halves") {
    xdec::il::Function lowFunction = lift(0x2ea3c045);
    xdec::il::Interpreter low{lowFunction};
    low.writeRegister(reg(lowFunction, "q2"),
                      {0x0000000300000002, 0x0000000700000006});
    low.writeRegister(reg(lowFunction, "q3"),
                      {0x0000000500000004, 0x0000000900000008});
    REQUIRE(low.runBlock(lowFunction.entryBlock()).stop ==
            xdec::il::ExecStop::Return);
    CHECK(low.readRegister(reg(lowFunction, "q5")) ==
          xdec::il::ConcreteValue{8, 15});

    xdec::il::Function highFunction = lift(0x6ea3c05c);
    xdec::il::Interpreter high{highFunction};
    high.writeRegister(reg(highFunction, "q2"),
                       {0x0000000300000002, 0x0000000700000006});
    high.writeRegister(reg(highFunction, "q3"),
                       {0x0000000500000004, 0x0000000900000008});
    REQUIRE(high.runBlock(highFunction.entryBlock()).stop ==
            xdec::il::ExecStop::Return);
    CHECK(high.readRegister(reg(highFunction, "q28")) ==
          xdec::il::ConcreteValue{48, 63});
  }

  SECTION("UZP2 remains correct when destination aliases a source") {
    xdec::il::Function function = lift(0x4e9d599d);
    xdec::il::Interpreter interpreter{function};
    interpreter.writeRegister(reg(function, "q12"),
                              {0x0000000100000000, 0x0000000300000002});
    interpreter.writeRegister(reg(function, "q29"),
                              {0x0000000500000004, 0x0000000700000006});
    REQUIRE(interpreter.runBlock(function.entryBlock()).stop ==
            xdec::il::ExecStop::Return);
    CHECK(interpreter.readRegister(reg(function, "q29")) ==
          xdec::il::ConcreteValue{0x0000000300000001,
                                  0x0000000700000005});
  }

  SECTION("MLA and MLS accumulate independently in every lane") {
    xdec::il::Function mlaFunction = lift(0x4e219402);
    xdec::il::Interpreter mla{mlaFunction};
    mla.writeRegister(reg(mlaFunction, "q0"),
                      {0x0202020202020202, 0x0202020202020202});
    mla.writeRegister(reg(mlaFunction, "q1"),
                      {0x0303030303030303, 0x0303030303030303});
    mla.writeRegister(reg(mlaFunction, "q2"),
                      {0x0101010101010101, 0x0101010101010101});
    REQUIRE(mla.runBlock(mlaFunction.entryBlock()).stop ==
            xdec::il::ExecStop::Return);
    CHECK(mla.readRegister(reg(mlaFunction, "q2")) ==
          xdec::il::ConcreteValue{0x0707070707070707,
                                  0x0707070707070707});

    xdec::il::Function mlsFunction = lift(0x6ea494a2);
    xdec::il::Interpreter mls{mlsFunction};
    mls.writeRegister(reg(mlsFunction, "q5"),
                      {0x0000000300000003, 0x0000000300000003});
    mls.writeRegister(reg(mlsFunction, "q4"),
                      {0x0000000400000004, 0x0000000400000004});
    mls.writeRegister(reg(mlsFunction, "q2"),
                      {0x0000001400000014, 0x0000001400000014});
    REQUIRE(mls.runBlock(mlsFunction.entryBlock()).stop ==
            xdec::il::ExecStop::Return);
    CHECK(mls.readRegister(reg(mlsFunction, "q2")) ==
          xdec::il::ConcreteValue{0x0000000800000008,
                                  0x0000000800000008});
  }

  SECTION("signed widening and unsigned subtract-wide preserve lane meaning") {
    xdec::il::Function sshllFunction = lift(0x0f08a463);
    xdec::il::Interpreter sshll{sshllFunction};
    sshll.writeRegister(reg(sshllFunction, "q3"),
                        {0x00000000000180ff, 0});
    REQUIRE(sshll.runBlock(sshllFunction.entryBlock()).stop ==
            xdec::il::ExecStop::Return);
    const xdec::il::ConcreteValue widened =
        sshll.readRegister(reg(sshllFunction, "q3"));
    CHECK((widened.lo & 0x0000ffffffffffff) ==
          0x00000001ff80ffff);

    xdec::il::Function usubwFunction = lift(0x2e623062);
    xdec::il::Interpreter usubw{usubwFunction};
    usubw.writeRegister(reg(usubwFunction, "q3"),
                        {0x000000c800000064, 0x000001900000012c});
    usubw.writeRegister(reg(usubwFunction, "q2"),
                        {0x0004000300020001, 0});
    REQUIRE(usubw.runBlock(usubwFunction.entryBlock()).stop ==
            xdec::il::ExecStop::Return);
    CHECK(usubw.readRegister(reg(usubwFunction, "q2")) ==
          xdec::il::ConcreteValue{0x000000c600000063,
                                  0x0000018c00000129});
  }

  SECTION("SHA-3 vector helpers combine full-width inputs") {
    xdec::il::Function function = lift(0xce062de8);
    xdec::il::Interpreter interpreter{function};
    interpreter.writeRegister(reg(function, "q15"),
                              {0xffff0000ffff0000, 0xaaaaaaaaaaaaaaaa});
    interpreter.writeRegister(reg(function, "q6"),
                              {0x00ff00ff00ff00ff, 0xcccccccccccccccc});
    interpreter.writeRegister(reg(function, "q11"),
                              {0x123456789abcdef0, 0xf0f0f0f0f0f0f0f0});
    REQUIRE(interpreter.runBlock(function.entryBlock()).stop ==
            xdec::il::ExecStop::Return);
    CHECK(interpreter.readRegister(reg(function, "q8")) ==
          xdec::il::ConcreteValue{
              0xffff0000ffff0000 ^ 0x00ff00ff00ff00ff ^
                  0x123456789abcdef0,
              0xaaaaaaaaaaaaaaaa ^ 0xcccccccccccccccc ^
                  0xf0f0f0f0f0f0f0f0});
  }

  SECTION("single-lane LD4 loads four registers and applies register post-index") {
    xdec::il::Function function = lift(0x0dea688f);
    xdec::il::ExecMemory memory;
    const std::array bytes = {
        std::byte{0x11}, std::byte{0x11}, std::byte{0x22}, std::byte{0x22},
        std::byte{0x33}, std::byte{0x33}, std::byte{0x44}, std::byte{0x44}};
    memory.seed(0x2000, bytes);
    xdec::il::Interpreter interpreter{function, &memory};
    interpreter.writeRegister(reg(function, "x4"), {0x2000, 0});
    interpreter.writeRegister(reg(function, "x10"), {0x20, 0});
    REQUIRE(interpreter.runBlock(function.entryBlock()).stop ==
            xdec::il::ExecStop::Return);
    CHECK((interpreter.readRegister(reg(function, "q15")).lo & 0xffff0000) ==
          0x11110000);
    CHECK((interpreter.readRegister(reg(function, "q16")).lo & 0xffff0000) ==
          0x22220000);
    CHECK((interpreter.readRegister(reg(function, "q17")).lo & 0xffff0000) ==
          0x33330000);
    CHECK((interpreter.readRegister(reg(function, "q18")).lo & 0xffff0000) ==
          0x44440000);
    CHECK(interpreter.readRegister(reg(function, "x4")).lo == 0x2020);
  }

  SECTION("CMHS handles equality and unsigned ordering with an aliased destination") {
    xdec::il::Function function = lift(0x6ee53c05);
    xdec::il::Interpreter interpreter{function};
    interpreter.writeRegister(reg(function, "q0"), {5, 4});
    interpreter.writeRegister(reg(function, "q5"), {5, 6});
    REQUIRE(interpreter.runBlock(function.entryBlock()).stop ==
            xdec::il::ExecStop::Return);
    CHECK(interpreter.readRegister(reg(function, "q5")) ==
          xdec::il::ConcreteValue{~uint64_t{0}, 0});
  }

  SECTION("ADDV reduces all four word lanes at destination width") {
    xdec::il::Function function = lift(0x4eb1b800);
    xdec::il::Interpreter interpreter{function};
    interpreter.writeRegister(reg(function, "q0"),
                              {0x0000000200000001, 0x0000000400000003});
    REQUIRE(interpreter.runBlock(function.entryBlock()).stop ==
            xdec::il::ExecStop::Return);
    CHECK(interpreter.readRegister(reg(function, "q0")).lo == 10);
  }

  SECTION("ST2 interleaves byte lanes from two registers") {
    xdec::il::Function function = lift(0x0c0081b2);
    xdec::il::ExecMemory memory;
    std::array<std::byte, 16> zero{};
    memory.seed(0x2000, zero);
    xdec::il::Interpreter interpreter{function, &memory};
    interpreter.writeRegister(reg(function, "x13"), {0x2000, 0});
    interpreter.writeRegister(reg(function, "q18"),
                              {0x0706050403020100, 0});
    interpreter.writeRegister(reg(function, "q19"),
                              {0x1716151413121110, 0});
    REQUIRE(interpreter.runBlock(function.entryBlock()).stop ==
            xdec::il::ExecStop::Return);
    REQUIRE(memory.read(0x2000, 16));
    CHECK(memory.read(0x2000, 16)->lo == 0x1303120211011000);
    CHECK(memory.read(0x2000, 16)->hi == 0x1707160615051404);
  }

  SECTION("ST4 interleaves four full byte vectors") {
    xdec::il::Function function = lift(0x4c000164);
    xdec::il::ExecMemory memory;
    std::array<std::byte, 64> zero{};
    memory.seed(0x2000, zero);
    xdec::il::Interpreter interpreter{function, &memory};
    interpreter.writeRegister(reg(function, "x11"), {0x2000, 0});
    interpreter.writeRegister(reg(function, "q4"),
                              {0x0706050403020100, 0x0f0e0d0c0b0a0908});
    interpreter.writeRegister(reg(function, "q5"),
                              {0x1716151413121110, 0x1f1e1d1c1b1a1918});
    interpreter.writeRegister(reg(function, "q6"),
                              {0x2726252423222120, 0x2f2e2d2c2b2a2928});
    interpreter.writeRegister(reg(function, "q7"),
                              {0x3736353433323130, 0x3f3e3d3c3b3a3938});
    REQUIRE(interpreter.runBlock(function.entryBlock()).stop ==
            xdec::il::ExecStop::Return);
    REQUIRE(memory.read(0x2000, 16));
    CHECK(memory.read(0x2000, 16)->lo == 0x3121110130201000);
    CHECK(memory.read(0x2000, 16)->hi == 0x3323130332221202);
    REQUIRE(memory.read(0x2030, 16));
    CHECK(memory.read(0x2030, 16)->lo == 0x3d2d1d0d3c2c1c0c);
    CHECK(memory.read(0x2030, 16)->hi == 0x3f2f1f0f3e2e1e0e);
  }

  SECTION("single-lane ST4 stores four registers and applies register post-index") {
    xdec::il::Function function = lift(0x0db223b9);
    xdec::il::ExecMemory memory;
    std::array<std::byte, 4> zero{};
    memory.seed(0x2000, zero);
    xdec::il::Interpreter interpreter{function, &memory};
    interpreter.writeRegister(reg(function, "x29"), {0x2000, 0});
    interpreter.writeRegister(reg(function, "x18"), {0x20, 0});
    interpreter.writeRegister(reg(function, "q25"), {0x11, 0});
    interpreter.writeRegister(reg(function, "q26"), {0x22, 0});
    interpreter.writeRegister(reg(function, "q27"), {0x33, 0});
    interpreter.writeRegister(reg(function, "q28"), {0x44, 0});
    REQUIRE(interpreter.runBlock(function.entryBlock()).stop ==
            xdec::il::ExecStop::Return);
    REQUIRE(memory.read(0x2000, 4));
    CHECK(memory.read(0x2000, 4)->lo == 0x44332211);
    CHECK(interpreter.readRegister(reg(function, "x29")).lo == 0x2020);
  }
}
