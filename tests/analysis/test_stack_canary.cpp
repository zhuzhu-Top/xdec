// findStackCanarySaves: the save/reload round trip that says "this local
// holds a stack-protector canary" without ever naming the guard address (see
// the header for why the second half is out of scope for this analysis).
#include <catch2/catch_test_macros.hpp>

#include "il/il_test_support.h"
#include "xdec/analysis/stack_canary.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::findStackCanarySaves;
using xdec::analysis::StackFrame;
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
  ExprId load(BlockId block, uint64_t va, ExprId address) {
    return function.valueRef(function.appendLoad(block, va, Type::integer(64), address));
  }

  Function function;
  BlockId entry;
};

}  // namespace

TEST_CASE("a save/reload round trip against the same global is recognised",
          "[analysis][stack-canary]") {
  Fixture f;
  const OpId save =
      f.function.appendStore(f.entry, 0x1000, Type::integer(64), f.slot(-0x10),
                             f.load(f.entry, 0x1000, f.i64(0x2000)));

  const BlockId ok = f.function.createBlock(0x1100);
  const BlockId fail = f.function.createBlock(0x1200);

  const ExprId reload = f.load(f.entry, 0x1004, f.i64(0x2000));
  const ExprId saved = f.load(f.entry, 0x1008, f.slot(-0x10));
  const ExprId cond = f.function.binary(ExprOp::CmpNe, reload, saved);
  const OpId check = f.function.appendCondBranch(f.entry, 0x100c, cond, fail, ok);

  f.function.appendReturn(ok, 0x1100);
  f.function.appendReturn(fail, 0x1200);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto saves = findStackCanarySaves(f.function, frame);
  REQUIRE(saves.size() == 1);
  CHECK(saves[0].save.index() == save.index());
  CHECK(saves[0].check.index() == check.index());
  CHECK(saves[0].guardAddress == 0x2000);
  CHECK(saves[0].savedDelta == -0x10);
}

TEST_CASE("the comparison's operand order does not matter", "[analysis][stack-canary]") {
  Fixture f;
  const OpId save =
      f.function.appendStore(f.entry, 0x1000, Type::integer(64), f.slot(-0x18),
                             f.load(f.entry, 0x1000, f.i64(0x3000)));

  const BlockId ok = f.function.createBlock(0x1100);
  const BlockId fail = f.function.createBlock(0x1200);

  // Saved value first, guard reload second: the mirror image of the other
  // test's operand order.
  const ExprId saved = f.load(f.entry, 0x1004, f.slot(-0x18));
  const ExprId reload = f.load(f.entry, 0x1008, f.i64(0x3000));
  const ExprId cond = f.function.binary(ExprOp::CmpEq, saved, reload);
  f.function.appendCondBranch(f.entry, 0x100c, cond, ok, fail);

  f.function.appendReturn(ok, 0x1100);
  f.function.appendReturn(fail, 0x1200);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto saves = findStackCanarySaves(f.function, frame);
  REQUIRE(saves.size() == 1);
  CHECK(saves[0].save.index() == save.index());
  CHECK(saves[0].guardAddress == 0x3000);
}

TEST_CASE("a compare against a different global than the one saved does not match",
          "[analysis][stack-canary]") {
  Fixture f;
  f.function.appendStore(f.entry, 0x1000, Type::integer(64), f.slot(-0x10),
                         f.load(f.entry, 0x1000, f.i64(0x2000)));

  const BlockId ok = f.function.createBlock(0x1100);
  const BlockId fail = f.function.createBlock(0x1200);

  // Reloads a *different* address than the one that was saved -- not the
  // stack-protector shape, and this analysis must not claim it is.
  const ExprId reload = f.load(f.entry, 0x1004, f.i64(0x9999));
  const ExprId saved = f.load(f.entry, 0x1008, f.slot(-0x10));
  const ExprId cond = f.function.binary(ExprOp::CmpNe, reload, saved);
  f.function.appendCondBranch(f.entry, 0x100c, cond, fail, ok);

  f.function.appendReturn(ok, 0x1100);
  f.function.appendReturn(fail, 0x1200);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  CHECK(findStackCanarySaves(f.function, frame).empty());
}

TEST_CASE("a save with no later compare is not reported", "[analysis][stack-canary]") {
  Fixture f;
  f.function.appendStore(f.entry, 0x1000, Type::integer(64), f.slot(-0x10),
                         f.load(f.entry, 0x1000, f.i64(0x2000)));
  f.function.appendReturn(f.entry, 0x1004);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  CHECK(findStackCanarySaves(f.function, frame).empty());
}

TEST_CASE("a compare between two ordinary locals is not a canary", "[analysis][stack-canary]") {
  Fixture f;
  const BlockId ok = f.function.createBlock(0x1100);
  const BlockId fail = f.function.createBlock(0x1200);

  // Both sides come off the stack -- no global address is read at all, so
  // there is nothing here that could be a guard round trip.
  const ExprId a = f.load(f.entry, 0x1000, f.slot(-0x10));
  const ExprId b = f.load(f.entry, 0x1004, f.slot(-0x18));
  const ExprId cond = f.function.binary(ExprOp::CmpNe, a, b);
  f.function.appendCondBranch(f.entry, 0x1008, cond, fail, ok);

  f.function.appendReturn(ok, 0x1100);
  f.function.appendReturn(fail, 0x1200);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  CHECK(findStackCanarySaves(f.function, frame).empty());
}
