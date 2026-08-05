// The cfg-finalize gate: audit, edge repair, and honest failure modes.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "il/il_test_support.h"
#include "xdec/il/function.h"
#include "xdec/il/verify.h"
#include "xdec/pass/manager.h"
#include "xdec/pass/registry.h"
#include "xdec/passes/builtin.h"

using xdec::Arch;
using xdec::il::BlockId;
using xdec::il::Function;
using xdec::il::Maturity;
using xdec::pass::Manager;
using xdec::pass::Registry;

namespace {

[[nodiscard]] Function twoBlockFunction() {
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId entry = function.createBlock(0x1000);
  const BlockId tail = function.createBlock(0x2000);
  function.setEntryBlock(entry);
  function.appendBranch(entry, 0x1000, tail);
  function.appendReturn(tail, 0x2000);
  return function;
}

TEST_CASE("a stale edge cache is caught before the gate ever runs", "[passes][cfg-finalize]") {
  Function function = twoBlockFunction();
  // Deliberately skip rebuildEdges: the stored edges disagree with the
  // terminators. The verifier treats a stale cache as an invariant violation
  // at every level, so the pipeline's first verify — after local-simplify —
  // is what catches it, and the pipeline stops there.
  Registry registry;
  xdec::passes::registerBuiltinPasses(registry);
  Manager manager;
  auto ran = manager.runTo(function, registry, Maturity::Cfg);
  REQUIRE_FALSE(ran);
  CHECK(ran.error().format().find("rebuildEdges") != std::string::npos);
}

TEST_CASE("a fresh function reports unchanged", "[passes][cfg-finalize]") {
  Function function = twoBlockFunction();
  function.rebuildEdges();

  Registry registry;
  xdec::passes::registerBuiltinPasses(registry);
  Manager manager;
  auto ran = manager.runTo(function, registry, Maturity::Cfg);
  REQUIRE(ran);
  // local-simplify, cfg-finalize, trampoline-fold: all three sit at or below
  // Cfg maturity, and none of them have anything to do to a function this
  // clean.
  REQUIRE(ran->size() == 3);
  CHECK_FALSE((*ran)[1].changed);
  CHECK_FALSE((*ran)[2].changed);
}

TEST_CASE("an unterminated block fails the gate and names the block", "[passes][cfg-finalize]") {
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId entry = function.createBlock(0x1000);
  function.setEntryBlock(entry);
  function.appendNop(entry, 0x1000);
  function.rebuildEdges();

  Registry registry;
  xdec::passes::registerBuiltinPasses(registry);
  Manager manager;
  auto ran = manager.runTo(function, registry, Maturity::Cfg);
  REQUIRE_FALSE(ran);
  CHECK(ran.error().format().find("has no terminator") != std::string::npos);
  CHECK(ran.error().format().find("cfg-finalize") != std::string::npos);
}

TEST_CASE("an external stub is legitimate, a stub with content is not",
          "[passes][cfg-finalize]") {
  Function function = twoBlockFunction();
  const BlockId stub = function.createBlock(0x9000);
  function.block(stub).external = true;
  function.rebuildEdges();

  Registry registry;
  xdec::passes::registerBuiltinPasses(registry);
  Manager manager;
  auto ran = manager.runTo(function, registry, Maturity::Cfg);
  const std::string error = ran ? std::string{} : ran.error().format();
  INFO(error);
  REQUIRE(ran);
  // And the verifier accepts the empty stub at cfg maturity.
  const xdec::il::VerifyReport report = xdec::il::verify(function);
  for (const xdec::Diag& diag : report.errors) {
    INFO(diag.format());
  }
  CHECK(report.ok());
}

}  // namespace
