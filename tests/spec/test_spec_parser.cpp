// Parsing the shapes a real spec uses, and refusing the ones it must not.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>

#include "spec/spec_test_support.h"
#include "xdec/spec/parse.h"

using xdec::spec::AsmPieceKind;
using xdec::spec::InsnDecl;
using xdec::spec::Module;
using xdec::spec::parseModule;

namespace {

[[nodiscard]] std::unique_ptr<Module> parseOk(std::string_view text) {
  auto parsed = parseModule(text, "<test>");
  if (!parsed) {
    FAIL(parsed.error().format());
  }
  return std::move(parsed).value();
}

[[nodiscard]] std::string parseError(std::string_view text) {
  auto parsed = parseModule(text, "<test>");
  REQUIRE_FALSE(parsed);
  return parsed.error().format();
}

constexpr std::string_view kMinimalArch = R"(
arch arm64 {
  endian little
  insnwidth 32
  regfile gpr : bits(64) [32] { prefix "x" }
  reg nzcv : flags role flags
}
)";

[[nodiscard]] std::string withArch(std::string_view body) {
  return std::string{kMinimalArch} + std::string{body};
}

}  // namespace

TEST_CASE("a real spec parses", "[spec][parser]") {
  const xdec::spec::Module* const module = &xdec::spec::testing::arm64Module();

  CHECK(module->arch.name == "arm64");
  CHECK(module->arch.insnWidth == 32);
  CHECK(module->arch.pointerBits == 64);
  // Named rather than indexed: which files the spec declares is its business,
  // and asserting a count makes adding one a test edit instead of a review.
  const auto fileNamed = [&](std::string_view name) {
    const auto found = std::ranges::find_if(
        module->arch.regFiles, [&](const auto& file) { return file.name == name; });
    REQUIRE(found != module->arch.regFiles.end());
    return found;
  };

  const auto gpr = fileNamed("gpr");
  CHECK(gpr->count == 32);
  CHECK(gpr->bits == 64);
  CHECK(gpr->role == xdec::spec::RegRole::General);
  CHECK(gpr->zeroIndex.has_value());
  REQUIRE(gpr->views.size() == 1);
  CHECK(gpr->views[0].zeroExtends);

  const auto vec = fileNamed("vec");
  CHECK(vec->count == 32);
  CHECK(vec->bits == 128);
  CHECK(vec->role == xdec::spec::RegRole::Vector);
  CHECK(std::ranges::all_of(vec->views, [](const auto& view) { return view.zeroExtends; }));
  CHECK(module->arch.regs.size() == 2);
  // Counted loosely on purpose: an exact total turns every addition to the
  // architecture spec into a test edit, which trains people to update the
  // number without reading what else moved.
  CHECK(module->functions.size() >= 4);
  CHECK(module->instructions.size() >= 100);
}

TEST_CASE("an encoding becomes a mask and a value", "[spec][parser]") {
  const xdec::spec::Module* const module = &xdec::spec::testing::arm64Module();

  const InsnDecl* subs = nullptr;
  for (const InsnDecl& insn : module->instructions) {
    if (insn.name == "subs_shifted_reg") {
      subs = &insn;
    }
  }
  REQUIRE(subs != nullptr);
  CHECK(subs->encoding.width == 32);

  // sf:1 "1101011" shift:2 "0" Rm:5 imm6:6 Rn:5 Rd:5
  // Only the literal runs are constrained: bits 30..24 and bit 21.
  CHECK(subs->encoding.mask == 0x7f200000);
  CHECK(subs->encoding.value == 0x6b000000);

  SECTION("fields are placed most significant first") {
    // `sf` is written first, so it is the top bit.
    const auto& items = subs->encoding.items;
    REQUIRE(items.size() == 8);
    CHECK(items[0].field == "sf");
    CHECK(items[0].bits == 1);
    CHECK(items[1].isLiteral);
    CHECK(items[7].field == "Rd");
  }
}

TEST_CASE("an asm template keeps its structure", "[spec][parser]") {
  const xdec::spec::Module* const module = &xdec::spec::testing::arm64Module();

  const InsnDecl* sub = nullptr;
  for (const InsnDecl& insn : module->instructions) {
    if (insn.name == "sub_shifted_reg") {
      sub = &insn;
    }
  }
  REQUIRE(sub != nullptr);
  REQUIRE(sub->asmTemplate.has_value());

  bool sawSubstitution = false;
  bool sawGroup = false;
  for (const auto& piece : sub->asmTemplate->pieces) {
    sawSubstitution = sawSubstitution || piece->kind == AsmPieceKind::Substitution;
    sawGroup = sawGroup || piece->kind == AsmPieceKind::OptionalGroup;
  }
  CHECK(sawSubstitution);
  // The `[, lsl #n]` tail is optional, which the decoder needs to know so that
  // it prints `sub x0, x1, x2` rather than `sub x0, x1, x2, lsl #0`.
  CHECK(sawGroup);

  SECTION("a style and its argument are separated") {
    const auto& first = sub->asmTemplate->pieces[1];
    REQUIRE(first->kind == AsmPieceKind::Substitution);
    CHECK(first->style == "reg");
    REQUIRE(first->styleArgument != nullptr);
    CHECK(first->styleArgument->name == "sf");
  }

  SECTION("an escaped bracket is literal text") {
    const xdec::spec::Module* const module2 = &xdec::spec::testing::arm64Module();
    const InsnDecl* ldr = nullptr;
    for (const InsnDecl& insn : module2->instructions) {
      if (insn.name == "ldr_imm_unsigned") {
        ldr = &insn;
      }
    }
    REQUIRE(ldr != nullptr);
    REQUIRE(ldr->asmTemplate.has_value());
    bool sawBracket = false;
    for (const auto& piece : ldr->asmTemplate->pieces) {
      if (piece->kind == AsmPieceKind::Text && piece->text.find('[') != std::string::npos) {
        sawBracket = true;
      }
    }
    CHECK(sawBracket);
    for (const auto& piece : ldr->asmTemplate->pieces) {
      CHECK(piece->kind != AsmPieceKind::OptionalGroup);
    }
  }
}

TEST_CASE("the arch block is mandatory and validated", "[spec][parser]") {
  CHECK(parseError("fn f() { nop(); }").find("expected 'arch'") != std::string::npos);
  CHECK(parseError("arch arm64 { endian little }").find("insnwidth") != std::string::npos);
  CHECK(parseError("arch sparc { endian little insnwidth 32 }").find("unknown architecture") !=
        std::string::npos);

  SECTION("a variable-length architecture is refused rather than half-supported") {
    CHECK(parseError("arch x86_64 { endian little insnwidth 0 }").find("insnwidth") !=
          std::string::npos);
  }
}

TEST_CASE("an insn must be complete", "[spec][parser]") {
  CHECK(parseError(withArch("insn a { encoding _:32 }")).find("no semantics") !=
        std::string::npos);
  CHECK(parseError(withArch("insn a { semantics { nop(); } }")).find("no encoding") !=
        std::string::npos);
  CHECK(parseError(withArch("insn a { encoding _:32 encoding _:32 semantics { nop(); } }"))
            .find("more than one encoding") != std::string::npos);
  CHECK(parseError(withArch("insn a { encoding \"012\" semantics { nop(); } }"))
            .find("only contain 0 and 1") != std::string::npos);
}

TEST_CASE("where an encoding ends is unambiguous", "[spec][parser]") {
  // `asm` and `semantics` are identifiers, and so are field names. What
  // separates them is the colon, which the parser looks one token ahead for.
  const auto module =
      parseOk(withArch(R"(
insn a {
  encoding "0000" Rd:5 _:23
  asm "a {Rd:reg(1)}"
  semantics { nop(); }
}
)"));
  REQUIRE(module->instructions.size() == 1);
  CHECK(module->instructions[0].encoding.items.size() == 3);
  CHECK(module->instructions[0].asmTemplate.has_value());
}

TEST_CASE("declarations that cannot mean anything are refused", "[spec][parser]") {
  CHECK(parseError(withArch("insn a { encoding _:65 semantics { nop(); } }"))
            .find("between 1 and 64") != std::string::npos);
  CHECK(parseError("arch arm64 { insnwidth 32 regfile gpr : bits(64) [32] { } }")
            .find("name prefix") != std::string::npos);
  CHECK(parseError(R"(
arch arm64 {
  insnwidth 32
  regfile gpr : bits(64) [32] { prefix "x" view w : bits(96) = low 0 prefix "w" }
}
)")
            .find("reaches past") != std::string::npos);
}

TEST_CASE("a spec is read across the files it includes", "[spec][parser]") {
  // The real spec, which is a root holding the architecture and a list of
  // includes. If the includes were not followed this would parse to an
  // architecture and nothing else, so the counts are the assertion that matters.
  const xdec::spec::Module& module = xdec::spec::testing::arm64Module();
  CHECK(module.arch.name == "arm64");
  CHECK(module.functions.size() >= 4);
  CHECK(module.instructions.size() >= 100);

  // Rules from files listed first and last both arrive, so the walk is not
  // stopping at the first include.
  const auto has = [&](std::string_view name) {
    return std::ranges::any_of(module.instructions,
                               [&](const InsnDecl& insn) { return insn.name == name; });
  };
  CHECK(has("adrp"));
  CHECK(has("simd_fp_unmodelled"));
}

TEST_CASE("an included file cannot redeclare the architecture", "[spec][parser]") {
  auto parsed = xdec::spec::parseFragment(kMinimalArch, "<fragment>");
  REQUIRE_FALSE(parsed);
  CHECK(parsed.error().format().find("cannot declare an arch block") != std::string::npos);
}

TEST_CASE("a missing include is reported against the file naming it", "[spec][parser]") {
  auto parsed = xdec::spec::parseSpecFile(xdec::spec::testing::specDir() / "no-such.xspec");
  REQUIRE_FALSE(parsed);
  CHECK(parsed.error().format().find("cannot open spec") != std::string::npos);
}

TEST_CASE("an integer range parses", "[spec][parser]") {
  const auto module = parseOk(withArch("fn f(n: int(0..31)) -> int { return n; }"));
  REQUIRE(module->functions.size() == 1);
  const auto& param = module->functions[0].params[0];
  CHECK(param.type.hasRange);
  CHECK(param.type.rangeLow == 0);
  CHECK(param.type.rangeHigh == 31);

  CHECK(parseError(withArch("fn f(n: int(31..0)) -> int { return n; }")).find("lower bound") !=
        std::string::npos);
}
