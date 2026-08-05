// print -> parse -> print must be an exact fixed point.
//
// This is the test that keeps the printer and the parser from drifting apart,
// and it is why the text form can be used as the debugging surface, as the test
// input format, and as the thing a model reads.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "il/il_test_support.h"
#include "xdec/il/function.h"
#include "xdec/il/parser.h"
#include "xdec/il/printer.h"

using namespace xdec::il;
using xdec::test::arm64Registers;

namespace {

RegId reg(const char* name) {
  const RegId id = arm64Registers().find(name);
  REQUIRE(id.valid());
  return id;
}

/// Prints, parses, prints again, and requires the two texts to be identical.
std::string requireRoundTrip(const Function& function) {
  const std::string first = print(function);
  auto parsed = parse(first, arm64Registers());
  if (!parsed) {
    FAIL("parse failed: " << parsed.error().format() << "\n--- text ---\n" << first);
  }
  const std::string second = print(**parsed);
  CHECK(second == first);
  return first;
}

/// Parses text, prints it, and requires the print to match the text. Used for
/// hand-written IL, which is how the interesting cases are written.
void requireCanonical(std::string_view text) {
  auto parsed = parse(text, arm64Registers());
  if (!parsed) {
    FAIL("parse failed: " << parsed.error().format() << "\n--- text ---\n" << text);
  }
  CHECK(print(**parsed) == text);
}

}  // namespace

TEST_CASE("a call with a result and SSA arguments round-trips", "[il][roundtrip]") {
  requireCanonical(R"(function @0x1000 name="" arch=arm64 maturity=ssa {
  block b0 @0x1000..0x1008 entry preds=[] {
    @0x1000
    %0 = call:i64 const:i64(0x2000)(entry:i64(x0), entry:i64(x1))
    ret val:i64(%0)
  }
}
)");
}

TEST_CASE("a lifted subtract with lazy flags round-trips", "[il][roundtrip]") {
  Function function{xdec::Arch::AArch64, arm64Registers(), 0x1000};
  function.setName("sub_flags");
  const BlockId entry = function.createBlock(0x1000);
  function.block(entry).endVa = 0x100C;

  // sub x2, x0, x1 / subs xzr, x0, x1 / b.eq
  const ValueId x0 = function.appendReadReg(entry, 0x1000, reg("x0"));
  const ValueId x1 = function.appendReadReg(entry, 0x1000, reg("x1"));
  const ExprId difference =
      function.binary(ExprOp::Sub, function.valueRef(x0), function.valueRef(x1));
  function.appendWriteReg(entry, 0x1000, reg("x2"), difference);

  const ExprId flagOperands[] = {function.valueRef(x0), function.valueRef(x1)};
  const ExprId flags = function.flagDef(FlagOp::Sub, 64, flagOperands);
  function.appendWriteReg(entry, 0x1004, reg("nzcv"), flags);

  const BlockId taken = function.createBlock(0x1010);
  const BlockId fallthrough = function.createBlock(0x100C);
  const ValueId nzcv = function.appendReadReg(entry, 0x1008, reg("nzcv"));
  function.appendCondBranch(entry, 0x1008,
                            function.flagCondition(function.valueRef(nzcv),
                                                   ConditionCode::Equal),
                            taken, fallthrough);
  function.appendReturn(taken, 0x1010);
  function.appendReturn(fallthrough, 0x100C);
  function.rebuildEdges();

  const std::string text = requireRoundTrip(function);
  // Spot-check that the laziness survives the trip rather than being expanded.
  CHECK(text.find("flagdef:sub.64") != std::string::npos);
  CHECK(text.find("flagcond:eq") != std::string::npos);
}

TEST_CASE("every op kind survives a round trip", "[il][roundtrip]") {
  Function function{xdec::Arch::AArch64, arm64Registers(), 0x2000};
  function.setName("all_ops");

  const BlockId entry = function.createBlock(0x2000);
  const BlockId second = function.createBlock(0x2020);
  const BlockId third = function.createBlock(0x2040);
  const BlockId exit = function.createBlock(0x2060);

  const ValueId base = function.appendReadReg(entry, 0x2000, reg("sp"));
  const ExprId address = function.binary(ExprOp::Add, function.valueRef(base),
                                         function.constant(Type::integer(64), 0x10));
  const ValueId loaded = function.appendLoad(entry, 0x2004, Type::integer(32), address);
  function.appendStore(entry, 0x2008, Type::integer(32), address,
                       function.constant(Type::integer(32), 0));
  function.appendWriteReg(entry, 0x200C, reg("w0"), function.valueRef(loaded));
  function.appendNop(entry, 0x2010);
  function.appendIntrinsic(entry, 0x2014, "aarch64.dmb", Type::voidType(), {});
  const ExprId intrinsicArguments[] = {function.valueRef(loaded)};
  function.appendIntrinsic(entry, 0x2018, "aarch64.crc32", Type::integer(32),
                           intrinsicArguments);
  function.appendCall(entry, 0x201C, function.constant(Type::integer(64), 0x3000));
  function.appendBranch(entry, 0x201C, second);

  // Every expression shape, so the printer's modifier handling is exercised.
  const ExprId wide = function.valueRef(base);
  const ExprId narrow = function.cast(ExprOp::Trunc, Type::integer(32), wide);
  const ExprId back = function.cast(ExprOp::ZExt, Type::integer(64), narrow);
  const ExprId signExtended = function.cast(ExprOp::SExt, Type::integer(64), narrow);
  const ExprId piece = function.extract(Type::integer(8), wide, 24);
  const ExprId joined = function.concat(Type::integer(64), narrow, narrow);
  const ExprId bits = function.unary(ExprOp::PopCount, wide);
  const ExprId compared = function.binary(ExprOp::CmpLtS, wide, back);
  const ExprId chosen = function.select(compared, back, signExtended);
  const ExprId undefinedValue = function.undefined(Type::integer(64));
  const ExprId logicalFlags = [&] {
    const ExprId operands[] = {wide};
    return function.flagDef(FlagOp::Logical, 64, operands);
  }();
  const ExprId carry = function.flagBitOf(logicalFlags, FlagBitIndex::Carry);
  const ExprId floatValue = function.cast(ExprOp::IntToFpS, Type::floating(64), wide);
  const ExprId floatSum = function.binary(ExprOp::FAdd, floatValue, floatValue);

  function.appendWriteReg(second, 0x2020, reg("x1"), joined);
  function.appendWriteReg(second, 0x2024, reg("x2"), chosen);
  function.appendWriteReg(second, 0x2028, reg("x3"), undefinedValue);
  function.appendWriteReg(second, 0x202C, reg("x4"),
                          function.cast(ExprOp::ZExt, Type::integer(64), piece));
  function.appendWriteReg(second, 0x2030, reg("x5"), bits);
  function.appendWriteReg(second, 0x2034, reg("nzcv"), logicalFlags);
  function.appendWriteReg(second, 0x2038, reg("x6"),
                          function.cast(ExprOp::ZExt, Type::integer(64), carry));
  const ExprId floatArguments[] = {floatSum};
  function.appendIntrinsic(second, 0x203C, "aarch64.fmov", Type::voidType(), floatArguments);
  const ValueId indirect = function.appendReadReg(second, 0x203C, reg("x7"));
  const OpId computed =
      function.appendIndirectBranch(second, 0x203C, function.valueRef(indirect));
  const BlockId resolvedTargets[] = {third, exit};
  function.setTargets(computed, resolvedTargets);

  // An op with no machine origin, produced by a later pass.
  function.setCurrentPass(function.internPass("deflatten"));
  function.appendUnreachable(third, kNoOpAddress);
  function.setCurrentPass(kPassLifter);

  function.appendReturn(exit, 0x2060);
  function.rebuildEdges();

  const std::string text = requireRoundTrip(function);
  CHECK(text.find("@none") != std::string::npos);
  CHECK(text.find("!from(deflatten)") != std::string::npos);
  CHECK(text.find("intrinsic \"aarch64.dmb\"()") != std::string::npos);
  CHECK(text.find("intrinsic:i32 \"aarch64.crc32\"") != std::string::npos);
}

TEST_CASE("an unresolved indirect branch round-trips as unresolved",
          "[il][roundtrip]") {
  Function function{xdec::Arch::AArch64, arm64Registers(), 0x1000};
  const BlockId entry = function.createBlock(0x1000);
  const ValueId target = function.appendReadReg(entry, 0x1000, reg("x0"));
  function.appendIndirectBranch(entry, 0x1004, function.valueRef(target));
  function.rebuildEdges();

  const std::string text = requireRoundTrip(function);
  CHECK(text.find("-> unresolved") != std::string::npos);
}

TEST_CASE("an annotation note round-trips, punctuation and all", "[il][roundtrip]") {
  // Notes are prose written by whichever pass learned something, so the format
  // has to survive the characters prose contains -- including the two the text
  // form itself uses: the quote that delimits the note and the semicolon that
  // starts a comment.
  Function function{xdec::Arch::AArch64, arm64Registers(), 0x1000};
  const BlockId entry = function.createBlock(0x1000);
  const ValueId target = function.appendReadReg(entry, 0x1000, reg("x0"));
  const OpId call = function.appendCall(entry, 0x1004, function.valueRef(target));
  function.appendReturn(entry, 0x1008);
  function.rebuildEdges();
  function.annotate(call, R"(target = load(v + i*0x8) ^ 0x4438; "encrypted")");

  const std::string text = requireRoundTrip(function);
  CHECK(text.find(R"(!note("target = load(v + i*0x8) ^ 0x4438; \"encrypted\""))") !=
        std::string::npos);

  auto parsed = parse(text, arm64Registers());
  REQUIRE(parsed.hasValue());
  CHECK((**parsed).noteOn(call) == R"(target = load(v + i*0x8) ^ 0x4438; "encrypted")");
}

TEST_CASE("a comment is stripped outside a quoted string but not inside one",
          "[il][roundtrip][parse]") {
  auto parsed = parse(
      "function @0x1000 name=\"f\" arch=arm64 maturity=lifted {\n"
      "  block b0 @0x1000..0x1004 entry preds=[] {\n"
      "    @0x1000\n"
      "    unimplemented \"weird ; mnemonic\"  ; ldr x0, [x1]\n"
      "  }\n"
      "}\n",
      arm64Registers());
  REQUIRE(parsed.hasValue());
  const Function& function = **parsed;
  const OpId opId = function.block(function.entryBlock()).ops.front();
  CHECK(function.nameOf(function.op(opId).payload) == "weird ; mnemonic");
}

TEST_CASE("an unimplemented instruction round-trips with its mnemonic",
          "[il][roundtrip]") {
  Function function{xdec::Arch::AArch64, arm64Registers(), 0x1000};
  const BlockId entry = function.createBlock(0x1000);
  function.appendUnimplemented(entry, 0x1000, "ld4 {v0.16b-v3.16b}, [x0]");
  function.rebuildEdges();

  const std::string first = print(function);
  auto parsed = parse(first, arm64Registers());
  REQUIRE(parsed.hasValue());
  CHECK(print(**parsed) == first);
  CHECK(first.find("unimplemented \"ld4 {v0.16b-v3.16b}, [x0]\"") != std::string::npos);
}

TEST_CASE("hand-written IL is canonical", "[il][roundtrip][parse]") {
  // This is the shape the tests for later phases will be written in, so the
  // exact text matters.
  requireCanonical(
      "function @0x1000 name=\"loop\" arch=arm64 maturity=cfg {\n"
      "  block b0 @0x1000..0x1008 entry preds=[] {\n"
      "    @0x1000\n"
      "    %0 = read x0\n"
      "    @0x1004\n"
      "    brc cmp.eq:i1(val:i64(%0), const:i64(0x0)), b2, b1\n"
      "  }\n"
      "  block b1 @0x1008..0x1010 preds=[b0, b1] {\n"
      "    @0x1008\n"
      "    %1 = read x1\n"
      "    write x1, add:i64(val:i64(%1), const:i64(0x1))\n"
      "    @0x100c\n"
      "    brc cmp.ltu:i1(val:i64(%1), const:i64(0xa)), b1, b2\n"
      "  }\n"
      "  block b2 @0x1010..0x1014 preds=[b0, b1] {\n"
      "    @0x1010\n"
      "    ret\n"
      "  }\n"
      "}\n");
}

TEST_CASE("phi operands referring to later blocks parse", "[il][roundtrip][parse]") {
  // The forward reference is the point: %2 is defined in b2, which the parser
  // has not read yet when it reaches the phi.
  requireCanonical(
      "function @0x1000 name=\"merge\" arch=arm64 maturity=ssa {\n"
      "  block b0 @0x1000..0x1004 entry preds=[] {\n"
      "    @0x1000\n"
      "    %0 = read x0\n"
      "    brc cmp.eq:i1(val:i64(%0), const:i64(0x0)), b1, b2\n"
      "  }\n"
      "  block b1 @0x1004..0x1008 preds=[b0] {\n"
      "    @0x1004\n"
      "    %1 = read x1\n"
      "    br b3\n"
      "  }\n"
      "  block b2 @0x1008..0x100c preds=[b0] {\n"
      "    @0x1008\n"
      "    %2 = read x2\n"
      "    br b3\n"
      "  }\n"
      "  block b3 @0x100c..0x1010 preds=[b1, b2] {\n"
      "    @0x100c\n"
      "    %3 = phi:i64(val:i64(%1), val:i64(%2))\n"
      "    write x0, val:i64(%3)\n"
      "    ret\n"
      "  }\n"
      "}\n");
}

TEST_CASE("comments and blank lines are ignored", "[il][roundtrip][parse]") {
  auto parsed = parse(
      "function @0x1000 name=\"commented\" arch=arm64 maturity=lifted {\n"
      "\n"
      "  block b0 @0x1000..0x1004 entry preds=[] {\n"
      "    @0x1000        ; sub x0, x0, #1\n"
      "    %0 = read x0   ; the disassembly rides along as a comment\n"
      "    write x0, sub:i64(val:i64(%0), const:i64(0x1))\n"
      "    ret\n"
      "  }\n"
      "}\n",
      arm64Registers());
  REQUIRE(parsed.hasValue());
  CHECK((*parsed)->blockCount() == 1);
  CHECK((*parsed)->name() == "commented");
  CHECK((*parsed)->maturity() == Maturity::Lifted);
}

TEST_CASE("the parser rejects rather than guesses", "[il][parse]") {
  const auto expectFailure = [](std::string_view text) {
    auto parsed = parse(text, arm64Registers());
    CHECK_FALSE(parsed.hasValue());
  };

  expectFailure("");
  expectFailure("block b0 {\n}\n");
  // An unknown register must not become a new storage location by accident.
  expectFailure(
      "function @0x1000 name=\"x\" arch=arm64 maturity=lifted {\n"
      "  block b0 @0x1000..0x1004 entry preds=[] {\n"
      "    %0 = read q7\n"
      "  }\n"
      "}\n");
  expectFailure(
      "function @0x1000 name=\"x\" arch=arm64 maturity=lifted {\n"
      "  block b0 @0x1000..0x1004 entry preds=[] {\n"
      "    frobnicate x0\n"
      "  }\n"
      "}\n");
  // A branch to a block that does not exist.
  expectFailure(
      "function @0x1000 name=\"x\" arch=arm64 maturity=lifted {\n"
      "  block b0 @0x1000..0x1004 entry preds=[] {\n"
      "    br b9\n"
      "  }\n"
      "}\n");
  // A value used before it is defined.
  expectFailure(
      "function @0x1000 name=\"x\" arch=arm64 maturity=lifted {\n"
      "  block b0 @0x1000..0x1004 entry preds=[] {\n"
      "    write x0, val:i64(%7)\n"
      "  }\n"
      "}\n");
  // A value-defining op with no result label.
  expectFailure(
      "function @0x1000 name=\"x\" arch=arm64 maturity=lifted {\n"
      "  block b0 @0x1000..0x1004 entry preds=[] {\n"
      "    read x0\n"
      "  }\n"
      "}\n");
  // Wrong operand count for the op.
  expectFailure(
      "function @0x1000 name=\"x\" arch=arm64 maturity=lifted {\n"
      "  block b0 @0x1000..0x1004 entry preds=[] {\n"
      "    write x0, add:i64(const:i64(0x1))\n"
      "  }\n"
      "}\n");
  // An unknown condition code.
  expectFailure(
      "function @0x1000 name=\"x\" arch=arm64 maturity=lifted {\n"
      "  block b0 @0x1000..0x1004 entry preds=[] {\n"
      "    %0 = read nzcv\n"
      "    write x0, flagcond:zz(val:flags(%0))\n"
      "  }\n"
      "}\n");
  // An unknown function attribute, rather than silently ignoring it.
  expectFailure("function @0x1000 colour=\"blue\" {\n}\n");
}

TEST_CASE("parse errors name the line", "[il][parse]") {
  auto parsed = parse(
      "function @0x1000 name=\"x\" arch=arm64 maturity=lifted {\n"
      "  block b0 @0x1000..0x1004 entry preds=[] {\n"
      "    %0 = read q7\n"
      "  }\n"
      "}\n",
      arm64Registers());
  REQUIRE_FALSE(parsed.hasValue());
  CHECK(parsed.error().code() == xdec::DiagCode::ParseError);
  const std::string message = parsed.error().format();
  CHECK(message.find("q7") != std::string::npos);
  CHECK(message.find("line 3") != std::string::npos);
}
