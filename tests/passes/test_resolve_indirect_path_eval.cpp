// resolve-indirect's third candidate source: analysis::PathExplorer, tried
// only once tableCandidates and valueSetCandidates both come up empty (see
// resolve_indirect.cpp's pathEvalCandidates and docs/23-path-eval.md).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>

#include "il/il_test_support.h"
#include "xdec/analysis/path_explorer.h"
#include "xdec/il/function.h"
#include "xdec/il/verify.h"
#include "xdec/pass/pass.h"
#include "xdec/passes/resolve_indirect.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::ByteReader;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::Maturity;
using xdec::il::Type;

namespace {

struct FakeImage {
  std::map<uint64_t, uint64_t> qwords;

  [[nodiscard]] ByteReader reader() const {
    return [this](uint64_t va, std::span<std::byte> out) -> xdec::Result<void> {
      const auto found = qwords.find(va);
      if (found == qwords.end() || out.size() > 8) {
        return xdec::err(xdec::DiagCode::UnmappedAddress, "not in the fake image");
      }
      for (std::size_t index = 0; index < out.size(); ++index) {
        out[index] = static_cast<std::byte>(found->second >> (index * 8));
      }
      return {};
    };
  }
};

}  // namespace

TEST_CASE(
    "an index spilled to the stack per-arm and reloaded at the merge resolves only "
    "through the path-sensitive fallback",
    "[passes][resolve-indirect][path-eval]") {
  // Same shape as test_path_explorer.cpp's own case, wired through the whole
  // pass: neither tableCandidates (the index is an opaque Load, so it cannot
  // be bounded or shaped) nor valueSetCandidates (ImageEval has no notion of
  // this function's own Store) can answer it; PathExplorer can, because it
  // is the same in-flight walk that recorded the store in the first place.
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId entry = function.createBlock(0x1000);
  const BlockId armZero = function.createBlock(0x1004);
  const BlockId armOne = function.createBlock(0x1008);
  const BlockId merge = function.createBlock(0x100c);
  function.setEntryBlock(entry);

  const ExprId condition = function.binary(
      ExprOp::CmpNe, function.entryReg(function.registers().find("x0")),
      function.constant(Type::integer(64), 0));
  function.appendCondBranch(entry, 0x1000, condition, armZero, armOne);

  const ExprId slot = function.constant(Type::integer(64), 0x7000);
  function.appendStore(armZero, 0x1004, Type::integer(64), slot,
                       function.constant(Type::integer(64), 0));
  function.appendBranch(armZero, 0x1004, merge);

  function.appendStore(armOne, 0x1008, Type::integer(64), slot,
                       function.constant(Type::integer(64), 1));
  function.appendBranch(armOne, 0x1008, merge);

  const il::ValueId index = function.appendLoad(merge, 0x100c, Type::integer(64), slot);
  const ExprId tableBase = function.constant(Type::integer(64), 0x8000);
  const ExprId offset = function.binary(ExprOp::Mul, function.valueRef(index),
                                        function.constant(Type::integer(64), 8));
  const ExprId entryAddress = function.binary(ExprOp::Add, tableBase, offset);
  const il::ValueId target =
      function.appendLoad(merge, 0x1010, Type::integer(64), entryAddress);
  function.appendIndirectBranch(merge, 0x1014, function.valueRef(target));

  // The two candidate targets need to be real blocks: resolveOne only
  // commits a branch once every candidate lands on one (see its own
  // all-or-nothing comment), and a missing block is reported as a discovery
  // rather than resolved.
  const BlockId targetZero = function.createBlock(0x9000);
  function.appendReturn(targetZero, 0x9000);
  const BlockId targetOne = function.createBlock(0x9100);
  function.appendReturn(targetOne, 0x9100);

  function.rebuildEdges();
  function.setMaturity(Maturity::Ssa);

  FakeImage image;
  image.qwords[0x8000] = 0x9000;
  image.qwords[0x8008] = 0x9100;
  // Readability probes on the targets themselves (see
  // PathExplorer::recordIndirectTarget): an address the fake image has
  // nothing mapped at is filtered out, same as an unrelocated slot would be.
  image.qwords[0x9000] = 0;
  image.qwords[0x9100] = 0;

  const auto pass = xdec::passes::makeResolveIndirectPass();
  xdec::pass::Context context(function);
  context.setImage(image.reader());

  auto result = pass->run(context);
  const std::string error = result ? std::string{} : result.error().format();
  INFO(error);
  REQUIRE(result);
  CHECK(*result);

  const il::Op& terminator = function.op(function.block(merge).ops.back());
  REQUIRE(terminator.code == il::OpCode::IndirectBranch);
  const auto resolvedTargets = function.targets(terminator);
  REQUIRE(resolvedTargets.size() == 2);
  CHECK(std::find(resolvedTargets.begin(), resolvedTargets.end(), targetZero) !=
        resolvedTargets.end());
  CHECK(std::find(resolvedTargets.begin(), resolvedTargets.end(), targetOne) !=
        resolvedTargets.end());

  const il::VerifyReport report = il::verify(function, Maturity::Resolved);
  for (const xdec::Diag& diag : report.errors) {
    INFO(diag.format());
  }
  CHECK(report.ok());
}

TEST_CASE("disabling path-explore leaves the same branch unresolved",
          "[passes][resolve-indirect][path-eval]") {
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId entry = function.createBlock(0x1000);
  const BlockId armZero = function.createBlock(0x1004);
  const BlockId armOne = function.createBlock(0x1008);
  const BlockId merge = function.createBlock(0x100c);
  function.setEntryBlock(entry);

  const ExprId condition = function.binary(
      ExprOp::CmpNe, function.entryReg(function.registers().find("x0")),
      function.constant(Type::integer(64), 0));
  function.appendCondBranch(entry, 0x1000, condition, armZero, armOne);

  const ExprId slot = function.constant(Type::integer(64), 0x7000);
  function.appendStore(armZero, 0x1004, Type::integer(64), slot,
                       function.constant(Type::integer(64), 0));
  function.appendBranch(armZero, 0x1004, merge);
  function.appendStore(armOne, 0x1008, Type::integer(64), slot,
                       function.constant(Type::integer(64), 1));
  function.appendBranch(armOne, 0x1008, merge);

  const il::ValueId index = function.appendLoad(merge, 0x100c, Type::integer(64), slot);
  const ExprId tableBase = function.constant(Type::integer(64), 0x8000);
  const ExprId offset = function.binary(ExprOp::Mul, function.valueRef(index),
                                        function.constant(Type::integer(64), 8));
  const ExprId entryAddress = function.binary(ExprOp::Add, tableBase, offset);
  const il::ValueId target =
      function.appendLoad(merge, 0x1010, Type::integer(64), entryAddress);
  function.appendIndirectBranch(merge, 0x1014, function.valueRef(target));

  const BlockId targetZero = function.createBlock(0x9000);
  function.appendReturn(targetZero, 0x9000);
  const BlockId targetOne = function.createBlock(0x9100);
  function.appendReturn(targetOne, 0x9100);

  function.rebuildEdges();
  function.setMaturity(Maturity::Ssa);

  FakeImage image;
  image.qwords[0x8000] = 0x9000;
  image.qwords[0x8008] = 0x9100;

  const auto pass = xdec::passes::makeResolveIndirectPass();
  xdec::pass::Context context(function);
  context.setImage(image.reader());
  xdec::analysis::PathExploreOptions disabled;
  disabled.enabled = false;
  context.setPathExploreOptions(&disabled);

  auto result = pass->run(context);
  REQUIRE(result);
  CHECK_FALSE(*result);
  const il::Op& terminator = function.op(function.block(merge).ops.back());
  CHECK(terminator.code == il::OpCode::IndirectBranch);
  CHECK(function.targets(terminator).empty());
}
