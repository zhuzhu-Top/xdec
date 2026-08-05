// Every case here is "break one rule, check it is caught". The value of the
// checker is entirely in what it refuses, so that is what is tested.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "spec/spec_test_support.h"
#include "xdec/spec/check.h"
#include "xdec/spec/parse.h"

using xdec::spec::CheckResult;
using xdec::spec::check;
using xdec::spec::parseModule;

namespace {

constexpr std::string_view kArch = R"(
arch arm64 {
  endian little
  insnwidth 32
  pointer 64
  regfile gpr : bits(64) [32] {
    prefix "x"
    zero 31 as "xzr"
    view w : bits(32) = low 0 zeroext prefix "w" zero "wzr"
  }
  reg sp   : bits(64) role stack
  reg nzcv : flags     role flags
}
)";

struct Checked {
  std::unique_ptr<xdec::spec::Module> ast;
  CheckResult result;
};

[[nodiscard]] Checked run(std::string_view body) {
  const std::string text = std::string{kArch} + std::string{body};
  auto parsed = parseModule(text, "<test>");
  if (!parsed) {
    FAIL(parsed.error().format());
  }
  Checked checked;
  checked.ast = std::move(parsed).value();
  checked.result = check(*checked.ast);
  return checked;
}

/// The concatenated error text, for asserting that the right thing was said.
[[nodiscard]] std::string errorsOf(const CheckResult& result) {
  std::string text;
  for (const xdec::Diag& diag : result.report.errors) {
    text += diag.format();
    text += '\n';
  }
  return text;
}

[[nodiscard]] std::string checkFails(std::string_view body) {
  const Checked checked = run(body);
  REQUIRE_FALSE(checked.result.report.ok());
  return errorsOf(checked.result);
}

void checkPasses(std::string_view body) {
  const Checked checked = run(body);
  INFO(errorsOf(checked.result));
  CHECK(checked.result.report.ok());
}

}  // namespace

TEST_CASE("a real spec checks clean", "[spec][check]") {
  const CheckResult result = check(xdec::spec::testing::arm64Module());
  INFO(errorsOf(result));
  CHECK(result.report.ok());

  SECTION("the register file is built for the IL to use") {
    const auto* gpr = result.module->findRegFile("gpr");
    REQUIRE(gpr != nullptr);
    CHECK(gpr->count == 32);
    CHECK(gpr->bits == 64);
    REQUIRE(gpr->viewBase.size() == 1);

    const xdec::il::RegisterFile& registers = result.module->registers;
    // Elements are contiguous so that `gpr[n]` is an index, not a name lookup.
    CHECK(registers.nameOf(xdec::il::RegId{gpr->base.index() + 0}) == "x0");
    CHECK(registers.nameOf(xdec::il::RegId{gpr->base.index() + 7}) == "x7");
    // Register 31 reads as zero and is named accordingly.
    CHECK(registers.nameOf(xdec::il::RegId{gpr->base.index() + 31}) == "xzr");
    CHECK(registers.nameOf(xdec::il::RegId{gpr->viewBase[0].index() + 0}) == "w0");
    CHECK(registers.nameOf(xdec::il::RegId{gpr->viewBase[0].index() + 31}) == "wzr");
  }

  SECTION("a w-register write zeroes the top half, as AArch64 requires") {
    const auto* gpr = result.module->findRegFile("gpr");
    REQUIRE(gpr != nullptr);
    const xdec::il::RegisterInfo& w0 = result.module->registers[gpr->viewBase[0]];
    CHECK(w0.bits == 32);
    CHECK(w0.zeroExtendsParent);
    CHECK(result.module->registers.rootOf(gpr->viewBase[0]) == gpr->base);
  }

  SECTION("every field's position is recorded") {
    const auto* subs = result.module->findInsn("subs_shifted_reg");
    REQUIRE(subs != nullptr);
    REQUIRE(subs->fields.size() == 6);
    CHECK(subs->fields[0].name == "sf");
    CHECK(subs->fields[0].shift == 31);
    CHECK(subs->fields[0].bits == 1);
    const auto& last = subs->fields.back();
    CHECK(last.name == "Rd");
    CHECK(last.shift == 0);
    CHECK(last.bits == 5);
  }
}

TEST_CASE("width polymorphism is proved, not assumed", "[spec][check]") {
  SECTION("a rule covering both widths checks") {
    checkPasses(R"(
fn read(n: int(0..31), sf: int(0..1)) -> bits(32 << sf) {
  if sf == 1 { return gpr[n]; } else { return gpr[n].w; }
}
insn a {
  encoding sf:1 "0000000" _:19 Rd:5
  semantics { let v = read(Rd, sf); gpr[0] = zext(v, 64); }
}
)");
  }

  SECTION("returning the wrong half is caught") {
    // Under `sf == 1` the declared result folds to bits(64), so returning the
    // 32-bit view is a real error rather than a plausible-looking rule.
    const std::string errors = checkFails(R"(
fn read(n: int(0..31), sf: int(0..1)) -> bits(32 << sf) {
  if sf == 1 { return gpr[n].w; } else { return gpr[n].w; }
}
insn a { encoding _:32 semantics { nop(); } }
)");
    CHECK(errors.find("return value expects bits(64)") != std::string::npos);
  }

  SECTION("the else branch is only refined when the value is pinned down") {
    // `kind` ranges over four values, so `kind != 0` says nothing exact and the
    // symbolic width cannot be matched against a literal one.
    const std::string errors = checkFails(R"(
fn pick(kind: int(0..3)) -> bits(8 << kind) {
  if kind == 0 { return undef(8); } else { return undef(16); }
}
insn a { encoding _:32 semantics { nop(); } }
)");
    CHECK(errors.find("return value expects") != std::string::npos);
  }
}

TEST_CASE("operands must agree in width", "[spec][check]") {
  const std::string errors = checkFails(R"(
insn a {
  encoding _:32
  semantics { gpr[0] = zext(undef(32) + undef(64), 64); }
}
)");
  CHECK(errors.find("one width") != std::string::npos);
}

TEST_CASE("a register write must match the register", "[spec][check]") {
  SECTION("too narrow") {
    const std::string errors = checkFails(R"(
insn a { encoding _:32 semantics { gpr[0] = undef(32); } }
)");
    CHECK(errors.find("bits(64)") != std::string::npos);
  }

  SECTION("a flags register takes flags, not an integer") {
    const std::string errors = checkFails(R"(
insn a { encoding _:32 semantics { nzcv = undef(4); } }
)");
    CHECK(errors.find("flags") != std::string::npos);
  }

  SECTION("assigning a value rather than a register is refused") {
    const std::string errors = checkFails(R"(
insn a { encoding _:32 semantics { let v = undef(64); v = undef(64); } }
)");
    CHECK(errors.find("not a register") != std::string::npos);
  }
}

TEST_CASE("compile-time and runtime conditions are kept apart", "[spec][check]") {
  SECTION("`if` needs a decoded condition") {
    const std::string errors = checkFails(R"(
insn a {
  encoding _:32
  semantics { if undef(1) == undef(1) { nop(); } }
}
)");
    CHECK(errors.find("cbranch") != std::string::npos);
  }

  SECTION("a runtime choice goes through select") {
    checkPasses(R"(
insn a {
  encoding _:32
  semantics { gpr[0] = select(undef(1), undef(64), undef(64)); }
}
)");
  }

  SECTION("short-circuit operators are compile-time only") {
    const std::string errors = checkFails(R"(
insn a {
  encoding _:32
  semantics { gpr[0] = zext(undef(1) && undef(1), 64); }
}
)");
    CHECK(errors.find("compile-time only") != std::string::npos);
  }
}

TEST_CASE("register indices are bounded", "[spec][check]") {
  SECTION("a five-bit field indexing thirty-two registers is provably safe") {
    checkPasses(R"(
insn a { encoding _:27 Rd:5 semantics { gpr[Rd] = undef(64); } }
)");
  }

  SECTION("a six-bit field is not") {
    const std::string errors = checkFails(R"(
insn a { encoding _:26 Rd:6 semantics { gpr[Rd] = undef(64); } }
)");
    CHECK(errors.find("32 registers") != std::string::npos);
  }
}

TEST_CASE("calls are checked against their declarations", "[spec][check]") {
  SECTION("argument count") {
    CHECK(checkFails(R"(
fn f(a: int, b: int) -> int { return a + b; }
insn a { encoding _:32 semantics { gpr[0] = imm(f(1), 64); } }
)")
              .find("takes 2 arguments") != std::string::npos);
  }

  SECTION("a declared range is a promise the caller must keep") {
    CHECK(checkFails(R"(
fn f(n: int(0..1)) -> int { return n; }
insn a { encoding _:28 k:4 semantics { gpr[0] = imm(f(k), 64); } }
)")
              .find("ranges over 0..15") != std::string::npos);
  }

  SECTION("recursion cannot terminate and is refused") {
    CHECK(checkFails(R"(
fn f(n: int) -> int { return g(n); }
fn g(n: int) -> int { return f(n); }
insn a { encoding _:32 semantics { nop(); } }
)")
              .find("call cycle") != std::string::npos);
  }

  SECTION("a value-returning function must return on every path") {
    CHECK(checkFails(R"(
fn f(n: int(0..1)) -> int { if n == 1 { return 1; } }
insn a { encoding _:32 semantics { nop(); } }
)")
              .find("without returning") != std::string::npos);
  }
}

TEST_CASE("builtin signatures are enforced", "[spec][check]") {
  CHECK(checkFails("insn a { encoding _:32 semantics { gpr[0] = trunc(undef(32), 64); } }")
            .find("cannot widen") != std::string::npos);
  CHECK(checkFails("insn a { encoding _:32 semantics { gpr[0] = zext(undef(64), 32); } }")
            .find("cannot narrow") != std::string::npos);
  CHECK(checkFails("insn a { encoding _:32 semantics { gpr[0] = extract(undef(32), 24, 16); } }")
            .find("extract reads bits") != std::string::npos);
  CHECK(checkFails("insn a { encoding _:32 semantics { gpr[0] = imm(256, 8); } }")
            .find("does not fit") != std::string::npos);
  CHECK(checkFails("insn a { encoding _:32 semantics { store(undef(32), undef(64)); } }")
            .find("must be 64 bits") != std::string::npos);
  CHECK(checkFails("insn a { encoding _:32 semantics { nzcv = flagdef_sub(undef(32), "
                   "undef(64)); } }")
            .find("one width") != std::string::npos);
  CHECK(checkFails("insn a { encoding _:32 semantics { gpr[0] = zext(cond(nzcv, 99), 64); } }")
            .find("out of range") != std::string::npos);

  SECTION("concat adds widths") {
    checkPasses("insn a { encoding _:32 semantics { gpr[0] = concat(undef(32), undef(32)); } }");
  }

  SECTION("a select condition must be one bit") {
    CHECK(checkFails("insn a { encoding _:32 semantics { gpr[0] = select(undef(8), undef(64), "
                     "undef(64)); } }")
              .find("bits(1)") != std::string::npos);
  }
}

TEST_CASE("names are resolved", "[spec][check]") {
  CHECK(checkFails("insn a { encoding _:32 semantics { gpr[0] = zext(nope, 64); } }")
            .find("unknown name") != std::string::npos);
  CHECK(checkFails("insn a { encoding _:32 semantics { nope(); } }").find("unknown function") !=
        std::string::npos);
  CHECK(checkFails("insn a { encoding _:32 semantics { nope = undef(64); } }")
            .find("unknown register") != std::string::npos);
  CHECK(checkFails("insn a { encoding _:32 semantics { gpr[0] = vec[0]; } }")
            .find("unknown register file") != std::string::npos);
  CHECK(checkFails("insn a { encoding _:32 semantics { gpr[0] = zext(gpr[0].h, 64); } }")
            .find("no view") != std::string::npos);

  SECTION("a decoded field cannot be shadowed, because the asm template prints it") {
    CHECK(checkFails("insn a { encoding _:27 Rd:5 semantics { let Rd = 1; nop(); } }")
              .find("cannot be rebound") != std::string::npos);
  }

  SECTION("reserved names are reserved") {
    CHECK(checkFails("insn a { encoding _:6 insn_pc:26 semantics { nop(); } }")
              .find("reserved") != std::string::npos);
  }
}

TEST_CASE("declaration mistakes are caught", "[spec][check]") {
  CHECK(checkFails("insn a { encoding _:16 semantics { nop(); } }")
            .find("16 bits but the architecture") != std::string::npos);
  CHECK(checkFails("insn a { encoding Rd:5 Rd:5 _:22 semantics { nop(); } }")
            .find("appears twice") != std::string::npos);
  CHECK(checkFails(R"(
insn a { encoding _:32 semantics { nop(); } }
insn a { encoding _:32 semantics { nop(); } }
)")
            .find("more than once") != std::string::npos);
  CHECK(checkFails("fn zext(a: int) -> int { return a; }\ninsn a { encoding _:32 semantics { "
                   "nop(); } }")
            .find("builtin") != std::string::npos);
}

TEST_CASE("a discarded value is worth a warning", "[spec][check]") {
  const Checked checked =
      run("insn a { encoding _:32 semantics { load(undef(64), 32); } }");
  CHECK(checked.result.report.ok());
  CHECK_FALSE(checked.result.report.warnings.empty());
}

TEST_CASE("every builtin has a name", "[spec][check]") {
  const auto& names = xdec::spec::builtinNames();
  CHECK(names.size() > 50);
  for (const std::string& name : names) {
    CHECK_FALSE(name.empty());
  }
}
