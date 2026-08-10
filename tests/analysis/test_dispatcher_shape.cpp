// matchDispatcherShape: the flattening dispatcher's shared tail, and honest
// misses when the shape does not hold.
#include <catch2/catch_test_macros.hpp>

#include "il/il_test_support.h"
#include "xdec/analysis/dispatcher_shape.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::DispatcherShape;
using xdec::analysis::matchDispatcherShape;
using xdec::il::BlockId;
using xdec::il::Function;
using xdec::il::Type;

namespace {

struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {
    entry = function.createBlock(0x1000);
    function.setEntryBlock(entry);
  }

  BlockId block(uint64_t va) { return function.createBlock(va); }

  /// A resolved indirect branch from `entry` to every block in `targets`, the
  /// way a switch's dispatch block reaches its handlers -- and, crucially,
  /// the only thing that makes each of them `entry`'s sole predecessor.
  void dispatchTo(const std::vector<BlockId>& targets) {
    const il::OpId brind =
        function.appendIndirectBranch(entry, 0x1000, function.undefined(Type::integer(64)));
    function.setTargets(brind, targets);
  }

  Function function;
  BlockId entry;
};

}  // namespace

TEST_CASE("three private handlers falling into one shared tail match the shape",
          "[analysis][dispatcher-shape]") {
  Fixture f;
  const BlockId h1 = f.block(0x2000);
  const BlockId h2 = f.block(0x2100);
  const BlockId h3 = f.block(0x2200);
  const BlockId merge = f.block(0x3000);
  const BlockId hub = f.block(0x4000);
  f.dispatchTo({h1, h2, h3});
  f.function.appendBranch(h1, 0x2000, merge);
  f.function.appendBranch(h2, 0x2100, merge);
  f.function.appendBranch(h3, 0x2200, merge);
  f.function.appendBranch(merge, 0x3000, hub);
  f.function.appendReturn(hub, 0x4000);
  f.function.rebuildEdges();

  const std::vector<BlockId> targets{h1, h2, h3};
  const auto shape = matchDispatcherShape(f.function, f.entry, targets);
  REQUIRE(shape.has_value());
  CHECK(shape->dispatch == f.entry);
  CHECK(shape->merge == merge);
  CHECK(shape->hub == hub);
}

TEST_CASE("fewer than three targets is never worth a shared-tail search",
          "[analysis][dispatcher-shape]") {
  Fixture f;
  const BlockId h1 = f.block(0x2000);
  const BlockId h2 = f.block(0x2100);
  const BlockId merge = f.block(0x3000);
  f.dispatchTo({h1, h2});
  f.function.appendBranch(h1, 0x2000, merge);
  f.function.appendBranch(h2, 0x2100, merge);
  f.function.appendReturn(merge, 0x3000);
  f.function.rebuildEdges();

  const std::vector<BlockId> targets{h1, h2};
  CHECK_FALSE(matchDispatcherShape(f.function, f.entry, targets).has_value());
}

TEST_CASE("a lone return handler among a clear majority still lets the rest match",
          "[analysis][dispatcher-shape]") {
  // The real shape this models (0x2a2428): 161 of 162 handlers merge, one
  // returns outright. A return handler has no successor to vote with, so it
  // simply does not count towards -- or against -- the merge's support.
  Fixture f;
  const BlockId h1 = f.block(0x2000);
  const BlockId h2 = f.block(0x2100);
  const BlockId h3 = f.block(0x2200);
  const BlockId h4 = f.block(0x2300);
  const BlockId ret = f.block(0x2400);
  const BlockId merge = f.block(0x3000);
  const BlockId hub = f.block(0x4000);
  f.dispatchTo({h1, h2, h3, h4, ret});
  f.function.appendBranch(h1, 0x2000, merge);
  f.function.appendBranch(h2, 0x2100, merge);
  f.function.appendBranch(h3, 0x2200, merge);
  f.function.appendBranch(h4, 0x2300, merge);
  f.function.appendReturn(ret, 0x2400);
  f.function.appendBranch(merge, 0x3000, hub);
  f.function.appendReturn(hub, 0x4000);
  f.function.rebuildEdges();

  const std::vector<BlockId> targets{h1, h2, h3, h4, ret};
  const auto shape = matchDispatcherShape(f.function, f.entry, targets);
  REQUIRE(shape.has_value());
  CHECK(shape->merge == merge);
  CHECK(shape->hub == hub);
}

TEST_CASE("a merge candidate below the support threshold is not a shared tail",
          "[analysis][dispatcher-shape]") {
  // Five handlers split their votes 3-2 between two candidate tails: neither
  // clears the bar, so this is not a dispatcher's shared epilogue, just a
  // coincidence of a few blocks happening to jump to the same place.
  Fixture f;
  const BlockId h1 = f.block(0x2000);
  const BlockId h2 = f.block(0x2100);
  const BlockId h3 = f.block(0x2200);
  const BlockId h4 = f.block(0x2300);
  const BlockId h5 = f.block(0x2400);
  const BlockId mergeA = f.block(0x3000);
  const BlockId mergeB = f.block(0x3100);
  f.dispatchTo({h1, h2, h3, h4, h5});
  f.function.appendBranch(h1, 0x2000, mergeA);
  f.function.appendBranch(h2, 0x2100, mergeA);
  f.function.appendBranch(h3, 0x2200, mergeA);
  f.function.appendBranch(h4, 0x2300, mergeB);
  f.function.appendBranch(h5, 0x2400, mergeB);
  f.function.appendReturn(mergeA, 0x3000);
  f.function.appendReturn(mergeB, 0x3100);
  f.function.rebuildEdges();

  const std::vector<BlockId> targets{h1, h2, h3, h4, h5};
  CHECK_FALSE(matchDispatcherShape(f.function, f.entry, targets).has_value());
}

TEST_CASE("a candidate tail that itself branches is not a plain shared jump",
          "[analysis][dispatcher-shape]") {
  // The tail has to be one unconditional jump back to the hub -- a block
  // that still tests something of its own is not an epilogue, it is more of
  // the state machine, and printing it once ahead of a switch that can reach
  // it through several different states would be wrong.
  Fixture f;
  const BlockId h1 = f.block(0x2000);
  const BlockId h2 = f.block(0x2100);
  const BlockId h3 = f.block(0x2200);
  const BlockId merge = f.block(0x3000);
  const BlockId hubA = f.block(0x4000);
  const BlockId hubB = f.block(0x4100);
  f.dispatchTo({h1, h2, h3});
  f.function.appendBranch(h1, 0x2000, merge);
  f.function.appendBranch(h2, 0x2100, merge);
  f.function.appendBranch(h3, 0x2200, merge);
  const il::ExprId cond = f.function.entryReg(f.function.registers().find("x0"));
  f.function.appendCondBranch(merge, 0x3000, cond, hubA, hubB);
  f.function.appendReturn(hubA, 0x4000);
  f.function.appendReturn(hubB, 0x4100);
  f.function.rebuildEdges();

  const std::vector<BlockId> targets{h1, h2, h3};
  CHECK_FALSE(matchDispatcherShape(f.function, f.entry, targets).has_value());
}
