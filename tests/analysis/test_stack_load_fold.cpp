// findFoldableStackLoads: the safety rules that decide when a Load from a
// stack slot is redundant enough for the emitter to print the slot's own
// name in place of a temporary (see the header for the full rule list).
#include <catch2/catch_test_macros.hpp>

#include <unordered_set>

#include "il/il_test_support.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/analysis/stack_load_fold.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::FoldableStackLoad;
using xdec::analysis::StackFrame;
using xdec::analysis::findFoldableStackLoads;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::OpId;
using xdec::il::RegId;
using xdec::il::Type;

namespace {

struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {
    entry = function.createBlock(0x1000);
    function.setEntryBlock(entry);
  }

  RegId reg(std::string_view name) { return function.registers().find(name); }
  ExprId i64(uint64_t value) { return function.constant(Type::integer(64), value); }
  ExprId slot(int64_t delta) {
    const ExprId sp = function.entryReg(reg("sp"));
    return delta < 0 ? function.binary(ExprOp::Sub, sp, i64(static_cast<uint64_t>(-delta)))
                     : function.binary(ExprOp::Add, sp, i64(static_cast<uint64_t>(delta)));
  }

  Function function;
  BlockId entry;
};

}  // namespace

TEST_CASE("a load read once right after it, nothing between, folds",
          "[analysis][stack-load-fold]") {
  Fixture f;
  f.function.appendStore(f.entry, 0x1000, Type::integer(32), f.slot(-0x10),
                         f.function.entryReg(f.reg("x0")));
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1004, Type::integer(32), f.slot(-0x10));
  f.function.appendStore(f.entry, 0x1008, Type::integer(32), f.i64(0x9000),
                         f.function.valueRef(loaded));
  f.function.appendReturn(f.entry, 0x100c);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto foldable = findFoldableStackLoads(f.function, frame, {});
  const auto load = f.function.op(OpId{1});
  REQUIRE(load.code == il::OpCode::Load);
  const auto found = foldable.find(1);
  REQUIRE(found != foldable.end());
  CHECK(found->second.delta == -0x10);
  CHECK(found->second.width == 32);
  CHECK(!found->second.usedAsAddress);
}

TEST_CASE("an intervening store that may alias the slot blocks the fold",
          "[analysis][stack-load-fold]") {
  Fixture f;
  f.function.appendStore(f.entry, 0x1000, Type::integer(32), f.slot(-0x10),
                         f.function.entryReg(f.reg("x0")));
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1004, Type::integer(32), f.slot(-0x10));
  // A second store to the same slot, between the load and its one reader:
  // by the time the reader runs, the load's value may no longer be fresh.
  f.function.appendStore(f.entry, 0x1008, Type::integer(32), f.slot(-0x10),
                         f.function.entryReg(f.reg("x1")));
  f.function.appendStore(f.entry, 0x100c, Type::integer(32), f.i64(0x9000),
                         f.function.valueRef(loaded));
  f.function.appendReturn(f.entry, 0x1010);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto foldable = findFoldableStackLoads(f.function, frame, {});
  CHECK(foldable.find(1) == foldable.end());
}

TEST_CASE("a call between the load and its use blocks the fold",
          "[analysis][stack-load-fold]") {
  Fixture f;
  f.function.appendStore(f.entry, 0x1000, Type::integer(32), f.slot(-0x10),
                         f.function.entryReg(f.reg("x0")));
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1004, Type::integer(32), f.slot(-0x10));
  f.function.appendCall(f.entry, 0x1008, f.i64(0x8000));
  f.function.appendStore(f.entry, 0x100c, Type::integer(32), f.i64(0x9000),
                         f.function.valueRef(loaded));
  f.function.appendReturn(f.entry, 0x1010);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto foldable = findFoldableStackLoads(f.function, frame, {});
  CHECK(foldable.find(1) == foldable.end());
}

TEST_CASE("a use in a different block blocks the fold", "[analysis][stack-load-fold]") {
  Fixture f;
  f.function.appendStore(f.entry, 0x1000, Type::integer(32), f.slot(-0x10),
                         f.function.entryReg(f.reg("x0")));
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1004, Type::integer(32), f.slot(-0x10));
  const BlockId other = f.function.createBlock(0x2000);
  f.function.appendBranch(f.entry, 0x1008, other);
  f.function.appendStore(other, 0x2000, Type::integer(32), f.i64(0x9000),
                         f.function.valueRef(loaded));
  f.function.appendReturn(other, 0x2004);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto foldable = findFoldableStackLoads(f.function, frame, {});
  CHECK(foldable.find(1) == foldable.end());
}

TEST_CASE("a load classified against a global address, not a stack slot, is left alone",
          "[analysis][stack-load-fold]") {
  Fixture f;
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1000, Type::integer(32), f.i64(0x30c420));
  f.function.appendStore(f.entry, 0x1004, Type::integer(32), f.i64(0x9000),
                         f.function.valueRef(loaded));
  f.function.appendReturn(f.entry, 0x1008);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto foldable = findFoldableStackLoads(f.function, frame, {});
  CHECK(foldable.empty());
}

TEST_CASE("two live readers in the same block, both after the load and both fresh, both fold",
          "[analysis][stack-load-fold]") {
  Fixture f;
  f.function.appendStore(f.entry, 0x1000, Type::integer(32), f.slot(-0x10),
                         f.function.entryReg(f.reg("x0")));
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1004, Type::integer(32), f.slot(-0x10));
  // Neither sink writes through the slot itself (both addresses are globals,
  // provably disjoint from the frame), so both stay fresh reads of the load.
  f.function.appendStore(f.entry, 0x1008, Type::integer(32), f.i64(0x9000),
                         f.function.valueRef(loaded));
  f.function.appendStore(f.entry, 0x100c, Type::integer(32), f.i64(0x9008),
                         f.function.valueRef(loaded));
  f.function.appendReturn(f.entry, 0x1010);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto foldable = findFoldableStackLoads(f.function, frame, {});
  REQUIRE(foldable.find(1) != foldable.end());
}

TEST_CASE("a load already dead does not stop the fold, its use elsewhere still counts",
          "[analysis][stack-load-fold]") {
  Fixture f;
  f.function.appendStore(f.entry, 0x1000, Type::integer(32), f.slot(-0x10),
                         f.function.entryReg(f.reg("x0")));
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1004, Type::integer(32), f.slot(-0x10));
  const il::OpId deadSink = f.function.appendStore(f.entry, 0x1008, Type::integer(32),
                                                   f.i64(0x9000), f.function.valueRef(loaded));
  f.function.appendStore(f.entry, 0x100c, Type::integer(32), f.i64(0x9008),
                         f.function.valueRef(loaded));
  f.function.appendReturn(f.entry, 0x1010);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  // Pretend an emit prepass already decided the first sink never prints.
  const std::unordered_set<uint32_t> deadOps{deadSink.index()};
  const auto foldable = findFoldableStackLoads(f.function, frame, deadOps);
  REQUIRE(foldable.find(1) != foldable.end());
}

TEST_CASE("a load whose sole use is another access's address is flagged usedAsAddress",
          "[analysis][stack-load-fold]") {
  Fixture f;
  // A spilled pointer: the slot holds an address, later loaded back and used
  // only as where the next store writes, never read for its own value.
  f.function.appendStore(f.entry, 0x1000, Type::integer(64), f.slot(-0x20),
                         f.function.entryReg(f.reg("x1")));
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1004, Type::integer(64), f.slot(-0x20));
  f.function.appendStore(f.entry, 0x1008, Type::integer(32), f.function.valueRef(loaded),
                         f.function.entryReg(f.reg("x2")));
  f.function.appendReturn(f.entry, 0x100c);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto foldable = findFoldableStackLoads(f.function, frame, {});
  const auto found = foldable.find(1);
  REQUIRE(found != foldable.end());
  CHECK(found->second.usedAsAddress);
}

TEST_CASE("a load used once as a value and once as an address is not usedAsAddress",
          "[analysis][stack-load-fold]") {
  Fixture f;
  f.function.appendStore(f.entry, 0x1000, Type::integer(64), f.slot(-0x20),
                         f.function.entryReg(f.reg("x1")));
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1004, Type::integer(64), f.slot(-0x20));
  // An ordinary reader first -- the load's own value used in an addition,
  // stored to an address that provably cannot be the slot itself, so it
  // cannot cast doubt on the second reader's freshness.
  f.function.appendStore(f.entry, 0x1008, Type::integer(64), f.i64(0x9000),
                         f.function.binary(ExprOp::Add, f.function.valueRef(loaded), f.i64(8)));
  // A second reader that does use the load as an address, so usedAsAddress
  // has to notice the first reader disqualifies the "only ever an address"
  // claim rather than just looking at the last site.
  f.function.appendStore(f.entry, 0x100c, Type::integer(32), f.function.valueRef(loaded),
                         f.function.entryReg(f.reg("x2")));
  f.function.appendReturn(f.entry, 0x1010);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto foldable = findFoldableStackLoads(f.function, frame, {});
  const auto found = foldable.find(1);
  REQUIRE(found != foldable.end());
  CHECK(!found->second.usedAsAddress);
}
