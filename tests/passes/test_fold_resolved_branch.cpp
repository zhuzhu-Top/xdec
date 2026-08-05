// fold-resolved-branch: an IndirectBranch resolved to exactly one target
// becomes a plain Branch; one resolved to several is left alone.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include "il/il_test_support.h"
#include "xdec/il/function.h"
#include "xdec/il/verify.h"
#include "xdec/pass/pass.h"
#include "xdec/passes/fold_resolved_branch.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::il::BlockId;
using xdec::il::Function;
using xdec::il::Maturity;
using xdec::il::OpCode;
using xdec::il::Type;

namespace {

/// Runs the pass once against a function already at Resolved maturity, the
/// same state resolve-indirect leaves one in.
bool run(Function& function) {
  const std::unique_ptr<xdec::pass::Pass> pass = xdec::passes::makeFoldResolvedBranchPass();
  xdec::pass::Context context(function);
  auto result = pass->run(context);
  REQUIRE(result);
  return *result;
}

[[nodiscard]] bool verifiesCleanAt(const Function& function, Maturity level) {
  const il::VerifyReport report = il::verify(function, level);
  for (const xdec::Diag& diag : report.errors) {
    INFO(diag.format());
  }
  return report.ok();
}

}  // namespace

TEST_CASE("an indirect branch resolved to one target becomes an unconditional branch",
          "[passes][fold-resolved-branch]") {
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId entry = function.createBlock(0x1000);
  const BlockId target = function.createBlock(0x2000);
  function.setEntryBlock(entry);
  const il::ExprId addr = function.constant(Type::integer(64), 0x2000);
  const il::OpId brind = function.appendIndirectBranch(entry, 0x1000, addr);
  function.setTargets(brind, std::vector<BlockId>{target});
  function.appendReturn(target, 0x2000);
  function.rebuildEdges();
  function.setMaturity(Maturity::Resolved);

  CHECK(run(function));
  CHECK(function.op(brind).code == OpCode::Branch);
  CHECK(function.operands(function.op(brind)).empty());
  const auto targets = function.targets(function.op(brind));
  REQUIRE(targets.size() == 1);
  CHECK(targets[0] == target);
  CHECK(verifiesCleanAt(function, Maturity::Resolved));
}

TEST_CASE("an indirect branch resolved to several targets is left alone",
          "[passes][fold-resolved-branch]") {
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId entry = function.createBlock(0x1000);
  const BlockId first = function.createBlock(0x2000);
  const BlockId second = function.createBlock(0x3000);
  function.setEntryBlock(entry);
  const il::ExprId addr = function.entryReg(function.registers().find("x0"));
  const il::OpId brind = function.appendIndirectBranch(entry, 0x1000, addr);
  function.setTargets(brind, std::vector<BlockId>{first, second});
  function.appendReturn(first, 0x2000);
  function.appendReturn(second, 0x3000);
  function.rebuildEdges();
  function.setMaturity(Maturity::Resolved);

  CHECK_FALSE(run(function));
  CHECK(function.op(brind).code == OpCode::IndirectBranch);
  CHECK(verifiesCleanAt(function, Maturity::Resolved));
}

TEST_CASE("an indirect branch still unresolved is left alone",
          "[passes][fold-resolved-branch]") {
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId entry = function.createBlock(0x1000);
  function.setEntryBlock(entry);
  const il::ExprId addr = function.entryReg(function.registers().find("x0"));
  function.appendIndirectBranch(entry, 0x1000, addr);
  function.rebuildEdges();
  function.setMaturity(Maturity::Resolved);

  CHECK_FALSE(run(function));
}
