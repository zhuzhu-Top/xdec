// fold-resolved-branch: an IndirectBranch resolved to exactly one target
// becomes a plain Branch; one resolved to several is left alone.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

//   entry: t = load [tableBase]; brind t -> target
TEST_CASE("folding a resolved branch removes the now-unused jump-table load",
          "[passes][fold-resolved-branch]") {
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId entry = function.createBlock(0x1000);
  const BlockId target = function.createBlock(0x2000);
  function.setEntryBlock(entry);
  const il::ValueId tableLoad =
      function.appendLoad(entry, 0x1000, Type::integer(64), function.constant(Type::integer(64), 0x30c000));
  const il::OpId loadOp = function.value(tableLoad).definition;
  const il::OpId brind = function.appendIndirectBranch(entry, 0x1004, function.valueRef(tableLoad));
  function.setTargets(brind, std::vector<BlockId>{target});
  function.appendReturn(target, 0x2000);
  function.rebuildEdges();
  function.setMaturity(Maturity::Resolved);

  CHECK(run(function));
  CHECK(function.op(brind).code == OpCode::Branch);
  const auto& entryOps = function.block(entry).ops;
  CHECK(std::find(entryOps.begin(), entryOps.end(), loadOp) == entryOps.end());
  CHECK(verifiesCleanAt(function, Maturity::Resolved));
}

//   entry: base = load [tableBase]; t = load [base + 8]; brind t -> target
//
// A two-level dispatch: the secondary table load reads through the primary
// one's result, so removing it is what makes the primary load unused too.
// Both must go, one round after the other.
TEST_CASE("folding a resolved branch removes a two-level chain of jump-table loads",
          "[passes][fold-resolved-branch]") {
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId entry = function.createBlock(0x1000);
  const BlockId target = function.createBlock(0x2000);
  function.setEntryBlock(entry);
  const il::ValueId baseLoad =
      function.appendLoad(entry, 0x1000, Type::integer(64), function.constant(Type::integer(64), 0x30c000));
  const il::OpId baseLoadOp = function.value(baseLoad).definition;
  const il::ExprId secondaryAddr = function.binary(il::ExprOp::Add, function.valueRef(baseLoad),
                                                    function.constant(Type::integer(64), 8));
  const il::ValueId secondaryLoad = function.appendLoad(entry, 0x1004, Type::integer(64), secondaryAddr);
  const il::OpId secondaryLoadOp = function.value(secondaryLoad).definition;
  const il::OpId brind =
      function.appendIndirectBranch(entry, 0x1008, function.valueRef(secondaryLoad));
  function.setTargets(brind, std::vector<BlockId>{target});
  function.appendReturn(target, 0x2000);
  function.rebuildEdges();
  function.setMaturity(Maturity::Resolved);

  CHECK(run(function));
  CHECK(function.op(brind).code == OpCode::Branch);
  const auto& entryOps = function.block(entry).ops;
  CHECK(std::find(entryOps.begin(), entryOps.end(), baseLoadOp) == entryOps.end());
  CHECK(std::find(entryOps.begin(), entryOps.end(), secondaryLoadOp) == entryOps.end());
  CHECK(verifiesCleanAt(function, Maturity::Resolved));
}

//   entry: t = load [tableBase]; store t -> [0x9000]; brind t -> target
TEST_CASE("folding a resolved branch keeps a load whose value is read elsewhere",
          "[passes][fold-resolved-branch]") {
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId entry = function.createBlock(0x1000);
  const BlockId target = function.createBlock(0x2000);
  function.setEntryBlock(entry);
  const il::ValueId tableLoad =
      function.appendLoad(entry, 0x1000, Type::integer(64), function.constant(Type::integer(64), 0x30c000));
  const il::OpId loadOp = function.value(tableLoad).definition;
  function.appendStore(entry, 0x1004, Type::integer(64), function.constant(Type::integer(64), 0x9000),
                       function.valueRef(tableLoad));
  const il::OpId brind = function.appendIndirectBranch(entry, 0x1008, function.valueRef(tableLoad));
  function.setTargets(brind, std::vector<BlockId>{target});
  function.appendReturn(target, 0x2000);
  function.rebuildEdges();
  function.setMaturity(Maturity::Resolved);

  CHECK(run(function));
  CHECK(function.op(brind).code == OpCode::Branch);
  const auto& entryOps = function.block(entry).ops;
  CHECK(std::find(entryOps.begin(), entryOps.end(), loadOp) != entryOps.end());
  CHECK(verifiesCleanAt(function, Maturity::Resolved));
}
