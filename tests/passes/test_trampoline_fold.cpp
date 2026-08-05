// trampoline-fold: retargeting edges around empty forwarding blocks.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "il/il_test_support.h"
#include "xdec/il/function.h"
#include "xdec/il/verify.h"
#include "xdec/pass/manager.h"
#include "xdec/pass/registry.h"
#include "xdec/passes/builtin.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::Maturity;
using xdec::il::Type;

namespace {

void runToCfg(Function& function) {
  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);
  xdec::pass::Manager manager;
  auto ran = manager.runTo(function, registry, Maturity::Cfg);
  const std::string error = ran ? std::string{} : ran.error().format();
  INFO(error);
  REQUIRE(ran);
}

[[nodiscard]] bool verifiesCleanAt(const Function& function, Maturity level) {
  const il::VerifyReport report = il::verify(function, level);
  for (const xdec::Diag& diag : report.errors) {
    INFO(diag.format());
  }
  return report.ok();
}

}  // namespace

TEST_CASE("a chain of empty forwarding blocks collapses to a direct edge",
          "[passes][trampoline-fold]") {
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId entry = function.createBlock(0x1000);
  const BlockId hopA = function.createBlock(0x2000);
  const BlockId hopB = function.createBlock(0x3000);
  const BlockId real = function.createBlock(0x4000);
  function.setEntryBlock(entry);
  function.appendBranch(entry, 0x1000, hopA);
  function.appendBranch(hopA, 0x2000, hopB);
  function.appendBranch(hopB, 0x3000, real);
  function.appendReturn(real, 0x4000);
  function.rebuildEdges();

  runToCfg(function);
  CHECK(verifiesCleanAt(function, Maturity::Cfg));

  CHECK(function.block(entry).successors == std::vector<BlockId>{real});
  // Both hops are still valid blocks (nothing removes a block), just no
  // longer reachable from anything.
  CHECK(function.block(hopA).predecessors.empty());
  CHECK(function.block(hopB).predecessors.empty());
}

TEST_CASE("a conditional branch's trampoline arm is redirected, its real arm left alone",
          "[passes][trampoline-fold]") {
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId entry = function.createBlock(0x1000);
  const BlockId hop = function.createBlock(0x2000);
  const BlockId real = function.createBlock(0x3000);
  const BlockId other = function.createBlock(0x4000);
  function.setEntryBlock(entry);
  const ExprId cond = function.binary(
      ExprOp::CmpNe, function.entryReg(function.registers().find("x0")),
      function.constant(Type::integer(64), 0));
  function.appendCondBranch(entry, 0x1000, cond, hop, other);
  function.appendBranch(hop, 0x2000, real);
  function.appendReturn(real, 0x3000);
  function.appendReturn(other, 0x4000);
  function.rebuildEdges();

  runToCfg(function);
  CHECK(verifiesCleanAt(function, Maturity::Cfg));

  CHECK(function.block(entry).successors == std::vector<BlockId>{real, other});
  CHECK(function.block(hop).predecessors.empty());
}

TEST_CASE("a trampoline that only ever branches to itself is left alone",
          "[passes][trampoline-fold]") {
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId entry = function.createBlock(0x1000);
  const BlockId spinner = function.createBlock(0x2000);
  function.setEntryBlock(entry);
  function.appendBranch(entry, 0x1000, spinner);
  function.appendBranch(spinner, 0x2000, spinner);
  function.rebuildEdges();

  // Must terminate (the cycle guard) and stay semantically faithful: nothing
  // downstream of `spinner` to skip to, so the edge into it is untouched.
  runToCfg(function);
  CHECK(verifiesCleanAt(function, Maturity::Cfg));
  CHECK(function.block(entry).successors == std::vector<BlockId>{spinner});
}
