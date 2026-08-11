// matchGuardCascade: two nested guards sharing one fallback body, and honest
// misses when the shape does not hold (see the header for why this is not
// just an unlucky diamond).
#include <catch2/catch_test_macros.hpp>

#include "il/il_test_support.h"
#include "xdec/analysis/dominators.h"
#include "xdec/analysis/guard_cascade.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::GuardCascadeShape;
using xdec::analysis::matchGuardCascade;
using xdec::analysis::PostDominators;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::Function;
using xdec::il::Type;

namespace {

struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {
    entry = function.createBlock(0x1000);
    function.setEntryBlock(entry);
  }

  BlockId block(uint64_t va) { return function.createBlock(va); }
  ExprId cond(std::string_view reg) { return function.entryReg(function.registers().find(reg)); }

  Function function;
  BlockId entry;
};

}  // namespace

TEST_CASE("two nested guards sharing one fallback match the cascade",
          "[analysis][guard-cascade]") {
  // Mirrors bc_lib's sub_2f9a38: outer guard's bad arm and inner guard's bad
  // arm both land on the same fallback, which itself falls into the merge
  // the inner guard's good arm reaches directly.
  Fixture f;
  const BlockId outer = f.entry;
  const BlockId inner = f.block(0x2000);
  const BlockId fallback = f.block(0x3000);
  const BlockId merge = f.block(0x4000);

  f.function.appendCondBranch(outer, 0x1000, f.cond("x0"), fallback, inner);
  f.function.appendCondBranch(inner, 0x2000, f.cond("x1"), merge, fallback);
  f.function.appendBranch(fallback, 0x3000, merge);
  f.function.appendReturn(merge, 0x4000);
  f.function.rebuildEdges();

  const PostDominators postDominators = PostDominators::compute(f.function);
  const auto shape = matchGuardCascade(f.function, postDominators, outer);
  REQUIRE(shape.has_value());
  CHECK(shape->outerHead == outer);
  CHECK(shape->innerHead == inner);
  CHECK(shape->fallback == fallback);
  CHECK(shape->merge == merge);
  CHECK(shape->innerSuccessIsTaken);
}

TEST_CASE("the same shape with the arms swapped still matches",
          "[analysis][guard-cascade]") {
  Fixture f;
  const BlockId outer = f.entry;
  const BlockId inner = f.block(0x2000);
  const BlockId fallback = f.block(0x3000);
  const BlockId merge = f.block(0x4000);

  // Outer's taken arm is the inner guard this time, and inner's untaken arm
  // is the merge -- every polarity flipped from the case above.
  f.function.appendCondBranch(outer, 0x1000, f.cond("x0"), inner, fallback);
  f.function.appendCondBranch(inner, 0x2000, f.cond("x1"), fallback, merge);
  f.function.appendBranch(fallback, 0x3000, merge);
  f.function.appendReturn(merge, 0x4000);
  f.function.rebuildEdges();

  const PostDominators postDominators = PostDominators::compute(f.function);
  const auto shape = matchGuardCascade(f.function, postDominators, outer);
  REQUIRE(shape.has_value());
  CHECK(shape->innerHead == inner);
  CHECK(shape->fallback == fallback);
  CHECK_FALSE(shape->innerSuccessIsTaken);
}

TEST_CASE("a fallback reached from only one guard is an ordinary diamond, not a cascade",
          "[analysis][guard-cascade]") {
  // tryDiamond already closes this shape on its own (each arm has exactly
  // one predecessor); matchGuardCascade must stay out of its way.
  Fixture f;
  const BlockId outer = f.entry;
  const BlockId inner = f.block(0x2000);
  const BlockId fallback = f.block(0x3000);
  const BlockId merge = f.block(0x4000);

  f.function.appendCondBranch(outer, 0x1000, f.cond("x0"), fallback, inner);
  f.function.appendCondBranch(inner, 0x2000, f.cond("x1"), merge, merge);
  f.function.appendBranch(fallback, 0x3000, merge);
  f.function.appendReturn(merge, 0x4000);
  f.function.rebuildEdges();

  const PostDominators postDominators = PostDominators::compute(f.function);
  CHECK_FALSE(matchGuardCascade(f.function, postDominators, outer).has_value());
}

TEST_CASE("an inner guard whose failure arm leaves through a different block is not a cascade",
          "[analysis][guard-cascade]") {
  Fixture f;
  const BlockId outer = f.entry;
  const BlockId inner = f.block(0x2000);
  const BlockId fallback = f.block(0x3000);
  const BlockId otherExit = f.block(0x3100);
  const BlockId merge = f.block(0x4000);

  f.function.appendCondBranch(outer, 0x1000, f.cond("x0"), fallback, inner);
  f.function.appendCondBranch(inner, 0x2000, f.cond("x1"), merge, otherExit);
  f.function.appendBranch(fallback, 0x3000, merge);
  f.function.appendReturn(otherExit, 0x3100);
  f.function.appendReturn(merge, 0x4000);
  f.function.rebuildEdges();

  const PostDominators postDominators = PostDominators::compute(f.function);
  CHECK_FALSE(matchGuardCascade(f.function, postDominators, outer).has_value());
}

TEST_CASE("a fallback with a third predecessor is not privately shared", "[analysis][guard-cascade]") {
  Fixture f;
  const BlockId outer = f.entry;
  const BlockId inner = f.block(0x2000);
  const BlockId fallback = f.block(0x3000);
  const BlockId merge = f.block(0x4000);
  const BlockId outsider = f.block(0x5000);

  f.function.appendCondBranch(outer, 0x1000, f.cond("x0"), fallback, inner);
  f.function.appendCondBranch(inner, 0x2000, f.cond("x1"), merge, fallback);
  f.function.appendBranch(fallback, 0x3000, merge);
  f.function.appendBranch(outsider, 0x5000, fallback);
  f.function.appendReturn(merge, 0x4000);
  f.function.rebuildEdges();

  const PostDominators postDominators = PostDominators::compute(f.function);
  CHECK_FALSE(matchGuardCascade(f.function, postDominators, outer).has_value());
}

TEST_CASE("a fallback that branches away from the inner guard's own merge is not a cascade",
          "[analysis][guard-cascade]") {
  Fixture f;
  const BlockId outer = f.entry;
  const BlockId inner = f.block(0x2000);
  const BlockId fallback = f.block(0x3000);
  const BlockId merge = f.block(0x4000);
  const BlockId elsewhere = f.block(0x4100);

  f.function.appendCondBranch(outer, 0x1000, f.cond("x0"), fallback, inner);
  f.function.appendCondBranch(inner, 0x2000, f.cond("x1"), merge, fallback);
  f.function.appendBranch(fallback, 0x3000, elsewhere);
  f.function.appendReturn(merge, 0x4000);
  f.function.appendReturn(elsewhere, 0x4100);
  f.function.rebuildEdges();

  const PostDominators postDominators = PostDominators::compute(f.function);
  CHECK_FALSE(matchGuardCascade(f.function, postDominators, outer).has_value());
}

TEST_CASE("an inner guard reached from somewhere other than the outer guard is not private",
          "[analysis][guard-cascade]") {
  Fixture f;
  const BlockId outer = f.entry;
  const BlockId inner = f.block(0x2000);
  const BlockId fallback = f.block(0x3000);
  const BlockId merge = f.block(0x4000);
  const BlockId outsider = f.block(0x1500);

  f.function.appendBranch(outsider, 0x1500, inner);
  f.function.appendCondBranch(outer, 0x1000, f.cond("x0"), fallback, inner);
  f.function.appendCondBranch(inner, 0x2000, f.cond("x1"), merge, fallback);
  f.function.appendBranch(fallback, 0x3000, merge);
  f.function.appendReturn(merge, 0x4000);
  f.function.rebuildEdges();

  const PostDominators postDominators = PostDominators::compute(f.function);
  CHECK_FALSE(matchGuardCascade(f.function, postDominators, outer).has_value());
}

TEST_CASE("a block with no CondBranch terminator is never an outer guard",
          "[analysis][guard-cascade]") {
  Fixture f;
  f.function.appendReturn(f.entry, 0x1000);
  f.function.rebuildEdges();

  const PostDominators postDominators = PostDominators::compute(f.function);
  CHECK_FALSE(matchGuardCascade(f.function, postDominators, f.entry).has_value());
}
