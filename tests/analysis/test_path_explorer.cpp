// PathExplorer: bounded, path-sensitive CFG exploration -- the third
// candidate source passes/resolve_indirect.cpp falls back to once
// analysis::ImageEval's table/value-set candidates come up empty. See
// path_explorer.h for what this adds and docs/23-path-eval.md for the full
// picture.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>

#include "il/il_test_support.h"
#include "xdec/analysis/path_explorer.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::ByteReader;
using xdec::analysis::PathExploreOptions;
using xdec::analysis::PathExplorer;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::Maturity;
using xdec::il::OpCode;
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
    "an indirect branch whose index is spilled to the stack on each arm and "
    "reloaded at the merge resolves via both explored paths",
    "[analysis][path-explorer]") {
  // entry forks on an unconstrained condition (an entry register with no
  // fact bound to it -- ImageEval and PathState alike see this as top, which
  // is exactly the case that forces PathExplorer to fork rather than settle
  // the branch statically). Each arm spills a different, path-specific index
  // to the same stack slot; the merge block reloads it and uses it as a
  // jump-table index. Neither analysis::ImageEval (no notion of a store) nor
  // a single-shot value set (the loaded index is not a phi at all, so there
  // is nothing for it to union) can answer this; PathExplorer answers it by
  // walking each arm concretely.
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
  const ExprId offset =
      function.binary(ExprOp::Mul, function.valueRef(index), function.constant(Type::integer(64), 8));
  const ExprId entryAddress = function.binary(ExprOp::Add, tableBase, offset);
  const il::ValueId target =
      function.appendLoad(merge, 0x1010, Type::integer(64), entryAddress);
  function.appendIndirectBranch(merge, 0x1014, function.valueRef(target));
  function.rebuildEdges();
  function.setMaturity(Maturity::Ssa);

  FakeImage image;
  image.qwords[0x8000] = 0x9000;
  image.qwords[0x8008] = 0x9100;
  // Readability probes on the targets themselves (see
  // PathExplorer::recordIndirectTarget's CachePointerDecoder fallback path).
  image.qwords[0x9000] = 0;
  image.qwords[0x9100] = 0;

  PathExplorer explorer(function, image.reader(), /*entryRegs=*/nullptr, PathExploreOptions{});
  explorer.explore();

  const std::vector<uint64_t>* candidates = explorer.targetsFor(0x1014);
  REQUIRE(candidates != nullptr);
  CHECK(std::find(candidates->begin(), candidates->end(), 0x9000u) != candidates->end());
  CHECK(std::find(candidates->begin(), candidates->end(), 0x9100u) != candidates->end());
}

TEST_CASE("explore() is a no-op when disabled", "[analysis][path-explorer]") {
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId entry = function.createBlock(0x1000);
  function.setEntryBlock(entry);
  function.appendIndirectBranch(entry, 0x1000,
                                function.entryReg(function.registers().find("x0")));
  function.rebuildEdges();

  FakeImage image;
  PathExploreOptions options;
  options.enabled = false;
  PathExplorer explorer(function, image.reader(), nullptr, options);
  explorer.explore();
  CHECK(explorer.targetsFor(0x1000) == nullptr);
  CHECK(explorer.pathsExplored() == 0);
}

TEST_CASE(
    "a stack-canary mismatch arm is not explored by default, and is when asked",
    "[analysis][path-explorer]") {
  // The exact save/check shape analysis/stack_canary.h recognises (see
  // test_stack_canary.cpp), with the mismatch arm shaped as a bare call --
  // `bl __stack_chk_fail(); unreachable;` -- and nothing this project can
  // name it by (see looksLikeBareCallBlock's own comment on why this is
  // structural, not symbol-based).
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId entry = function.createBlock(0x1000);
  function.setEntryBlock(entry);

  const ExprId sp = function.entryReg(function.registers().find("sp"));
  const ExprId slot = function.binary(ExprOp::Sub, sp, function.constant(Type::integer(64), 0x10));
  const ExprId guard = function.constant(Type::integer(64), 0x2000);
  const il::ValueId savedGuard = function.appendLoad(entry, 0x1000, Type::integer(64), guard);
  function.appendStore(entry, 0x1000, Type::integer(64), slot, function.valueRef(savedGuard));

  const BlockId ok = function.createBlock(0x1100);
  const BlockId fail = function.createBlock(0x1200);
  const il::ValueId reload = function.appendLoad(entry, 0x1004, Type::integer(64), guard);
  const il::ValueId saved = function.appendLoad(entry, 0x1008, Type::integer(64), slot);
  const ExprId condition =
      function.binary(ExprOp::CmpNe, function.valueRef(reload), function.valueRef(saved));
  function.appendCondBranch(entry, 0x100c, condition, fail, ok);

  function.appendCall(fail, 0x1200, function.constant(Type::integer(64), 0x5000));
  function.appendUnreachable(fail, 0x1204);

  function.appendReturn(ok, 0x1100);
  function.rebuildEdges();
  function.setMaturity(Maturity::Ssa);

  FakeImage image;
  image.qwords[0x2000] = 0x42;

  PathExploreOptions suppressed;
  PathExplorer explorerSuppressed(function, image.reader(), nullptr, suppressed);
  explorerSuppressed.explore();

  PathExploreOptions all;
  all.exploreCanaryFailPaths = true;
  PathExplorer explorerAll(function, image.reader(), nullptr, all);
  explorerAll.explore();

  // Suppressed never even steps into the mismatch arm; asked for both, it
  // does -- one more block visited.
  CHECK(explorerSuppressed.pathsExplored() < explorerAll.pathsExplored());
}
