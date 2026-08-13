// J3 (docs/architecture-optimization-eval-prompt.md §6.6): an obfuscator's
// routing decision sometimes compiles twice -- a `state=A:B` store that only
// ever feeds a `switch` on that same local elsewhere, and a genuine compare
// on the exact same condition right after it whose own arms just goto
// straight to the two targets that value would have dispatched to anyway
// (`quantify_c.py`'s `duplicate-routing-if`). These tests exercise
// `deadRoutingStateStore` end to end through the real printer.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "il/il_test_support.h"
#include "xdec/analysis/dominators.h"
#include "xdec/analysis/loops.h"
#include "xdec/emit/c_printer.h"
#include "xdec/emit/structure.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::Dominators;
using xdec::analysis::NaturalLoop;
using xdec::analysis::PostDominators;
using xdec::analysis::StackFrame;
using xdec::analysis::VariableTable;
using xdec::emit::printFunction;
using xdec::emit::structureFunction;
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
  ExprId slot(int64_t delta) {
    const ExprId sp = function.entryReg(function.registers().find("sp"));
    return delta < 0
               ? function.binary(ExprOp::Sub, sp, i64(static_cast<uint64_t>(-delta)))
               : function.binary(ExprOp::Add, sp, i64(static_cast<uint64_t>(delta)));
  }
  ExprId entryReg(std::string_view name) {
    return function.entryReg(function.registers().find(name));
  }
  BlockId block(uint64_t va) { return function.createBlock(va); }

  std::string emit() {
    function.rebuildEdges();
    const StackFrame frame = StackFrame::compute(function);
    const VariableTable variables = VariableTable::recover(function, frame);
    const Dominators dominators = Dominators::compute(function);
    const PostDominators postDominators = PostDominators::compute(function);
    const std::vector<NaturalLoop> loops = naturalLoops(function, dominators);
    return printFunction(function, variables, frame,
                         structureFunction(function, dominators, postDominators, loops));
  }

  Function function;
  BlockId entry;
};

[[nodiscard]] std::size_t occurrences(const std::string& text, std::string_view needle) {
  std::size_t count = 0;
  std::size_t pos = 0;
  while ((pos = text.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

}  // namespace

TEST_CASE(
    "a routing state store duplicated by a real compare on the same "
    "condition, both arms bare gotos, is dropped rather than printed twice",
    "[emit][dead-ops][routing-if]") {
  Fixture f;
  const ExprId cond = f.function.binary(ExprOp::CmpEq, f.entryReg("x0"), f.i64(0));
  f.function.appendStore(f.entry, 0x1000, Type::integer(64), f.slot(-0x10),
                         f.function.select(cond, f.i64(0xf), f.i64(0xd)));
  const BlockId target0 = f.block(0x2000);
  const BlockId target1 = f.block(0x3000);
  f.function.appendCondBranch(f.entry, 0x1004, cond, target0, target1);
  f.function.appendReturn(target0, 0x2000);
  f.function.appendReturn(target1, 0x3000);
  // Each target gets a second, unrelated predecessor -- exactly the shape
  // "a handler shared with a non-table predecessor still falls back to
  // goto" (test_structure_dispatch_region.cpp) uses to disqualify a target
  // from being inlined: a dangling block never itself reached from `entry`,
  // whose only purpose is the edge it leaves in the predecessor list, so
  // `entry`'s branch has nothing left to try but `gotoChain`'s bare-goto
  // pair -- the only shape this fold is allowed to fire under.
  f.function.appendBranch(f.block(0x9000), 0x9000, target0);
  f.function.appendBranch(f.block(0x9010), 0x9010, target1);

    const std::string text = f.emit();
    INFO(text);
    CHECK(occurrences(text, "= 0xf;") == 0);
    CHECK(occurrences(text, "= 0xd;") == 0);
    // One target stays a `goto` leaf; the other, being the entry block's
    // natural layout successor, is reached by plain fallthrough instead of
    // a redundant `goto` -- either way, both arms leave directly and the
    // store this fold targets is gone.
    CHECK(occurrences(text, "goto") >= 1);
  }

TEST_CASE(
    "a routing state store is kept when something between it and the "
    "matching goto still reads the local",
    "[emit][dead-ops][routing-if]") {
  Fixture f;
  const ExprId cond = f.function.binary(ExprOp::CmpEq, f.entryReg("x0"), f.i64(0));
  f.function.appendStore(f.entry, 0x1000, Type::integer(64), f.slot(-0x10),
                         f.function.select(cond, f.i64(0xf), f.i64(0xd)));
  // A read of the same slot, right after the write this fold would
  // otherwise drop -- `deadRoutingStateStore` must see it and decline.
  const il::ValueId reread =
      f.function.appendLoad(f.entry, 0x1002, Type::integer(64), f.slot(-0x10));
  f.function.appendStore(f.entry, 0x1003, Type::integer(64), f.i64(0x9000),
                         f.function.valueRef(reread));
  const BlockId target0 = f.block(0x2000);
  const BlockId target1 = f.block(0x3000);
  f.function.appendCondBranch(f.entry, 0x1004, cond, target0, target1);
  f.function.appendReturn(target0, 0x2000);
  f.function.appendReturn(target1, 0x3000);
  f.function.appendBranch(f.block(0x9000), 0x9000, target0);
  f.function.appendBranch(f.block(0x9010), 0x9010, target1);

  const std::string text = f.emit();
  INFO(text);
  CHECK(occurrences(text, "0xf") >= 1);
  CHECK(occurrences(text, "0xd") >= 1);
}

TEST_CASE(
    "a routing state store is kept when the paired branch's arms are not "
    "both bare gotos",
    "[emit][dead-ops][routing-if]") {
  Fixture f;
  const ExprId cond = f.function.binary(ExprOp::CmpEq, f.entryReg("x0"), f.i64(0));
  f.function.appendStore(f.entry, 0x1000, Type::integer(64), f.slot(-0x10),
                         f.function.select(cond, f.i64(0xf), f.i64(0xd)));
  const BlockId target0 = f.block(0x2000);
  const BlockId target1 = f.block(0x3000);
  f.function.appendCondBranch(f.entry, 0x1004, cond, target0, target1);
  // A downstream reread, forwarded to a global exactly as
  // test_c_expr_reuse.cpp's fixture does: keeps the whole store observable
  // to the unrelated whole-function findDeadStackStores pass, so this test
  // exercises `deadRoutingStateStore`'s own decline, not that pass's.
  const il::ValueId reread =
      f.function.appendLoad(target0, 0x2000, Type::integer(64), f.slot(-0x10));
  f.function.appendStore(target0, 0x2004, Type::integer(64), f.i64(0x9000),
                         f.function.valueRef(reread));
  f.function.appendReturn(target0, 0x2008);
  f.function.appendReturn(target1, 0x3000);
  // Only `target1` gets a disqualifying second predecessor: `target0` stays
  // exclusively claimable, so the branch is a one-sided if (an inlined arm,
  // not a bare goto on both sides) rather than a `gotoChain`.
  f.function.appendBranch(f.block(0x9010), 0x9010, target1);

  const std::string text = f.emit();
  INFO(text);
  CHECK(occurrences(text, "0xf") >= 1);
  CHECK(occurrences(text, "0xd") >= 1);
}
