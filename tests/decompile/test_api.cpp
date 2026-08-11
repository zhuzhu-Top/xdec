// xdec/api.h: the thin public re-export (see its own doc comment for why it
// is thin). What this locks down is exactly that thinness -- xdec::api's
// aliases must be the same types and the same function xdec::decompile
// already exposes, not a parallel copy that could quietly drift -- plus the
// one thing api.h's own header comment promises that emit.h's tests do not
// already cover: DecompileReport actually reaching a caller that only
// knows the top-level names.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <type_traits>

#include "../fixture/pipeline_fixture.h"
#include "xdec/api.h"

using xdec::testing::FlatProgram;

namespace {

[[nodiscard]] constexpr uint32_t movz64(unsigned rd, uint16_t imm) {
  return 0xd2800000U | (static_cast<uint32_t>(imm) << 5U) | rd;
}
constexpr uint32_t kRet = 0xd65f03c0U;

// Same aliasing contract for the two structs api.h exports as functions:
// checked at compile time (a mismatch here is a build failure, not a test
// failure), rather than a runtime CHECK that could not tell "different type"
// from "coincidentally equal contents" apart.
static_assert(std::is_same_v<xdec::DecompileOptions, xdec::decompile::DecompileToCOptions>);
static_assert(std::is_same_v<xdec::DecompileResult, xdec::decompile::DecompileToCResult>);
static_assert(std::is_same_v<xdec::DecompileReport, xdec::decompile::DecompileReport>);

TEST_CASE("xdec::decompileToC is xdec::decompile::decompileToC, callable through the alias",
          "[decompile][integration][api]") {
  FlatProgram memory;
  memory.putInsn(0x1000, movz64(0, 5));
  memory.putInsn(0x1004, kRet);

  xdec::DecompileOptions options;
  auto result = xdec::testing::decompileToCFromBinary(memory.reader(), 0x1000, options);
  const std::string error = result ? std::string{} : result.error().format();
  INFO(error);
  REQUIRE(result);

  // decltype-checked against the alias rather than assigned to a named
  // `xdec::DecompileResult` local: the point is that *result's type and the
  // alias are the same type (the earlier static_asserts already establish
  // that for the struct names), not that a second local can also hold it.
  static_assert(std::is_same_v<decltype(*result), xdec::DecompileResult&>);
  CHECK(result->cSource.find("0x5") != std::string::npos);
}

TEST_CASE("decompileToC's report reaches a caller through the xdec::DecompileReport alias",
          "[decompile][integration][api]") {
  FlatProgram memory;
  memory.putInsn(0x1000, movz64(0, 1));
  memory.putInsn(0x1004, kRet);

  xdec::DecompileOptions options;
  options.computeEmitRedundancy = true;
  auto result = xdec::testing::decompileToCFromBinary(memory.reader(), 0x1000, options);
  const std::string error = result ? std::string{} : result.error().format();
  INFO(error);
  REQUIRE(result);

  const xdec::DecompileReport& report = result->report;
  CHECK_FALSE(report.stageTimings.empty());
  CHECK(report.labeledBlockCount == 0);
  REQUIRE(report.emitRedundancy.has_value());
}

}  // namespace
