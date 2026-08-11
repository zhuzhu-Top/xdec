// findFoldableMemoryLoads: the same shape findFoldableStackLoads targets,
// generalised to a Global or Other (e.g. argument-plus-offset) address (see
// the header for the full rule list, including why StackSlot loads stay
// stack_load_fold.cpp's own territory).
#include <catch2/catch_test_macros.hpp>

#include <unordered_set>

#include "il/il_test_support.h"
#include "xdec/analysis/load_inline.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::FoldableMemoryLoad;
using xdec::analysis::StackFrame;
using xdec::analysis::findFoldableMemoryLoads;
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
  ExprId argPlus(std::string_view name, int64_t offset) {
    const ExprId base = function.entryReg(reg(name));
    return offset == 0 ? base : function.binary(ExprOp::Add, base, i64(static_cast<uint64_t>(offset)));
  }

  Function function;
  BlockId entry;
};

}  // namespace

TEST_CASE("a global load read once right after it, nothing between, folds",
          "[analysis][load-inline]") {
  Fixture f;
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1000, Type::integer(32), f.i64(0x400900));
  f.function.appendStore(f.entry, 0x1004, Type::integer(32), f.i64(0x9000),
                         f.function.valueRef(loaded));
  f.function.appendReturn(f.entry, 0x1008);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto foldable = findFoldableMemoryLoads(f.function, frame, {});
  const auto found = foldable.find(0);
  REQUIRE(found != foldable.end());
  CHECK(found->second.width == 32);
}

TEST_CASE("an argument-plus-offset load read once right after it folds",
          "[analysis][load-inline]") {
  Fixture f;
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1000, Type::integer(32), f.argPlus("x0", 0x18));
  f.function.appendStore(f.entry, 0x1004, Type::integer(32), f.i64(0x9000),
                         f.function.valueRef(loaded));
  f.function.appendReturn(f.entry, 0x1008);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto foldable = findFoldableMemoryLoads(f.function, frame, {});
  CHECK(foldable.find(0) != foldable.end());
}

TEST_CASE("a stack-slot load is left to stack_load_fold, not this analysis",
          "[analysis][load-inline]") {
  Fixture f;
  const ExprId sp = f.function.entryReg(f.reg("sp"));
  const ExprId slot = f.function.binary(ExprOp::Sub, sp, f.i64(0x10));
  f.function.appendStore(f.entry, 0x1000, Type::integer(32), slot,
                         f.function.entryReg(f.reg("x0")));
  const il::ValueId loaded = f.function.appendLoad(f.entry, 0x1004, Type::integer(32), slot);
  f.function.appendStore(f.entry, 0x1008, Type::integer(32), f.i64(0x9000),
                         f.function.valueRef(loaded));
  f.function.appendReturn(f.entry, 0x100c);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto foldable = findFoldableMemoryLoads(f.function, frame, {});
  CHECK(foldable.empty());
}

TEST_CASE("a call between the load and its use blocks the fold", "[analysis][load-inline]") {
  Fixture f;
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1000, Type::integer(32), f.i64(0x400900));
  f.function.appendCall(f.entry, 0x1004, f.i64(0x8000));
  f.function.appendStore(f.entry, 0x1008, Type::integer(32), f.i64(0x9000),
                         f.function.valueRef(loaded));
  f.function.appendReturn(f.entry, 0x100c);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto foldable = findFoldableMemoryLoads(f.function, frame, {});
  CHECK(foldable.empty());
}

TEST_CASE("a use in a different block blocks the fold", "[analysis][load-inline]") {
  Fixture f;
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1000, Type::integer(32), f.i64(0x400900));
  const BlockId other = f.function.createBlock(0x2000);
  f.function.appendBranch(f.entry, 0x1004, other);
  f.function.appendStore(other, 0x2000, Type::integer(32), f.i64(0x9000),
                         f.function.valueRef(loaded));
  f.function.appendReturn(other, 0x2004);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto foldable = findFoldableMemoryLoads(f.function, frame, {});
  CHECK(foldable.empty());
}

TEST_CASE("a load consumed as another access's address is not folded",
          "[analysis][load-inline]") {
  Fixture f;
  // The classic pointer-chain shape: `next = *(a0+8); v = *(next);` -- fieldAccess
  // (c_context.cpp) recognises this chain by name, which requires `next` to
  // keep its own printed temporary rather than being inlined away.
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1000, Type::integer(64), f.argPlus("x0", 8));
  f.function.appendLoad(f.entry, 0x1004, Type::integer(32), f.function.valueRef(loaded));
  f.function.appendReturn(f.entry, 0x1008);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto foldable = findFoldableMemoryLoads(f.function, frame, {});
  CHECK(foldable.empty());
}

TEST_CASE("a load used once as a value and once as an address is not folded",
          "[analysis][load-inline]") {
  Fixture f;
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1000, Type::integer(64), f.argPlus("x0", 8));
  f.function.appendStore(f.entry, 0x1004, Type::integer(64), f.i64(0x9000),
                         f.function.binary(ExprOp::Add, f.function.valueRef(loaded), f.i64(8)));
  f.function.appendLoad(f.entry, 0x1008, Type::integer(32), f.function.valueRef(loaded));
  f.function.appendReturn(f.entry, 0x100c);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto foldable = findFoldableMemoryLoads(f.function, frame, {});
  CHECK(foldable.empty());
}

TEST_CASE("a load already dead does not stop the fold, its use elsewhere still counts",
          "[analysis][load-inline]") {
  Fixture f;
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1000, Type::integer(32), f.i64(0x400900));
  const il::OpId deadSink = f.function.appendStore(f.entry, 0x1004, Type::integer(32),
                                                   f.i64(0x9000), f.function.valueRef(loaded));
  f.function.appendStore(f.entry, 0x1008, Type::integer(32), f.i64(0x9008),
                         f.function.valueRef(loaded));
  f.function.appendReturn(f.entry, 0x100c);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const std::unordered_set<uint32_t> deadOps{deadSink.index()};
  const auto foldable = findFoldableMemoryLoads(f.function, frame, deadOps);
  CHECK(foldable.find(0) != foldable.end());
}
