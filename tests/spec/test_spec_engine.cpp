// Decoding, probing and lifting, checked against real AArch64 instruction words.
//
// The words below are what an assembler actually emits, not values derived from
// the spec. That is the point: a test built from the same encoding table it is
// testing would agree with a spec that had the bit order backwards.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

#include "spec/spec_test_support.h"
#include "xdec/il/printer.h"
#include "xdec/il/verify.h"
#include "xdec/spec/compile.h"
#include "xdec/spec/engine.h"

using xdec::spec::DecodedInsn;
using xdec::spec::FlowKind;
using xdec::spec::SpecEngine;

namespace {

[[nodiscard]] const SpecEngine& engine() {
  static const std::unique_ptr<SpecEngine> kEngine = [] {
    auto loaded = xdec::spec::loadSpecFile(xdec::spec::testing::arm64SpecPath());
    if (!loaded) {
      FAIL(loaded.error().format());
    }
    return std::move(loaded).value();
  }();
  return *kEngine;
}

/// Little-endian bytes of one instruction word, as they sit in the file.
/// Catch's INFO builds a message with <<, which binds tighter than ?:, so the
/// conditional needs to be a call rather than an inline expression.
[[nodiscard]] std::string errorText(const auto& result) {
  return result ? std::string{} : result.error().format();
}

/// A word in the reserved encoding group, which no rule and no catch-all claims.
constexpr uint32_t kUnallocated = 0x00010000;

[[nodiscard]] std::array<std::byte, 4> encode(uint32_t word) {
  return {static_cast<std::byte>(word & 0xFF), static_cast<std::byte>((word >> 8) & 0xFF),
          static_cast<std::byte>((word >> 16) & 0xFF),
          static_cast<std::byte>((word >> 24) & 0xFF)};
}

[[nodiscard]] DecodedInsn decode(uint32_t word, uint64_t address = 0x1000) {
  const auto bytes = encode(word);
  return engine().decode(bytes, address);
}

[[nodiscard]] std::string ruleOf(uint32_t word) {
  const DecodedInsn insn = decode(word);
  if (!insn.valid) {
    return "<undecoded>";
  }
  return engine().program().instructions[insn.instruction].name;
}

[[nodiscard]] uint64_t fieldOf(const DecodedInsn& insn, std::string_view name) {
  const auto& fields = engine().program().instructions[insn.instruction].fields;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (fields[index].name == name) {
      return insn.fields[index];
    }
  }
  FAIL("no field " << name);
  return 0;
}

}  // namespace

TEST_CASE("real instruction words decode to the right rule", "[spec][engine]") {
  CHECK(ruleOf(0xcb020020) == "sub_shifted_reg");    // sub  x0, x1, x2
  CHECK(ruleOf(0xeb020020) == "subs_shifted_reg");   // subs x0, x1, x2
  CHECK(ruleOf(0x6b020020) == "subs_shifted_reg");   // subs w0, w1, w2
  CHECK(ruleOf(0x91004020) == "add_imm");            // add  x0, x1, #0x10
  CHECK(ruleOf(0x14000002) == "b_uncond");           // b    +8
  CHECK(ruleOf(0x54000040) == "b_cond");             // b.eq +8
  CHECK(ruleOf(0xd61f0100) == "br_reg");             // br   x8
  CHECK(ruleOf(0xd65f03c0) == "ret_reg");            // ret
  CHECK(ruleOf(0xf9400420) == "ldr_imm_unsigned");   // ldr  x0, [x1, #8]
  CHECK(ruleOf(0xd5033bbf) == "dmb_barrier");        // dmb  ish

  SECTION("an alias wins over the encoding it refines") {
    // cmp x1, x2 is subs xzr, x1, x2. Decoding it as subs would be defensible
    // and useless, so the more specific rule has to be tried first.
    CHECK(ruleOf(0xeb02003f) == "cmp_shifted_reg");
  }

  SECTION("a word matching nothing is reported, not guessed at") {
    // A word in the reserved group at the top of the encoding table. 0xffffffff
    // would not do: that is a vector operation, which the spec knows about even
    // though it does not model it.
    const DecodedInsn insn = decode(kUnallocated);
    INFO(ruleOf(kUnallocated));
    CHECK_FALSE(insn.valid);
    CHECK(insn.word == kUnallocated);
  }

  SECTION("a truncated buffer decodes to nothing") {
    const auto bytes = encode(0xd65f03c0);
    CHECK_FALSE(engine().decode(std::span{bytes}.first(3), 0x1000).valid);
  }
}

TEST_CASE("fuzz-found over-decodes stay undecodable", "[spec][engine]") {
  // Every word here was decoded by xdec and rejected by capstone in a
  // fuzz_decode.py run; each exposed a missing encoding constraint. The tests
  // pin the constraints so a spec edit cannot quietly reopen the hole.
  SECTION("32-bit shifted-register forms reject imm6 >= 32") {
    CHECK_FALSE(decode(0x4a488f58).valid);  // eor w24, w26, w8, lsr #35
    CHECK_FALSE(decode(0x4b4d8474).valid);  // shifted w-form, imm6 = 33
  }

  SECTION("add/sub shifted-register forms reject shift type ROR") {
    CHECK_FALSE(decode(0x8bcce7cd).valid);  // add x13, x30, x12, ror #57
  }

  SECTION("logical-immediate reserved encodings") {
    // imms == 59 with N == 0 (s == levels) and the all-ones mask it yields.
    CHECK_FALSE(decode(0xd217eecb).valid);  // eor x11, x22, #0xffff...f
    CHECK_FALSE(decode(0xf204bf26).valid);  // ands x6, x25, #0xffff...f
    CHECK_FALSE(decode(0x923dbee6).valid);  // and  x6, x23, #0xffff...f
  }

  SECTION("32-bit bitfield forms reject immr/imms >= 32") {
    CHECK_FALSE(decode(0x5323c0cc).valid);  // ubfx  w12, w6, #35, #14
    CHECK_FALSE(decode(0x5336ca08).valid);  // ubfiz w8, w16, .., #51
    CHECK_FALSE(decode(0x13284c79).valid);  // sbfiz w25, w3, .., #20
    CHECK_FALSE(decode(0x131cba7a).valid);  // sbfx  w26, w19, #28, #19
    CHECK_FALSE(decode(0x332f92ed).valid);  // bfi   w13, w23, .., #37
    CHECK_FALSE(decode(0x3314b305).valid);  // bfxil w5, w24, #20, #25
  }

  SECTION("writeback load/store rejects Rn == Rt(/Rt2)") {
    // These sit inside the memory_unmodelled catch-all's bit space, and the
    // catch-all cannot exclude them: Rn == Rt is a register *relationship*, not
    // a bit pattern. What matters is that no modelled rule claims them — they
    // surface as the opaque sink (asm prints `ldst #...`), never as a real
    // ldr/stp with fabricated semantics.
    CHECK(ruleOf(0xb8572cc6) == "memory_unmodelled");  // ldr w6, [x6, #-142]!
    CHECK(ruleOf(0x2896b929) == "memory_unmodelled");  // stp w9, w14, [x9], #180
  }

  SECTION("load-pair rejects Rt == Rt2") {
    CHECK(ruleOf(0x6df8d816) == "memory_unmodelled");  // ldp d22, d22, [x0, ..]!
    CHECK(ruleOf(0x28da7f1f) == "memory_unmodelled");  // ldp wzr, wzr, [x24], #208
    CHECK(ruleOf(0xa9400020) == "memory_unmodelled");  // ldp x0, x0, [x1]
  }

  SECTION("unallocated PAC data-processing opcodes") {
    CHECK_FALSE(decode(0xdac15382).valid);  // op = 21; only 0..7 exist
  }

  SECTION("the neighbouring valid encodings still decode") {
    CHECK(ruleOf(0x8b13c2e4) == "add_shifted_reg");  // add x4, x23, x19, lsl #48
    CHECK(ruleOf(0x0a1f2fe0) == "and_shifted_reg");  // and w0, wzr, wzr, lsl #11
    CHECK(ruleOf(0x9b5936b3) == "smulh");            // smulh x19, x21, x25 (Ra ignored)
    CHECK(ruleOf(0xdac10062) == "pacia");            // pacia x2, x3
    CHECK(ruleOf(0xd50b7b24) == "dc_cvau");          // dc cvau, x4
    CHECK(ruleOf(0xa9410022) == "ldp_offset");       // ldp x2, x0, [x1, #0x10]
  }
}

TEST_CASE("fields are extracted from the right bits", "[spec][engine]") {
  const DecodedInsn subs = decode(0xeb020020);  // subs x0, x1, x2
  CHECK(fieldOf(subs, "sf") == 1);
  CHECK(fieldOf(subs, "Rd") == 0);
  CHECK(fieldOf(subs, "Rn") == 1);
  CHECK(fieldOf(subs, "Rm") == 2);
  CHECK(fieldOf(subs, "imm6") == 0);
  CHECK(fieldOf(subs, "shift") == 0);

  SECTION("the 32-bit form differs only in sf") {
    CHECK(fieldOf(decode(0x6b020020), "sf") == 0);
  }

  SECTION("an immediate keeps its scale") {
    // ldr x0, [x1, #8] encodes 8 as 1, because the offset is scaled by the
    // access size. The spec, not the decoder, is what knows that.
    CHECK(fieldOf(decode(0xf9400420), "imm12") == 1);
    CHECK(fieldOf(decode(0xf9400420), "sz") == 1);
  }

  SECTION("add's immediate is not scaled") {
    CHECK(fieldOf(decode(0x91004020), "imm12") == 0x10);
  }
}

TEST_CASE("control flow is discovered without building IL", "[spec][engine]") {
  SECTION("an unconditional branch names its target") {
    const DecodedInsn insn = decode(0x14000002, 0x1000);  // b +8
    const auto flow = engine().probe(insn);
    CHECK(flow.kind == FlowKind::Branch);
    CHECK(flow.target == 0x1008);
    CHECK(flow.terminates());
  }

  SECTION("a backward branch sign-extends") {
    // b -8 is imm26 = -2, which only lands in the right place if the spec's
    // sextint is applied at the field's own width.
    const uint32_t word = 0x14000000 | (0x3fffffe & 0x3ffffff);
    const DecodedInsn insn = decode(word, 0x1000);
    const auto flow = engine().probe(insn);
    CHECK(flow.kind == FlowKind::Branch);
    CHECK(flow.target == 0x0ff8);
  }

  SECTION("a conditional branch names both successors") {
    const DecodedInsn insn = decode(0x54000040, 0x2000);  // b.eq +8
    const auto flow = engine().probe(insn);
    CHECK(flow.kind == FlowKind::CondBranch);
    CHECK(flow.target == 0x2008);
    CHECK(flow.fallthrough == 0x2004);
  }

  SECTION("a computed branch is recorded as unresolved rather than dropped") {
    const auto flow = engine().probe(decode(0xd61f0100));  // br x8
    CHECK(flow.kind == FlowKind::IndirectBranch);
    CHECK(flow.terminates());
  }

  SECTION("a return terminates") {
    CHECK(engine().probe(decode(0xd65f03c0)).kind == FlowKind::Return);
  }

  SECTION("arithmetic falls through") {
    const auto flow = engine().probe(decode(0xeb020020, 0x1000));
    CHECK(flow.kind == FlowKind::Fallthrough);
    CHECK(flow.fallthrough == 0x1004);
    CHECK_FALSE(flow.terminates());
  }

  SECTION("an undecodable word is unknown, not fallthrough") {
    CHECK(engine().probe(decode(kUnallocated)).kind == FlowKind::Unknown);
  }
}

TEST_CASE("disassembly renders", "[spec][engine]") {
  CHECK(engine().disassemble(decode(0xeb020020)) == "subs x0, x1, x2");
  CHECK(engine().disassemble(decode(0x6b020020)) == "subs w0, w1, w2");
  CHECK(engine().disassemble(decode(0xeb02003f)) == "cmp x1, x2");
  CHECK(engine().disassemble(decode(0xd65f03c0)) == "ret");
  CHECK(engine().disassemble(decode(0x91004020)) == "add x0, x1, #0x10");

  SECTION("a zero register prints by name") {
    // `sub x0, xzr, x2` is spelled `neg`, so a source-side 31 has to be found
    // somewhere no alias takes over. A logical operation has none.
    CHECK(engine().disassemble(decode(0xeb02003e)) == "subs x30, x1, x2");
    CHECK(engine().disassemble(decode(0x8a0203e0)) == "and x0, xzr, x2");
  }

  SECTION("an optional operand is omitted when it is the default") {
    // sub x0, x1, x2 with no shift must not print `, lsl #0`.
    CHECK(engine().disassemble(decode(0xcb020020)) == "sub x0, x1, x2");
    // sub x0, x1, x2, lsl #4
    CHECK(engine().disassemble(decode(0xcb021020)) == "sub x0, x1, x2, lsl #4");
  }

  SECTION("register 31 is the stack pointer where the encoding says so") {
    // A 31 in Rn is the stack pointer for add-immediate and the zero register
    // for a logical operation. Printing either one the other way is wrong.
    CHECK(engine().disassemble(decode(0x910083fd)) == "add x29, sp, #0x20");
    CHECK(engine().disassemble(decode(0x8a0203e0)) == "and x0, xzr, x2");
  }

  SECTION("a scaled immediate prints its real value") {
    // ldr x8, [x8, #0x28] encodes 0x28 as 5. Printing the field verbatim would
    // silently disagree with every other disassembler.
    CHECK(engine().disassemble(decode(0xf9401508)) == "ldr x8, [x8, #0x28]");
    // The 32-bit form scales by four, not eight.
    CHECK(engine().disassemble(decode(0xb9401508)) == "ldr w8, [x8, #0x14]");
  }

  SECTION("a shifted immediate prints its real value") {
    // add x0, x1, #0x10, lsl #12
    CHECK(engine().disassemble(decode(0x91404020)) == "add x0, x1, #0x10000");
  }

  SECTION("a backward branch resolves behind the instruction") {
    // b -0x2c at 0x8442c. Rendering the field unsigned would put the target
    // 0x10000000 bytes away, which is the kind of wrong that looks plausible.
    CHECK(engine().disassemble(decode(0x17fffff5, 0x8442c)) == "b 0x84400");
    CHECK(engine().disassemble(decode(0x14000005, 0x843cc)) == "b 0x843e0");
  }

  SECTION("a condition code prints as a mnemonic") {
    CHECK(engine().disassemble(decode(0x54000040, 0)).starts_with("b.eq "));
    CHECK(engine().disassemble(decode(0x54000041, 0)).starts_with("b.ne "));
  }

  SECTION("an undecodable word prints as data") {
    CHECK(engine().disassemble(decode(kUnallocated)) == ".word 0x00010000");
  }
}

TEST_CASE("semantics elaborate to IL", "[spec][engine]") {
  xdec::il::Function function{engine().program().arch, engine().program().registers, 0x1000};
  const xdec::il::BlockId block = function.createBlock(0x1000);
  function.setEntryBlock(block);

  xdec::spec::LiftSite site;
  site.function = &function;
  site.block = block;
  site.address = 0x1000;
  site.blockAt = [&](uint64_t) { return block; };

  const DecodedInsn subs = decode(0xeb020020, 0x1000);  // subs x0, x1, x2
  auto lifted = engine().elaborate(subs, site);
  INFO(errorText(lifted));
  REQUIRE(lifted);

  const std::string text = xdec::il::print(function);
  INFO(text);

  SECTION("the operands are read from the named registers") {
    CHECK(text.find("read x1") != std::string::npos);
    CHECK(text.find("read x2") != std::string::npos);
  }

  SECTION("the result is written back") {
    CHECK(text.find("write x0") != std::string::npos);
  }

  SECTION("the flags stay lazy") {
    // One opaque node, not four bit computations. This is what makes folding an
    // opaque predicate a single rewrite later on.
    CHECK(text.find("flagdef:sub.64") != std::string::npos);
    CHECK(text.find("write nzcv") != std::string::npos);
  }

  SECTION("every op carries the instruction's address") {
    CHECK(text.find("@0x1000") != std::string::npos);
    for (const xdec::il::OpId op : function.block(block).ops) {
      CHECK(function.op(op).va == 0x1000);
    }
  }
}

TEST_CASE("the 32-bit form writes the 32-bit view", "[spec][engine]") {
  xdec::il::Function function{engine().program().arch, engine().program().registers, 0x1000};
  const xdec::il::BlockId block = function.createBlock(0x1000);
  function.setEntryBlock(block);

  xdec::spec::LiftSite site;
  site.function = &function;
  site.block = block;
  site.address = 0x1000;
  site.blockAt = [&](uint64_t) { return block; };

  REQUIRE(engine().elaborate(decode(0x6b020020, 0x1000), site));  // subs w0, w1, w2
  const std::string text = xdec::il::print(function);
  INFO(text);
  CHECK(text.find("read w1") != std::string::npos);
  CHECK(text.find("write w0") != std::string::npos);
  CHECK(text.find("flagdef:sub.32") != std::string::npos);
  // The 64-bit registers must not appear: picking the wrong view is exactly the
  // bug the declared width `bits(32 << sf)` exists to prevent.
  CHECK(text.find("read x1") == std::string::npos);
}

TEST_CASE("a write to the zero register is discarded", "[spec][engine]") {
  xdec::il::Function function{engine().program().arch, engine().program().registers, 0x1000};
  const xdec::il::BlockId block = function.createBlock(0x1000);
  function.setEntryBlock(block);

  xdec::spec::LiftSite site;
  site.function = &function;
  site.block = block;
  site.address = 0x1000;
  site.blockAt = [&](uint64_t) { return block; };

  // sub xzr, x1, x2: the destination is the zero register, so nothing is stored.
  REQUIRE(engine().elaborate(decode(0xcb02003f, 0x1000), site));
  const std::string text = xdec::il::print(function);
  INFO(text);
  CHECK(text.find("write xzr") == std::string::npos);
}

TEST_CASE("branches resolve to blocks", "[spec][engine]") {
  xdec::il::Function function{engine().program().arch, engine().program().registers, 0x1000};
  const xdec::il::BlockId entry = function.createBlock(0x1000);
  const xdec::il::BlockId taken = function.createBlock(0x1008);
  const xdec::il::BlockId next = function.createBlock(0x1004);
  function.setEntryBlock(entry);

  xdec::spec::LiftSite site;
  site.function = &function;
  site.block = entry;
  site.address = 0x1000;
  site.blockAt = [&](uint64_t address) {
    switch (address) {
      case 0x1000:
        return entry;
      case 0x1004:
        return next;
      case 0x1008:
        return taken;
      default:
        return xdec::il::BlockId{};
    }
  };

  REQUIRE(engine().elaborate(decode(0x54000040, 0x1000), site));  // b.eq +8
  function.appendReturn(taken, 0x1008);
  function.appendReturn(next, 0x1004);
  function.rebuildEdges();

  CHECK(function.block(entry).successors.size() == 2);
  const std::string text = xdec::il::print(function);
  INFO(text);
  CHECK(text.find("flagcond:eq") != std::string::npos);

  SECTION("a target outside the function is an error, not a wrong edge") {
    xdec::il::Function other{engine().program().arch, engine().program().registers, 0x1000};
    const xdec::il::BlockId only = other.createBlock(0x1000);
    xdec::spec::LiftSite lost;
    lost.function = &other;
    lost.block = only;
    lost.address = 0x1000;
    lost.blockAt = [](uint64_t) { return xdec::il::BlockId{}; };
    CHECK_FALSE(engine().elaborate(decode(0x14000002, 0x1000), lost));
  }
}

TEST_CASE("lifted IL satisfies the verifier", "[spec][engine]") {
  xdec::il::Function function{engine().program().arch, engine().program().registers, 0x1000};
  const xdec::il::BlockId block = function.createBlock(0x1000);
  function.setEntryBlock(block);
  function.setMaturity(xdec::il::Maturity::Lifted);

  xdec::spec::LiftSite site;
  site.function = &function;
  site.block = block;
  site.blockAt = [&](uint64_t) { return block; };

  // A short straight-line run, then a return, so the block is well formed.
  const uint32_t words[] = {0xeb020020, 0x91004020, 0xf9400420, 0xd5033bbf, 0xd65f03c0};
  uint64_t address = 0x1000;
  for (const uint32_t word : words) {
    site.address = address;
    const DecodedInsn insn = decode(word, address);
    REQUIRE(insn.valid);
    auto lifted = engine().elaborate(insn, site);
    INFO(errorText(lifted));
    REQUIRE(lifted);
    address += insn.length;
  }
  function.block(block).endVa = address;
  function.rebuildEdges();

  const auto report = xdec::il::verify(function);
  INFO(report.format());
  INFO(xdec::il::print(function));
  CHECK(report.ok());
}

TEST_CASE("an intrinsic keeps an unmodelled effect visible", "[spec][engine]") {
  xdec::il::Function function{engine().program().arch, engine().program().registers, 0x1000};
  const xdec::il::BlockId block = function.createBlock(0x1000);
  function.setEntryBlock(block);

  xdec::spec::LiftSite site;
  site.function = &function;
  site.block = block;
  site.address = 0x1000;
  site.blockAt = [&](uint64_t) { return block; };

  REQUIRE(engine().elaborate(decode(0xd5033bbf, 0x1000), site));  // dmb ish
  const std::string text = xdec::il::print(function);
  INFO(text);
  // A barrier lowered to a nop would let a later pass reorder across it.
  CHECK(text.find("aarch64.dmb") != std::string::npos);
  CHECK(text.find("nop") == std::string::npos);
}

TEST_CASE("an undecodable word lifts to unimplemented", "[spec][engine]") {
  xdec::il::Function function{engine().program().arch, engine().program().registers, 0x1000};
  const xdec::il::BlockId block = function.createBlock(0x1000);
  function.setEntryBlock(block);

  xdec::spec::LiftSite site;
  site.function = &function;
  site.block = block;
  site.address = 0x1000;
  site.blockAt = [&](uint64_t) { return block; };

  REQUIRE(engine().elaborate(decode(kUnallocated, 0x1000), site));
  const std::string text = xdec::il::print(function);
  INFO(text);
  // Lifting it to a nop would silently claim the instruction does nothing.
  CHECK(text.find("unimplemented") != std::string::npos);
}

TEST_CASE("a program survives the blob round trip", "[spec][engine]") {
  auto compiled =
      xdec::spec::compileFile(xdec::spec::testing::arm64SpecPath());
  REQUIRE(compiled);
  const std::unique_ptr<xdec::spec::SpecProgram> original = std::move(compiled).value();

  const std::vector<std::byte> blob = xdec::spec::serialize(*original);
  CHECK(blob.size() > 1000);

  auto reloaded = xdec::spec::deserialize(blob);
  INFO(errorText(reloaded));
  REQUIRE(reloaded);
  const xdec::spec::SpecProgram& copy = *reloaded.value();

  CHECK(copy.arch == original->arch);
  CHECK(copy.insnWidth == original->insnWidth);
  CHECK(copy.registers.size() == original->registers.size());
  CHECK(copy.code.size() == original->code.size());
  CHECK(copy.instructions.size() == original->instructions.size());
  CHECK(copy.patterns.size() == original->patterns.size());
  CHECK(copy.registers.nameOf(xdec::il::RegId{31}) == "xzr");

  SECTION("and the reloaded program decodes and lifts identically") {
    const SpecEngine reloadedEngine{std::move(reloaded).value()};
    const auto bytes = encode(0xeb020020);
    const DecodedInsn fromBlob = reloadedEngine.decode(bytes, 0x1000);
    REQUIRE(fromBlob.valid);
    CHECK(reloadedEngine.disassemble(fromBlob) == "subs x0, x1, x2");
    CHECK(reloadedEngine.probe(fromBlob).kind == FlowKind::Fallthrough);
  }
}

TEST_CASE("a corrupt blob is refused", "[spec][engine]") {
  auto compiled =
      xdec::spec::compileFile(xdec::spec::testing::arm64SpecPath());
  REQUIRE(compiled);
  std::vector<std::byte> blob = xdec::spec::serialize(*compiled.value());

  SECTION("bad magic") {
    blob[0] = std::byte{0};
    CHECK_FALSE(xdec::spec::deserialize(blob));
  }

  SECTION("truncated") {
    blob.resize(blob.size() / 2);
    CHECK_FALSE(xdec::spec::deserialize(blob));
  }

  SECTION("empty") {
    CHECK_FALSE(xdec::spec::deserialize({}));
  }

  SECTION("a wrong version is named rather than misread") {
    blob[4] = std::byte{99};
    auto loaded = xdec::spec::deserialize(blob);
    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().format().find("version") != std::string::npos);
  }
}
