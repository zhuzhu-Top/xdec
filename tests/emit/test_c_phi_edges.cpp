// Where phi copies land.
//
// A phi's incoming value belongs to one edge. Emitting it anywhere control can
// reach without taking that edge is a silent wrong answer, and emitting several
// copies of one edge in sequence is another, because a copy would then observe
// a sibling's write instead of the value from before the transfer.
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
  ExprId entryReg(std::string_view name) {
    return function.entryReg(function.registers().find(name));
  }

  std::string emit() {
    function.rebuildEdges();
    const StackFrame frame = StackFrame::compute(function);
    const VariableTable variables = VariableTable::recover(function, frame);
    const Dominators dominators = Dominators::compute(function);
    const PostDominators postDominators = PostDominators::compute(function);
    const std::vector<NaturalLoop> loops = naturalLoops(function, dominators);
    return printFunction(function, variables, frame,
                         structureFunction(function, dominators, postDominators,
                                           loops));
  }

  Function function;
  BlockId entry;
};

[[nodiscard]] bool contains(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

/// The order two snippets appear in, so a test can say "inside the then arm"
/// without depending on exact indentation.
[[nodiscard]] bool before(const std::string& text, std::string_view first,
                          std::string_view second) {
  const std::size_t a = text.find(first);
  const std::size_t b = text.find(second);
  return a != std::string::npos && b != std::string::npos && a < b;
}

[[nodiscard]] std::size_t count(const std::string& text, std::string_view needle) {
  std::size_t total = 0;
  for (std::size_t at = text.find(needle); at != std::string::npos;
       at = text.find(needle, at + 1)) {
    ++total;
  }
  return total;
}

TEST_CASE("each arm of a conditional carries only its own edge's copies",
          "[emit][phi]") {
  Fixture f;
  // Both successors of the head have a phi. Emitting both copies at the end of
  // the head would run the untaken edge's copy as well.
  const BlockId left = f.function.createBlock(0x2000);
  const BlockId right = f.function.createBlock(0x3000);
  const ExprId cond = f.function.binary(ExprOp::CmpNe, f.entryReg("x0"), f.i64(0));
  f.function.appendCondBranch(f.entry, 0x1000, cond, left, right);
  f.function.appendPhi(left, 0x2000, Type::integer(64),
                       std::vector<ExprId>{f.i64(0x11)});
  f.function.appendReturn(left, 0x2004);
  f.function.appendPhi(right, 0x3000, Type::integer(64),
                       std::vector<ExprId>{f.i64(0x22)});
  f.function.appendReturn(right, 0x3004);

  const std::string text = f.emit();
  INFO(text);
  REQUIRE(contains(text, "if ("));
  CHECK(count(text, "= 0x11;") == 1);
  CHECK(count(text, "= 0x22;") == 1);
  CHECK(before(text, "if (", "= 0x11;"));
  CHECK(before(text, "= 0x11;", "} else {"));
  CHECK(before(text, "} else {", "= 0x22;"));
}

TEST_CASE("an unconditional branch carries its copies at the end of the block",
          "[emit][phi]") {
  Fixture f;
  const BlockId next = f.function.createBlock(0x2000);
  f.function.appendStore(f.entry, 0x1000, Type::integer(64), f.i64(0x400000),
                         f.i64(7));
  f.function.appendBranch(f.entry, 0x1004, next);
  const il::OpId phi = f.function.appendPhi(next, 0x2000, Type::integer(64),
                                           std::vector<ExprId>{f.i64(0x33)});
  const il::OpId ret = f.function.appendReturn(next, 0x2004);
  f.function.setOperands(
      ret, std::vector<ExprId>{f.function.valueRef(f.function.op(phi).result)});

  const std::string text = f.emit();
  INFO(text);
  CHECK(before(text, "(*(uint64_t*)(0x400000)) = 0x7;", "= 0x33;"));
  CHECK(before(text, "= 0x33;", "return t"));
}

TEST_CASE("an empty if arm still carries the edge it stands for", "[emit][phi]") {
  Fixture f;
  // The taken arm goes straight to the merge, so the structurizer produces an
  // if with one arm. The other edge's copies have nowhere else to live.
  const BlockId body = f.function.createBlock(0x2000);
  const BlockId merge = f.function.createBlock(0x3000);
  const ExprId cond = f.function.binary(ExprOp::CmpNe, f.entryReg("x0"), f.i64(0));
  f.function.appendCondBranch(f.entry, 0x1000, cond, merge, body);
  f.function.appendBranch(body, 0x2000, merge);
  f.function.appendPhi(merge, 0x3000, Type::integer(64),
                       std::vector<ExprId>{f.i64(0x44), f.i64(0x55)});
  f.function.appendReturn(merge, 0x3004);

  const std::string text = f.emit();
  INFO(text);
  // One copy per edge, each on its own path, neither before the branch.
  CHECK(count(text, "= 0x44;") == 1);
  CHECK(count(text, "= 0x55;") == 1);
  CHECK(before(text, "if (", "= 0x44;"));
  CHECK(before(text, "if (", "= 0x55;"));
}

TEST_CASE("copies on one edge are parallel: a swap snapshots its source",
          "[emit][phi]") {
  Fixture f;
  // The classic shape: two phis whose incoming values are each other. In
  // sequence the second copy would read the first's result and both variables
  // would end up holding the same value.
  const BlockId head = f.function.createBlock(0x2000);
  f.function.appendBranch(f.entry, 0x1000, head);
  const il::OpId left = f.function.appendPhi(head, 0x2000, Type::integer(64),
                                             std::vector<ExprId>{f.i64(1)});
  const il::OpId right = f.function.appendPhi(head, 0x2004, Type::integer(64),
                                              std::vector<ExprId>{f.i64(2)});
  const ExprId leftRef = f.function.valueRef(f.function.op(left).result);
  const ExprId rightRef = f.function.valueRef(f.function.op(right).result);
  // Close the loop so the phis have a second incoming edge that swaps them.
  const BlockId latch = f.function.createBlock(0x2008);
  const ExprId cond = f.function.binary(ExprOp::CmpNe, leftRef, f.i64(0));
  f.function.appendCondBranch(head, 0x2008, cond, latch, f.entry);
  f.function.appendBranch(latch, 0x2010, head);
  f.function.setOperands(left, std::vector<ExprId>{f.i64(1), rightRef});
  f.function.setOperands(right, std::vector<ExprId>{f.i64(2), leftRef});

  const std::string text = f.emit();
  INFO(text);
  // t0 = t1 and t1 = t0 on the same edge: one of them must read a snapshot.
  CHECK(contains(text, "__prev"));
  CHECK(before(text, "__prev = t", "t0 = t"));
}

TEST_CASE("an edge that hands a phi back its own value carries nothing",
          "[emit][phi]") {
  Fixture f;
  // A loop whose latch leaves one of the two phis alone. The unchanged one
  // takes its own result back, and writing `t1 = t1` on the latch edge tells a
  // reader that something happens there when nothing does -- which matters
  // most on exactly the code that has most of these, a flattened dispatcher
  // whose state registers stay put across the majority of its edges.
  const BlockId head = f.function.createBlock(0x2000);
  f.function.appendBranch(f.entry, 0x1000, head);
  const il::OpId counter = f.function.appendPhi(head, 0x2000, Type::integer(64),
                                                std::vector<ExprId>{f.i64(1)});
  const il::OpId parked = f.function.appendPhi(head, 0x2004, Type::integer(64),
                                               std::vector<ExprId>{f.i64(0x77)});
  const ExprId counterRef = f.function.valueRef(f.function.op(counter).result);
  const ExprId parkedRef = f.function.valueRef(f.function.op(parked).result);
  const BlockId latch = f.function.createBlock(0x2008);
  const ExprId cond = f.function.binary(ExprOp::CmpNe, parkedRef, f.i64(0));
  f.function.appendCondBranch(head, 0x2008, cond, latch, f.entry);
  f.function.appendBranch(latch, 0x2010, head);
  f.function.setOperands(
      counter, std::vector<ExprId>{f.i64(1), f.function.binary(ExprOp::Add, counterRef,
                                                               f.i64(1))});
  f.function.setOperands(parked, std::vector<ExprId>{f.i64(0x77), parkedRef});

  const std::string text = f.emit();
  INFO(text);
  // The counter's own increment still crosses the edge; the parked value's
  // self-copy is gone.
  CHECK(contains(text, "+ 0x1)"));
  CHECK_FALSE(contains(text, "t1 = t1;"));
}

TEST_CASE("a dispatcher chain's cases carry their own edges", "[emit][phi]") {
  Fixture f;
  // The switch replaces the compare blocks, so each case is the only place the
  // edge into its handler exists.
  const BlockId test1 = f.function.createBlock(0x2000);
  const BlockId test2 = f.function.createBlock(0x2008);
  const BlockId test3 = f.function.createBlock(0x2010);
  const BlockId h1 = f.function.createBlock(0x3000);
  const BlockId h2 = f.function.createBlock(0x4000);
  const BlockId h3 = f.function.createBlock(0x5000);
  const BlockId fallthrough = f.function.createBlock(0x6000);
  f.function.appendBranch(f.entry, 0x1000, test1);
  const ExprId state = f.entryReg("x0");
  const auto compare = [&](BlockId block, uint64_t va, uint64_t value,
                           BlockId taken, BlockId next) {
    const ExprId cond = f.function.binary(ExprOp::CmpEq, state, f.i64(value));
    f.function.appendCondBranch(block, va, cond, taken, next);
  };
  compare(test1, 0x2000, 1, h1, test2);
  compare(test2, 0x2008, 2, h2, test3);
  compare(test3, 0x2010, 3, h3, fallthrough);
  f.function.appendPhi(h2, 0x4000, Type::integer(64),
                       std::vector<ExprId>{f.i64(0x66)});
  // The handlers return rather than reconverge, which is what makes this a
  // dispatcher and not a nest of diamonds.
  for (const BlockId handler : {h1, h2, h3}) {
    f.function.appendReturn(handler, f.function.block(handler).va);
  }
  f.function.appendReturn(fallthrough, 0x6000);

  const std::string text = f.emit();
  INFO(text);
  REQUIRE(contains(text, "switch ("));
  CHECK(count(text, "= 0x66;") == 1);
  CHECK(before(text, "case 0x2:", "= 0x66;"));
  // Each handler is reached only through its case, so it is written there, and the
  // phi copy has to come before the handler's own code rather than before a jump
  // to it: the copy is what the edge into the handler carries.
  CHECK(before(text, "= 0x66;", "@0x4000"));
  CHECK_FALSE(contains(text, "goto L_0x4000;"));
}

}  // namespace
