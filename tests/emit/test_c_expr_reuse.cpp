// Emit-level regression for the Block+Switch CSE scope merge (expression-reuse
// plan, Phase 1): a resolved computed branch's own block and the switch built
// from its terminator print from one shared scope, so a value both use is
// materialized once instead of once per scope. See docs/09-expression-reuse.md.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "il/il_test_support.h"
#include "xdec/analysis/dominators.h"
#include "xdec/analysis/loops.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/analysis/variables.h"
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
    "a dispatcher state stored to a slot and used as its own resolved switch "
    "index materializes once, not once per scope",
    "[emit][expr-reuse]") {
  Fixture f;
  // flag = entry(x0) + 0x1000: the shape a flattened dispatcher's state
  // update takes, real enough to be worth a name (a bare EntryReg would not
  // be -- see ExprPrinter::isShared).
  const ExprId flag = f.function.binary(ExprOp::Add, f.entryReg("x0"), f.i64(0x1000));
  f.function.appendStore(f.entry, 0x1000, Type::integer(64), f.slot(-0x10), flag);
  // switch (load64(table + flag*8)): flag is the table's index, reached a
  // second time only as a subexpression of the branch's own target address --
  // exactly sub_199214's shape, not a hand-picked shortcut.
  const ExprId address = f.function.binary(
      ExprOp::Add, f.i64(0x30b7f0), f.function.binary(ExprOp::Shl, flag, f.i64(3)));
  const il::ValueId loaded = f.function.appendLoad(f.entry, 0x1004, Type::integer(64), address);
  const BlockId target0 = f.block(0x2000);
  const BlockId target1 = f.block(0x3000);
  const BlockId target2 = f.block(0x4000);
  const il::OpId brind =
      f.function.appendIndirectBranch(f.entry, 0x1008, f.function.valueRef(loaded));
  f.function.setTargets(brind, std::vector<BlockId>{target0, target1, target2});
  // A real dispatcher loop closes back around and reads its state slot again
  // at the top of the next iteration; model that one loop-carried read (here
  // simply forwarded to a global) so the state store stays observable --
  // otherwise findDeadStackStores (correctly) folds the store away, leaving
  // only one real reference to `flag` and nothing left to materialize.
  const il::ValueId reread =
      f.function.appendLoad(target0, 0x2000, Type::integer(64), f.slot(-0x10));
  f.function.appendStore(target0, 0x2004, Type::integer(64), f.i64(0x9000),
                         f.function.valueRef(reread));
  f.function.appendReturn(target0, 0x2008);
  f.function.appendReturn(target1, 0x3000);
  f.function.appendReturn(target2, 0x4000);

  const std::string text = f.emit();
  INFO(text);
  // `flag` is shared across the whole function (the store, the switch's own
  // discriminant, and the reread in target0), so it still materializes
  // exactly once regardless of scope -- but as of the H2 store/CSE merge
  // (docs/09, docs/14 Phase 4) that one materialization IS the store into
  // `var_10` itself, not a separate `_cse0` the store then copies from: one
  // assignment, computed once, and every other reference (the switch, the
  // reread) just reads `var_10` back rather than introducing a `_cseN`.
  CHECK(occurrences(text, "(arg1 + 0x1000)") == 1);
  CHECK(occurrences(text, "var_10 = (arg1 + 0x1000);") == 1);
  CHECK(occurrences(text, "switch (var_10)") == 1);
  CHECK(occurrences(text, "_cse") == 0);
}

TEST_CASE(
    "a shared value stored through a redundant root ZExt still materializes "
    "under its own name, not its operand's",
    "[emit][expr-reuse]") {
  Fixture f;
  // bit = (x0 == 0); z32 = zext.i32(bit); z64 = zext.i64(z32). z64 is exactly
  // the shape rootText/rootInteger special-case (its own operand z32 is
  // already a plain scalar integer, so a root use may drop z64's cast) --
  // but z64 itself, not z32, is what a sibling Select re-reads below, so z64
  // is the node that must end up shared, not silently bypassed in favour of
  // an unshared z32 (see ExprPrinter::rootText's own note on why).
  const ExprId bit = f.function.binary(ExprOp::CmpEq, f.entryReg("x0"), f.i64(0));
  const ExprId z32 = f.function.cast(ExprOp::ZExt, Type::integer(32), bit);
  const ExprId z64 = f.function.cast(ExprOp::ZExt, Type::integer(64), z32);
  f.function.appendStore(f.entry, 0x1000, Type::integer(64), f.slot(-0x10), z64);
  // index = (3 < z64) ? 2 : z64 -- z64 reached twice more here, exactly
  // sub_199214's clamp-then-dispatch shape.
  const ExprId clamp = f.function.binary(ExprOp::CmpLtS, f.i64(3), z64);
  const ExprId index = f.function.select(clamp, f.i64(2), z64);
  const ExprId address =
      f.function.binary(ExprOp::Add, f.i64(0x30b7f0), f.function.binary(ExprOp::Shl, index, f.i64(3)));
  const il::ValueId loaded = f.function.appendLoad(f.entry, 0x1004, Type::integer(64), address);
  const BlockId target0 = f.block(0x2000);
  const BlockId target1 = f.block(0x3000);
  const BlockId target2 = f.block(0x4000);
  const il::OpId brind =
      f.function.appendIndirectBranch(f.entry, 0x1008, f.function.valueRef(loaded));
  f.function.setTargets(brind, std::vector<BlockId>{target0, target1, target2});
  f.function.appendReturn(target0, 0x2000);
  f.function.appendReturn(target1, 0x3000);
  f.function.appendReturn(target2, 0x4000);

  const std::string text = f.emit();
  INFO(text);
  // z64 is referenced from the store, the clamp compare, and the select's
  // else arm: shared, so it gets exactly one _cseN, declared once, and every
  // one of those three sites names it -- none re-expands the "== 0x0"
  // comparison a second time.
  CHECK(occurrences(text, "== 0x0") == 1);
  CHECK(occurrences(text, "_cse0 = ") == 1);
}

TEST_CASE(
    "a resolved computed branch's own jump-table read is not printed once "
    "the switch dispatches on the index alone",
    "[emit][expr-reuse]") {
  Fixture f;
  // Same dispatcher shape as the Block+Switch scope-merge case above, but
  // this time the branch actually resolves to a jump table (matchJumpTable
  // needs every target reachable through consecutive table slots), so the
  // structurizer builds a table-mode switch and the Load that computed the
  // branch's original target has nothing left to feed.
  const ExprId flag = f.function.binary(ExprOp::And, f.entryReg("x0"), f.i64(0x3));
  const ExprId address = f.function.binary(
      ExprOp::Add, f.i64(0x30b7f0), f.function.binary(ExprOp::Shl, flag, f.i64(3)));
  const il::ValueId loaded = f.function.appendLoad(f.entry, 0x1004, Type::integer(64), address);
  const BlockId target0 = f.block(0x2000);
  const BlockId target1 = f.block(0x3000);
  const il::OpId brind =
      f.function.appendIndirectBranch(f.entry, 0x1008, f.function.valueRef(loaded));
  f.function.setTargets(brind, std::vector<BlockId>{target0, target1});
  f.function.appendReturn(target0, 0x2000);
  f.function.appendReturn(target1, 0x3000);

  const std::string text = f.emit();
  INFO(text);
  // The load that fed the now-resolved branch prints nowhere: no `tN`
  // declaration and no `tN = ...;` assignment reading the jump table.
  CHECK(occurrences(text, "0x30b7f0") == 0);
  CHECK(occurrences(text, "uint64_t t0;") == 0);
  CHECK(occurrences(text, "switch (") == 1);
}
