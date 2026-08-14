// findDispatchRegions / matchDispatchClamp: clustering computed-branch
// dispatch sites by the physical table (and clamp) they share, and the
// region-level shared-tail vote across many small two-way sites that no
// single site's own target list is big enough to vote with.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

#include "il/il_test_support.h"
#include "xdec/analysis/dispatch_region.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::confirmDispatcherShapeFromRegion;
using xdec::analysis::DispatchRegion;
using xdec::analysis::DispatchRegionTail;
using xdec::analysis::findDispatchJoins;
using xdec::analysis::findDispatchRegions;
using xdec::analysis::matchDispatchClamp;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::Type;

namespace {

struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {
    entry = function.createBlock(0x1000);
    function.setEntryBlock(entry);
  }

  ExprId i64(uint64_t value) { return function.constant(Type::integer(64), value); }
  ExprId entryReg(std::string_view name) {
    return function.entryReg(function.registers().find(name));
  }
  BlockId block(uint64_t va) { return function.createBlock(va); }

  /// `bound < inner ? replacement : inner` -- the clamp shape
  /// matchDispatchClamp recognises. `inner` is one site's own boolean split;
  /// `bound`/`replacement` are what every site reading the same physical
  /// table shares.
  ExprId clampedIndex(ExprId inner, uint64_t bound, uint64_t replacement) {
    const ExprId cond = function.binary(ExprOp::CmpLtS, i64(bound), inner);
    return function.select(cond, i64(replacement), inner);
  }

  /// An offset-table dispatch site at `dispatch`: `brind anchor +
  /// sext(load32(tableBase + (index << 2)))`, resolved to `targets`. The
  /// anchor is the site's own, which is what makes this shape worth its own
  /// helper: two sites reading one table rarely share one.
  void offsetDispatchSite(BlockId dispatch, uint64_t tableBase, uint64_t anchor, ExprId index,
                          const std::vector<BlockId>& targets) {
    const uint64_t va = function.block(dispatch).va;
    const ExprId address =
        function.binary(ExprOp::Add, i64(tableBase), function.binary(ExprOp::Shl, index, i64(2)));
    const ExprId loaded =
        function.valueRef(function.appendLoad(dispatch, va, Type::integer(32), address));
    const ExprId target = function.binary(ExprOp::Add, i64(anchor),
                                          function.cast(ExprOp::SExt, Type::integer(64), loaded));
    const il::OpId brind = function.appendIndirectBranch(dispatch, va, target);
    function.setTargets(brind, targets);
  }

  /// A pointer-table dispatch site at `dispatch`: `brind load(tableBase +
  /// (index << 3))`, resolved to `targets`.
  void dispatchSite(BlockId dispatch, uint64_t tableBase, ExprId index,
                    const std::vector<BlockId>& targets) {
    const uint64_t va = function.block(dispatch).va;
    const ExprId address =
        function.binary(ExprOp::Add, i64(tableBase), function.binary(ExprOp::Shl, index, i64(3)));
    const ExprId loaded =
        function.valueRef(function.appendLoad(dispatch, va, Type::integer(64), address));
    const il::OpId brind = function.appendIndirectBranch(dispatch, va, loaded);
    function.setTargets(brind, targets);
  }

  Function function;
  BlockId entry;
};

}  // namespace

TEST_CASE("matchDispatchClamp recognises bound < index ? replacement : index",
          "[analysis][dispatch-region]") {
  Fixture f;
  const ExprId index = f.entryReg("x0");
  const ExprId cond = f.function.binary(ExprOp::CmpLtS, f.i64(0x2cc), index);
  const ExprId select = f.function.select(cond, f.i64(0x213), index);

  const auto clamp = matchDispatchClamp(f.function, select);
  REQUIRE(clamp.has_value());
  CHECK(clamp->index == index);
  CHECK(clamp->bound == 0x2cc);
  CHECK(clamp->replacement == 0x213);
  CHECK(clamp->isSigned);
}

TEST_CASE("matchDispatchClamp tells an unsigned clamp from a signed one",
          "[analysis][dispatch-region]") {
  Fixture f;
  const ExprId index = f.entryReg("x0");
  const ExprId cond = f.function.binary(ExprOp::CmpLtU, f.i64(0x2cc), index);
  const ExprId select = f.function.select(cond, f.i64(0x213), index);

  const auto clamp = matchDispatchClamp(f.function, select);
  REQUIRE(clamp.has_value());
  CHECK_FALSE(clamp->isSigned);
}

TEST_CASE("matchDispatchClamp is honest about a select that is not this shape",
          "[analysis][dispatch-region]") {
  Fixture f;
  const ExprId index = f.entryReg("x0");
  // The arms are swapped from the clamp's own convention -- `index < bound ?
  // index : replacement` is a plain min, not an out-of-range clamp, and must
  // not be mistaken for one just because it is also a Select over a compare.
  const ExprId cond = f.function.binary(ExprOp::CmpLtS, index, f.i64(0x2cc));
  const ExprId select = f.function.select(cond, index, f.i64(0x213));
  CHECK_FALSE(matchDispatchClamp(f.function, select).has_value());
}

TEST_CASE("matchDispatchClamp requires a literal bound and replacement",
          "[analysis][dispatch-region]") {
  Fixture f;
  const ExprId index = f.entryReg("x0");
  const ExprId computedBound = f.entryReg("x1");
  const ExprId cond = f.function.binary(ExprOp::CmpLtS, computedBound, index);
  const ExprId select = f.function.select(cond, f.i64(0x213), index);
  // A computed bound only bounds this one branch -- it is not evidence any
  // other site reads the same table the same way.
  CHECK_FALSE(matchDispatchClamp(f.function, select).has_value());
}

TEST_CASE("three unrelated two-way sites reading the same table and clamp form one region",
          "[analysis][dispatch-region]") {
  Fixture f;
  const BlockId d1 = f.block(0x2000);
  const BlockId d2 = f.block(0x2100);
  const BlockId d3 = f.block(0x2200);
  const BlockId h1a = f.block(0x3000);
  const BlockId h1b = f.block(0x3010);
  const BlockId h2a = f.block(0x3100);
  const BlockId h2b = f.block(0x3110);
  const BlockId h3a = f.block(0x3200);
  const BlockId h3b = f.block(0x3210);

  const ExprId b1 = f.function.select(f.function.binary(ExprOp::CmpNe, f.entryReg("x0"), f.i64(0)),
                                      f.i64(0x2a2), f.i64(0xb6));
  const ExprId b2 = f.function.select(f.function.binary(ExprOp::CmpNe, f.entryReg("x1"), f.i64(0)),
                                      f.i64(0x69), f.i64(0x1cf));
  const ExprId b3 = f.function.select(f.function.binary(ExprOp::CmpNe, f.entryReg("x2"), f.i64(0)),
                                      f.i64(0x213), f.i64(0x1));

  f.dispatchSite(d1, 0x1e70a0, f.clampedIndex(b1, 0x2cc, 0x213), {h1a, h1b});
  f.dispatchSite(d2, 0x1e70a0, f.clampedIndex(b2, 0x2cc, 0x213), {h2a, h2b});
  f.dispatchSite(d3, 0x1e70a0, f.clampedIndex(b3, 0x2cc, 0x213), {h3a, h3b});
  f.function.rebuildEdges();

  const auto regions = findDispatchRegions(f.function);
  REQUIRE(regions.size() == 1);
  const DispatchRegion& region = regions.front();
  CHECK(region.tableBase == 0x1e70a0);
  CHECK(region.tableStride == 8);
  REQUIRE(region.clampBound.has_value());
  REQUIRE(region.clampReplacement.has_value());
  CHECK(*region.clampBound == 0x2cc);
  CHECK(*region.clampReplacement == 0x213);
  REQUIRE(region.sites.size() == 3);
  // Nothing here funnels the region's handlers into a common tail -- each
  // pair goes its own way -- so the honest answer is no shared tail, not a
  // guessed one.
  CHECK_FALSE(region.sharedTail.has_value());

  const auto& site = region.sites.front();
  CHECK(site.dispatchBlock == d1);
  REQUIRE(site.caseValues.size() == 2);
  CHECK(site.caseValues[0] == 0xb6);
  CHECK(site.caseValues[1] == 0x2a2);
}

TEST_CASE("offset-table sites anchored at themselves are still one region",
          "[analysis][dispatch-region]") {
  // The shape a scatter dispatcher takes when its table holds offsets rather
  // than pointers: every site adds the entry to its own address, so no two
  // anchors agree. Filing each one as a region of its own would be reading
  // "where this site sits" as "which table it reads", and would leave the
  // pooled evidence findDispatchJoins works from with a single site per pool
  // -- nothing to pool.
  Fixture f;
  const BlockId d1 = f.block(0x2000);
  const BlockId d2 = f.block(0x2100);
  const BlockId h1a = f.block(0x3000);
  const BlockId h1b = f.block(0x3010);
  const BlockId h2a = f.block(0x3100);
  const BlockId h2b = f.block(0x3110);

  f.offsetDispatchSite(d1, 0x1e70a0, 0x2000, f.entryReg("x0"), {h1a, h1b});
  f.offsetDispatchSite(d2, 0x1e70a0, 0x2100, f.entryReg("x1"), {h2a, h2b});
  f.function.rebuildEdges();

  const auto regions = findDispatchRegions(f.function);
  REQUIRE(regions.size() == 1);
  CHECK(regions.front().tableBase == 0x1e70a0);
  CHECK(regions.front().sites.size() == 2);
}

TEST_CASE("sites reading different tables stay different regions",
          "[analysis][dispatch-region]") {
  // The other half of the rule above: dropping the anchor from the region's
  // identity must not go so far as to pool sites that read different memory.
  Fixture f;
  const BlockId d1 = f.block(0x2000);
  const BlockId d2 = f.block(0x2100);
  const BlockId h1a = f.block(0x3000);
  const BlockId h1b = f.block(0x3010);
  const BlockId h2a = f.block(0x3100);
  const BlockId h2b = f.block(0x3110);

  f.offsetDispatchSite(d1, 0x1e70a0, 0x2000, f.entryReg("x0"), {h1a, h1b});
  f.offsetDispatchSite(d2, 0x1e8000, 0x2100, f.entryReg("x1"), {h2a, h2b});
  f.function.rebuildEdges();

  CHECK(findDispatchRegions(f.function).size() == 2);
}

TEST_CASE("a region's pooled targets can still vote a shared tail across many small sites",
          "[analysis][dispatch-region]") {
  // The generalisation of matchDispatcherShape's own vote: no single site
  // here has more than two targets, but the six pooled together still
  // converge on one shared epilogue -- the shape a single-hub dispatcher
  // would leave if it had been split into several 2-way branches instead of
  // one N-way one.
  Fixture f;
  const BlockId d1 = f.block(0x2000);
  const BlockId d2 = f.block(0x2100);
  const BlockId d3 = f.block(0x2200);
  std::vector<BlockId> handlers;
  for (int i = 0; i < 6; ++i) {
    handlers.push_back(f.block(0x3000 + 0x10 * static_cast<uint64_t>(i)));
  }
  const BlockId merge = f.block(0x4000);
  const BlockId hub = f.block(0x5000);

  const ExprId b1 = f.function.select(f.function.binary(ExprOp::CmpNe, f.entryReg("x0"), f.i64(0)),
                                      f.i64(0x2a2), f.i64(0xb6));
  const ExprId b2 = f.function.select(f.function.binary(ExprOp::CmpNe, f.entryReg("x1"), f.i64(0)),
                                      f.i64(0x69), f.i64(0x1cf));
  const ExprId b3 = f.function.select(f.function.binary(ExprOp::CmpNe, f.entryReg("x2"), f.i64(0)),
                                      f.i64(0x213), f.i64(0x1));

  f.dispatchSite(d1, 0x1e70a0, f.clampedIndex(b1, 0x2cc, 0x213), {handlers[0], handlers[1]});
  f.dispatchSite(d2, 0x1e70a0, f.clampedIndex(b2, 0x2cc, 0x213), {handlers[2], handlers[3]});
  f.dispatchSite(d3, 0x1e70a0, f.clampedIndex(b3, 0x2cc, 0x213), {handlers[4], handlers[5]});
  for (const BlockId handler : handlers) {
    f.function.appendBranch(handler, f.function.block(handler).va, merge);
  }
  f.function.appendBranch(merge, f.function.block(merge).va, hub);
  f.function.appendReturn(hub, f.function.block(hub).va);
  f.function.rebuildEdges();

  const auto regions = findDispatchRegions(f.function);
  REQUIRE(regions.size() == 1);
  REQUIRE(regions.front().sharedTail.has_value());
  CHECK(regions.front().sharedTail->merge == merge);
  CHECK(regions.front().sharedTail->hub == hub);
}

TEST_CASE("sites reading different table bases stay in separate regions",
          "[analysis][dispatch-region]") {
  Fixture f;
  const BlockId d1 = f.block(0x2000);
  const BlockId d2 = f.block(0x2100);
  const BlockId h1a = f.block(0x3000);
  const BlockId h1b = f.block(0x3010);
  const BlockId h2a = f.block(0x3100);
  const BlockId h2b = f.block(0x3110);

  const ExprId b1 = f.function.select(f.function.binary(ExprOp::CmpNe, f.entryReg("x0"), f.i64(0)),
                                      f.i64(0x2a2), f.i64(0xb6));
  const ExprId b2 = f.function.select(f.function.binary(ExprOp::CmpNe, f.entryReg("x1"), f.i64(0)),
                                      f.i64(0x69), f.i64(0x1cf));

  f.dispatchSite(d1, 0x1e70a0, f.clampedIndex(b1, 0x2cc, 0x213), {h1a, h1b});
  f.dispatchSite(d2, 0x2a2428, f.clampedIndex(b2, 0x2cc, 0x213), {h2a, h2b});
  f.function.rebuildEdges();

  const auto regions = findDispatchRegions(f.function);
  REQUIRE(regions.size() == 2);
  CHECK(regions[0].sites.size() == 1);
  CHECK(regions[1].sites.size() == 1);
}

TEST_CASE("sites reading the same table but different clamp bounds stay in separate regions",
          "[analysis][dispatch-region]") {
  Fixture f;
  const BlockId d1 = f.block(0x2000);
  const BlockId d2 = f.block(0x2100);
  const BlockId h1a = f.block(0x3000);
  const BlockId h1b = f.block(0x3010);
  const BlockId h2a = f.block(0x3100);
  const BlockId h2b = f.block(0x3110);

  const ExprId b1 = f.function.select(f.function.binary(ExprOp::CmpNe, f.entryReg("x0"), f.i64(0)),
                                      f.i64(0x2a2), f.i64(0xb6));
  const ExprId b2 = f.function.select(f.function.binary(ExprOp::CmpNe, f.entryReg("x1"), f.i64(0)),
                                      f.i64(0x69), f.i64(0x1cf));

  f.dispatchSite(d1, 0x1e70a0, f.clampedIndex(b1, 0x2cc, 0x213), {h1a, h1b});
  f.dispatchSite(d2, 0x1e70a0, f.clampedIndex(b2, 0x555, 0x1), {h2a, h2b});
  f.function.rebuildEdges();

  const auto regions = findDispatchRegions(f.function);
  REQUIRE(regions.size() == 2);
}

TEST_CASE("a function with no resolved indirect branch reports no regions",
          "[analysis][dispatch-region]") {
  Fixture f;
  f.function.appendReturn(f.entry, 0x1000);
  f.function.rebuildEdges();
  CHECK(findDispatchRegions(f.function).empty());
}

TEST_CASE("confirmDispatcherShapeFromRegion confirms a two-target site the region backs",
          "[analysis][dispatch-region]") {
  // Exactly the shape matchDispatcherShape's own floor rejects on its own --
  // two targets, not three -- confirmed instead from the region's pooled
  // vote (see DispatchRegion::sharedTail).
  Fixture f;
  const BlockId dispatch = f.block(0x2000);
  const BlockId h1 = f.block(0x3000);
  const BlockId h2 = f.block(0x3100);
  const BlockId merge = f.block(0x4000);
  const BlockId hub = f.block(0x5000);
  const il::OpId brind =
      f.function.appendIndirectBranch(dispatch, 0x2000, f.function.undefined(Type::integer(64)));
  f.function.setTargets(brind, std::vector<BlockId>{h1, h2});
  f.function.appendBranch(h1, 0x3000, merge);
  f.function.appendBranch(h2, 0x3100, merge);
  f.function.rebuildEdges();

  DispatchRegion region;
  region.sharedTail = DispatchRegionTail{merge, hub};
  const std::vector<BlockId> targets{h1, h2};
  const auto shape = confirmDispatcherShapeFromRegion(f.function, dispatch, targets, region);
  REQUIRE(shape.has_value());
  CHECK(shape->dispatch == dispatch);
  CHECK(shape->merge == merge);
  CHECK(shape->hub == hub);
}

TEST_CASE("confirmDispatcherShapeFromRegion refuses a target that does not itself reach the tail",
          "[analysis][dispatch-region]") {
  // A region-wide majority is evidence the shape exists somewhere in the
  // region, not that *this* site's own targets are part of it -- h2 here
  // dispatches further instead of falling into merge, so this site is a
  // genuine dispatch tree, not the simple shared-tail shape.
  Fixture f;
  const BlockId dispatch = f.block(0x2000);
  const BlockId h1 = f.block(0x3000);
  const BlockId h2 = f.block(0x3100);
  const BlockId other = f.block(0x3200);
  const BlockId merge = f.block(0x4000);
  const BlockId hub = f.block(0x5000);
  const il::OpId brind =
      f.function.appendIndirectBranch(dispatch, 0x2000, f.function.undefined(Type::integer(64)));
  f.function.setTargets(brind, std::vector<BlockId>{h1, h2});
  f.function.appendBranch(h1, 0x3000, merge);
  f.function.appendBranch(h2, 0x3100, other);
  f.function.appendBranch(other, 0x3200, merge);
  f.function.rebuildEdges();

  DispatchRegion region;
  region.sharedTail = DispatchRegionTail{merge, hub};
  const std::vector<BlockId> targets{h1, h2};
  CHECK_FALSE(confirmDispatcherShapeFromRegion(f.function, dispatch, targets, region).has_value());
}

TEST_CASE("confirmDispatcherShapeFromRegion refuses a target reached from more than dispatch",
          "[analysis][dispatch-region]") {
  Fixture f;
  const BlockId dispatch = f.block(0x2000);
  const BlockId other = f.block(0x2100);
  const BlockId h1 = f.block(0x3000);
  const BlockId h2 = f.block(0x3100);
  const BlockId merge = f.block(0x4000);
  const BlockId hub = f.block(0x5000);
  const il::OpId brind =
      f.function.appendIndirectBranch(dispatch, 0x2000, f.function.undefined(Type::integer(64)));
  f.function.setTargets(brind, std::vector<BlockId>{h1, h2});
  f.function.appendBranch(h1, 0x3000, merge);
  // h2 is also reached from `other` -- not a private handler.
  f.function.appendBranch(other, 0x2100, h2);
  f.function.appendBranch(h2, 0x3100, merge);
  f.function.rebuildEdges();

  DispatchRegion region;
  region.sharedTail = DispatchRegionTail{merge, hub};
  const std::vector<BlockId> targets{h1, h2};
  CHECK_FALSE(confirmDispatcherShapeFromRegion(f.function, dispatch, targets, region).has_value());
}

TEST_CASE("confirmDispatcherShapeFromRegion is honest about a region with no shared tail",
          "[analysis][dispatch-region]") {
  Fixture f;
  const BlockId dispatch = f.block(0x2000);
  const BlockId h1 = f.block(0x3000);
  const BlockId h2 = f.block(0x3100);
  const BlockId merge = f.block(0x4000);
  f.function.appendBranch(h1, 0x3000, merge);
  f.function.appendBranch(h2, 0x3100, merge);
  f.function.rebuildEdges();

  DispatchRegion region;  // sharedTail left absent, the common honest case
  const std::vector<BlockId> targets{h1, h2};
  CHECK_FALSE(confirmDispatcherShapeFromRegion(f.function, dispatch, targets, region).has_value());
}

TEST_CASE("findDispatchJoins recognises a hub fed by three different sites' own private tails",
          "[analysis][dispatch-region][dispatch-join]") {
  // J2e (docs/architecture-optimization-eval-prompt.md §6.3): no single site
  // here has three targets to vote a shared tail with (matchDispatcherShape's
  // own floor), and the three sites don't even pool into one region-wide
  // majority (matchRegionSharedTail) since each site's *other* target keeps
  // re-dispatching rather than converging anywhere -- but one target from
  // each of the three still independently falls into the same local hub.
  Fixture f;
  const BlockId d1 = f.block(0x2000);
  const BlockId d2 = f.block(0x2100);
  const BlockId d3 = f.block(0x2200);
  const BlockId tail1 = f.block(0x3000);
  const BlockId tail2 = f.block(0x3100);
  const BlockId tail3 = f.block(0x3200);
  const BlockId hub = f.block(0x4000);

  const ExprId b1 = f.function.select(f.function.binary(ExprOp::CmpNe, f.entryReg("x0"), f.i64(0)),
                                      f.i64(0x2a2), f.i64(0xb6));
  const ExprId b2 = f.function.select(f.function.binary(ExprOp::CmpNe, f.entryReg("x1"), f.i64(0)),
                                      f.i64(0x69), f.i64(0x1cf));
  const ExprId b3 = f.function.select(f.function.binary(ExprOp::CmpNe, f.entryReg("x2"), f.i64(0)),
                                      f.i64(0x213), f.i64(0x1));

  // Each site's *other* target re-dispatches through the very same table --
  // the scatter-dispatcher shape that keeps a region-wide sharedTail vote
  // from ever finding a winner, per §4.2's own diagnosis.
  f.dispatchSite(d1, 0x1e70a0, f.clampedIndex(b1, 0x2cc, 0x213), {tail1, d2});
  f.dispatchSite(d2, 0x1e70a0, f.clampedIndex(b2, 0x2cc, 0x213), {tail2, d3});
  f.dispatchSite(d3, 0x1e70a0, f.clampedIndex(b3, 0x2cc, 0x213), {tail3, d1});
  f.function.appendBranch(tail1, f.function.block(tail1).va, hub);
  f.function.appendBranch(tail2, f.function.block(tail2).va, hub);
  f.function.appendBranch(tail3, f.function.block(tail3).va, hub);
  f.function.appendReturn(hub, f.function.block(hub).va);
  f.function.rebuildEdges();

  const auto regions = findDispatchRegions(f.function);
  REQUIRE(regions.size() == 1);
  CHECK_FALSE(regions.front().sharedTail.has_value());

  const auto joins = findDispatchJoins(f.function, regions.front());
  REQUIRE(joins.size() == 1);
  CHECK(joins.front().hub == hub);
  CHECK(joins.front().tails.size() == 3);
  CHECK(std::find(joins.front().tails.begin(), joins.front().tails.end(), tail1) !=
        joins.front().tails.end());
  CHECK(std::find(joins.front().tails.begin(), joins.front().tails.end(), tail2) !=
        joins.front().tails.end());
  CHECK(std::find(joins.front().tails.begin(), joins.front().tails.end(), tail3) !=
        joins.front().tails.end());
}

TEST_CASE("findDispatchJoins declines a hub with a predecessor outside the region's own tails",
          "[analysis][dispatch-region][dispatch-join]") {
  Fixture f;
  const BlockId d1 = f.block(0x2000);
  const BlockId d2 = f.block(0x2100);
  const BlockId tail1 = f.block(0x3000);
  const BlockId tail2 = f.block(0x3100);
  const BlockId hub = f.block(0x4000);
  const BlockId outsider = f.block(0x5000);

  const ExprId b1 = f.function.select(f.function.binary(ExprOp::CmpNe, f.entryReg("x0"), f.i64(0)),
                                      f.i64(0x2a2), f.i64(0xb6));
  const ExprId b2 = f.function.select(f.function.binary(ExprOp::CmpNe, f.entryReg("x1"), f.i64(0)),
                                      f.i64(0x69), f.i64(0x1cf));

  f.dispatchSite(d1, 0x1e70a0, f.clampedIndex(b1, 0x2cc, 0x213), {tail1, d2});
  f.dispatchSite(d2, 0x1e70a0, f.clampedIndex(b2, 0x2cc, 0x213), {tail2, d1});
  f.function.appendBranch(tail1, f.function.block(tail1).va, hub);
  f.function.appendBranch(tail2, f.function.block(tail2).va, hub);
  // A fourth predecessor with no connection to this region at all.
  f.function.appendBranch(outsider, f.function.block(outsider).va, hub);
  f.function.appendReturn(hub, f.function.block(hub).va);
  f.function.rebuildEdges();

  const auto regions = findDispatchRegions(f.function);
  REQUIRE(regions.size() == 1);
  CHECK(findDispatchJoins(f.function, regions.front()).empty());
}

TEST_CASE("buildDispatchNestGraph chains three sites whose handlers re-enter the same table",
          "[analysis][dispatch-region][dispatch-nest]") {
  // docs/19-scatter-dispatch-target-shape.md's own reference shape: each
  // site's "keep dispatching" arm runs a short handler (here just a plain
  // branch, standing in for the couple of MBA ops a real one has) that
  // lands squarely on the next site's own dispatch block -- d1 -> d2 -> d3,
  // one root, depth 2, both d2 and d3 counted as nested.
  Fixture f;
  const BlockId d1 = f.block(0x2000);
  const BlockId d2 = f.block(0x2100);
  const BlockId d3 = f.block(0x2200);
  const BlockId leaf1 = f.block(0x3000);
  const BlockId chain1 = f.block(0x3010);
  const BlockId leaf2 = f.block(0x3100);
  const BlockId chain2 = f.block(0x3110);
  const BlockId leaf3a = f.block(0x3200);
  const BlockId leaf3b = f.block(0x3210);

  const ExprId b1 = f.function.select(f.function.binary(ExprOp::CmpNe, f.entryReg("x0"), f.i64(0)),
                                      f.i64(0x2a2), f.i64(0xb6));
  const ExprId b2 = f.function.select(f.function.binary(ExprOp::CmpNe, f.entryReg("x1"), f.i64(0)),
                                      f.i64(0x69), f.i64(0x1cf));
  const ExprId b3 = f.function.select(f.function.binary(ExprOp::CmpNe, f.entryReg("x2"), f.i64(0)),
                                      f.i64(0x213), f.i64(0x1));

  f.dispatchSite(d1, 0x1e70a0, f.clampedIndex(b1, 0x2cc, 0x213), {leaf1, chain1});
  f.dispatchSite(d2, 0x1e70a0, f.clampedIndex(b2, 0x2cc, 0x213), {leaf2, chain2});
  f.dispatchSite(d3, 0x1e70a0, f.clampedIndex(b3, 0x2cc, 0x213), {leaf3a, leaf3b});
  f.function.appendReturn(leaf1, f.function.block(leaf1).va);
  f.function.appendBranch(chain1, f.function.block(chain1).va, d2);
  f.function.appendReturn(leaf2, f.function.block(leaf2).va);
  f.function.appendBranch(chain2, f.function.block(chain2).va, d3);
  f.function.appendReturn(leaf3a, f.function.block(leaf3a).va);
  f.function.appendReturn(leaf3b, f.function.block(leaf3b).va);
  f.function.rebuildEdges();

  const auto regions = findDispatchRegions(f.function);
  REQUIRE(regions.size() == 1);
  CHECK_FALSE(regions.front().sharedTail.has_value());

  const auto nest = xdec::analysis::buildDispatchNestGraph(f.function, regions.front());
  REQUIRE(nest.roots.size() == 1);
  CHECK(nest.roots.front() == d1);
  CHECK(nest.maxDepth == 2);
  CHECK(nest.nestedSiteCount == 2);
  const auto d1Children = nest.children.find(d1);
  REQUIRE(d1Children != nest.children.end());
  REQUIRE(d1Children->second.size() == 1);
  CHECK(d1Children->second.front() == d2);
  const auto d2Children = nest.children.find(d2);
  REQUIRE(d2Children != nest.children.end());
  REQUIRE(d2Children->second.size() == 1);
  CHECK(d2Children->second.front() == d3);
  CHECK_FALSE(nest.children.contains(d3));
}

TEST_CASE("buildDispatchNestGraph reports every site as its own root when none re-enter the table",
          "[analysis][dispatch-region][dispatch-nest]") {
  Fixture f;
  const BlockId d1 = f.block(0x2000);
  const BlockId d2 = f.block(0x2100);
  const BlockId h1a = f.block(0x3000);
  const BlockId h1b = f.block(0x3010);
  const BlockId h2a = f.block(0x3100);
  const BlockId h2b = f.block(0x3110);

  const ExprId b1 = f.function.select(f.function.binary(ExprOp::CmpNe, f.entryReg("x0"), f.i64(0)),
                                      f.i64(0x2a2), f.i64(0xb6));
  const ExprId b2 = f.function.select(f.function.binary(ExprOp::CmpNe, f.entryReg("x1"), f.i64(0)),
                                      f.i64(0x69), f.i64(0x1cf));

  f.dispatchSite(d1, 0x1e70a0, f.clampedIndex(b1, 0x2cc, 0x213), {h1a, h1b});
  f.dispatchSite(d2, 0x1e70a0, f.clampedIndex(b2, 0x2cc, 0x213), {h2a, h2b});
  f.function.appendReturn(h1a, f.function.block(h1a).va);
  f.function.appendReturn(h1b, f.function.block(h1b).va);
  f.function.appendReturn(h2a, f.function.block(h2a).va);
  f.function.appendReturn(h2b, f.function.block(h2b).va);
  f.function.rebuildEdges();

  const auto regions = findDispatchRegions(f.function);
  REQUIRE(regions.size() == 1);

  const auto nest = xdec::analysis::buildDispatchNestGraph(f.function, regions.front());
  CHECK(nest.children.empty());
  CHECK(nest.maxDepth == 0);
  CHECK(nest.nestedSiteCount == 0);
  REQUIRE(nest.roots.size() == 2);
  CHECK(std::find(nest.roots.begin(), nest.roots.end(), d1) != nest.roots.end());
  CHECK(std::find(nest.roots.begin(), nest.roots.end(), d2) != nest.roots.end());
}

TEST_CASE("findDispatchJoins does not count a single feeding tail as a join",
          "[analysis][dispatch-region][dispatch-join]") {
  Fixture f;
  const BlockId d1 = f.block(0x2000);
  const BlockId d2 = f.block(0x2100);
  const BlockId tail1 = f.block(0x3000);
  const BlockId other = f.block(0x3100);
  const BlockId hub = f.block(0x4000);

  const ExprId b1 = f.function.select(f.function.binary(ExprOp::CmpNe, f.entryReg("x0"), f.i64(0)),
                                      f.i64(0x2a2), f.i64(0xb6));
  const ExprId b2 = f.function.select(f.function.binary(ExprOp::CmpNe, f.entryReg("x1"), f.i64(0)),
                                      f.i64(0x69), f.i64(0x1cf));

  // d2's own targets never reach hub at all -- only tail1 (a single
  // predecessor) does, which is an ordinary private handler, not a join.
  f.dispatchSite(d1, 0x1e70a0, f.clampedIndex(b1, 0x2cc, 0x213), {tail1, d2});
  f.dispatchSite(d2, 0x1e70a0, f.clampedIndex(b2, 0x2cc, 0x213), {other, d1});
  f.function.appendBranch(tail1, f.function.block(tail1).va, hub);
  f.function.appendReturn(other, f.function.block(other).va);
  f.function.appendReturn(hub, f.function.block(hub).va);
  f.function.rebuildEdges();

  const auto regions = findDispatchRegions(f.function);
  REQUIRE(regions.size() == 1);
  CHECK(findDispatchJoins(f.function, regions.front()).empty());
}
