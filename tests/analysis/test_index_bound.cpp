// boundOnIndex: structural bounds proved from a select's own shape, with no
// guard and no dominator to climb -- and the dead-arm refinement that keeps
// one of those selects from claiming a table entry nothing ever reaches.
#include <catch2/catch_test_macros.hpp>

#include "il/il_test_support.h"
#include "xdec/analysis/dominators.h"
#include "xdec/analysis/index_bound.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::boundOnIndex;
using xdec::analysis::Dominators;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::Type;

namespace {

/// One block ending in an indirect branch: enough for Dominators to compute
/// and enough for boundOnIndex's dominator walk to find no CondBranch guard
/// to climb, so every test here is exercising localBound/armBound's purely
/// structural reasoning, not the guard-climbing half of the contract.
struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {
    entry = function.createBlock(0x1000);
    function.setEntryBlock(entry);
  }

  ExprId i64(uint64_t value) { return function.constant(Type::integer(64), value); }
  ExprId reg(const char* name) {
    return function.entryReg(function.registers().find(name));
  }
  void terminate() {
    function.appendIndirectBranch(entry, 0x1000, i64(0));
    function.rebuildEdges();
  }

  Function function;
  BlockId entry;
};

}  // namespace

TEST_CASE("a zero-extended one-bit flag bounds a select's dead arm out of the total",
          "[analysis][index-bound]") {
  // sub_199214's exact shape: a boolean (here, a comparison standing in for
  // the flag a syscall's errno check produces) zero-extended into i32 then
  // i64 -- localBound already reads that chain as "at most 1" -- selected
  // against a constant 2 on a signed guard that can never actually pick it:
  // `3 <s x` is never true of an `x` that chain has already bounded to 1.
  // Before the dead-arm check, the select's bound was max(2, 1) = 2, one
  // more entry than the table has live cases for.
  Fixture f;
  const ExprId flag = f.function.binary(ExprOp::CmpEq, f.reg("x0"), f.i64(0));
  const ExprId widened32 = f.function.cast(ExprOp::ZExt, Type::integer(32), flag);
  const ExprId widened64 = f.function.cast(ExprOp::ZExt, Type::integer(64), widened32);
  const ExprId condition = f.function.binary(ExprOp::CmpLtS, f.i64(3), widened64);
  const ExprId index = f.function.select(condition, f.i64(2), widened64);
  f.terminate();

  const Dominators dominators = Dominators::compute(f.function);
  const auto bound = boundOnIndex(f.function, dominators, f.entry, index);
  REQUIRE(bound.has_value());
  CHECK(*bound == 1);
}

TEST_CASE("a select whose condition can go either way still needs both arms bounded",
          "[analysis][index-bound]") {
  // The soundness check on the dead-arm path: nothing here proves `x2reg`
  // small, so the true arm (a wide, unbounded value) has to count, and the
  // select is unbounded, not accidentally 2.
  Fixture f;
  const ExprId opaque = f.reg("x2");
  const ExprId condition = f.function.binary(ExprOp::CmpLtS, f.i64(3), opaque);
  const ExprId index = f.function.select(condition, opaque, f.i64(2));
  f.terminate();

  const Dominators dominators = Dominators::compute(f.function);
  CHECK_FALSE(boundOnIndex(f.function, dominators, f.entry, index).has_value());
}

TEST_CASE("a branchless saturating clamp bounds the index to the clamp's threshold",
          "[analysis][index-bound]") {
  // `state > 5 ? 5 : state`, compiled with CSEL and no branch at all -- the
  // pre-existing readComparison/upperBound path this file had no direct
  // coverage for before.
  Fixture f;
  const ExprId state = f.reg("x3");
  const ExprId condition = f.function.binary(ExprOp::CmpLtU, f.i64(5), state);
  const ExprId index = f.function.select(condition, f.i64(5), state);
  f.terminate();

  const Dominators dominators = Dominators::compute(f.function);
  const auto bound = boundOnIndex(f.function, dominators, f.entry, index);
  REQUIRE(bound.has_value());
  CHECK(*bound == 5);
}
