// decompileToC (xdec/decompile/emit.h) end to end: the same call
// cmd_pipeline.cpp's `decompile` command makes, exercised here without a
// binary, a spawned CLI process, or an --out file -- just a hand-assembled
// program through the fixture (tests/fixture/pipeline_fixture.h) and an
// assertion on the printed C text. This is the safety net p0-decompile-to-c's
// extraction needed: a regression in the CLI-to-library move would show up
// here without needing a full binary or the eval/samples corpora.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../fixture/pipeline_fixture.h"

namespace il = xdec::il;
using xdec::emit::SymbolRef;
using xdec::testing::FlatProgram;

namespace {

/// `movz x<rd>, #imm16`.
[[nodiscard]] constexpr uint32_t movz64(unsigned rd, uint16_t imm) {
  return 0xd2800000U | (static_cast<uint32_t>(imm) << 5U) | rd;
}
/// `cbz x<rn>, #imm19` (as a word offset from this instruction).
[[nodiscard]] constexpr uint32_t cbz(unsigned rn, int32_t wordOffset) {
  return 0xb4000000U | (static_cast<uint32_t>(wordOffset) << 5U) | rn;
}
/// `b #imm26` (as a word offset from this instruction).
[[nodiscard]] constexpr uint32_t branch(int32_t wordOffset) {
  return 0x14000000U | (static_cast<uint32_t>(wordOffset) & 0x03ffffffU);
}
constexpr uint32_t kRet = 0xd65f03c0U;

TEST_CASE("decompileToC prints a straight-line function with no branches",
          "[decompile][integration]") {
  FlatProgram memory;
  memory.putInsn(0x1000, movz64(0, 0x2a));  // mov x0, #0x2a
  memory.putInsn(0x1004, kRet);

  auto result = xdec::testing::decompileToCFromBinary(memory.reader(), 0x1000);
  const std::string error = result ? std::string{} : result.error().format();
  INFO(error);
  REQUIRE(result);

  INFO(result->cSource);
  CHECK(result->driverReport.converged);
  CHECK(result->structured.labeled.empty());
  CHECK(result->cSource.find("goto") == std::string::npos);
  CHECK(result->cSource.find("sub_1000") != std::string::npos);
  CHECK(result->cSource.find("0x2a") != std::string::npos);
}

// The shape p3-struct-patterns' golden test will grow alongside: a guard on
// the first argument choosing between two returns, which the structurizer
// (see emit/structure.cpp) must render as `if`/`else` with no `goto` at all --
// exactly the property GCSF (docs/16-guard-cascade.md) exists to guarantee.
TEST_CASE("decompileToC structures a branch on the argument with no goto",
          "[decompile][integration]") {
  FlatProgram memory;
  memory.putInsn(0x1000, cbz(0, 3));         // cbz x0, #0x100c
  memory.putInsn(0x1004, movz64(0, 1));      // mov x0, #1
  memory.putInsn(0x1008, kRet);
  memory.putInsn(0x100c, movz64(0, 2));      // mov x0, #2
  memory.putInsn(0x1010, kRet);

  auto result = xdec::testing::decompileToCFromBinary(memory.reader(), 0x1000);
  const std::string error = result ? std::string{} : result.error().format();
  INFO(error);
  REQUIRE(result);

  CHECK(result->cSource.find("goto") == std::string::npos);
  CHECK(result->cSource.find("if") != std::string::npos);
  CHECK(result->cSource.find("arg1") != std::string::npos);
  CHECK(result->cSource.find("0x1") != std::string::npos);
  CHECK(result->cSource.find("0x2") != std::string::npos);
}

// The guard-cascade golden test itself (see the comment above the previous
// test): bc_lib's sub_2f9a38 in miniature -- an outer guard on arg1 and an
// inner guard on arg2 whose failure arms both fall into the same fallback
// block, which tryDiamond/tryOneSided cannot close (the fallback has two
// predecessors) but analysis::GuardCascadeShape/Structurizer::tryGuardCascade
// can (docs/16-guard-cascade.md). Checked at both ends of the pipeline: the
// printed C has no goto at all, and structuring's own record of what it did
// (StructuredFunction::matchedPatterns, p3-struct-patterns) names the exact
// pattern that closed it, so a future change that keeps the C goto-free by
// some other route (or regresses to a goto) shows up here either way.
TEST_CASE("decompileToC closes a guard cascade with no goto (bc_lib sub_2f9a38 shape)",
          "[decompile][integration]") {
  FlatProgram memory;
  memory.putInsn(0x1000, cbz(0, 2));      // outer: cbz x0, fallback (0x1008)
  memory.putInsn(0x1004, cbz(1, 3));      // inner: cbz x1, merge (0x1010)
  memory.putInsn(0x1008, movz64(0, 9));   // fallback: mov x0, #9
  memory.putInsn(0x100c, branch(1));      // fallback: b merge (0x1010)
  memory.putInsn(0x1010, kRet);           // merge: ret

  auto result = xdec::testing::decompileToCFromBinary(memory.reader(), 0x1000);
  const std::string error = result ? std::string{} : result.error().format();
  INFO(error);
  REQUIRE(result);

  INFO(result->cSource);
  CHECK(result->cSource.find("goto") == std::string::npos);
  CHECK(result->cSource.find("if") != std::string::npos);
  CHECK(result->cSource.find("0x9") != std::string::npos);
  CHECK(result->structured.matchedPatterns ==
        std::vector<std::string_view>{"guard-cascade"});
}

// decompileToCOptions.emit is read, not silently ignored: a caller's
// annotateBlocks/name/symbols choices (what cmd_pipeline.cpp wires from
// --no-annotate and the image's own symbol table) must reach the printed
// text the same way whether the caller is the CLI or this test.
TEST_CASE("decompileToC honours the caller's COptions", "[decompile][integration]") {
  FlatProgram memory;
  memory.putInsn(0x2000, movz64(0, 7));
  memory.putInsn(0x2004, kRet);

  xdec::decompile::DecompileToCOptions options;
  options.emit.annotateBlocks = true;
  options.emit.name = "renamed_fn";
  options.emit.symbols = [](uint64_t va) {
    SymbolRef ref;
    if (va == 0x2000) {
      ref.name = "not_used_since_name_wins";
      ref.isFunction = true;
    }
    return ref;
  };

  auto result = xdec::testing::decompileToCFromBinary(memory.reader(), 0x2000, options);
  const std::string error = result ? std::string{} : result.error().format();
  INFO(error);
  REQUIRE(result);

  CHECK(result->cSource.find("// b0 @0x2000") != std::string::npos);
  CHECK(result->cSource.find("renamed_fn") != std::string::npos);
}

}  // namespace
