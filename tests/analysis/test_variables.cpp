// VariableTable: arguments from entry leaves, locals from stack slots,
// temps from phis, and the pointer/signedness refinements.
#include <catch2/catch_test_macros.hpp>

#include "il/il_test_support.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/analysis/variables.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::StackFrame;
using xdec::analysis::Variable;
using xdec::analysis::VariableTable;
using xdec::analysis::VarKind;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
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
    return delta < 0
               ? function.binary(ExprOp::Sub, sp, i64(static_cast<uint64_t>(-delta)))
               : function.binary(ExprOp::Add, sp, i64(static_cast<uint64_t>(delta)));
  }

  /// A return carrying a value, the way SSA construction leaves one: the value
  /// is annotated onto the op rather than passed at creation.
  void returnValue(BlockId block, uint64_t va, ExprId value) {
    const il::OpId op = function.appendReturn(block, va);
    const std::vector<ExprId> operands{value};
    function.setOperands(op, operands);
  }

  Function function;
  BlockId entry;
};

TEST_CASE("arguments come from entry leaves, one per register", "[analysis][vars]") {
  Fixture f;
  // x0 and x2 are used; x1 is not, so there is no a1.
  const ExprId x0 = f.function.entryReg(f.reg("x0"));
  const ExprId x2 = f.function.entryReg(f.reg("x2"));
  f.function.appendStore(f.entry, 0x1000, Type::integer(64), f.i64(0x9000), x0);
  f.function.appendStore(f.entry, 0x1004, Type::integer(64), f.i64(0x9008), x2);
  // A sub-register's view shares the root's argument: w2 is x2's low half.
  const ExprId w2View = f.function.cast(ExprOp::Trunc, Type::integer(32),
                                        f.function.entryReg(f.reg("x2")));
  f.function.appendStore(f.entry, 0x1008, Type::integer(32), f.i64(0x9010), w2View);

  const StackFrame frame = StackFrame::compute(f.function);
  const VariableTable table = VariableTable::recover(f.function, frame);
  REQUIRE(table.arguments().size() == 2);
  const Variable* a0 = table.argumentFor(f.reg("x0"));
  const Variable* a2 = table.argumentFor(f.reg("x2"));
  REQUIRE(a0 != nullptr);
  REQUIRE(a2 != nullptr);
  CHECK(a0->name == "a0");
  CHECK(a2->name == "a2");
  CHECK(a0->type.width == 64);
  CHECK(a0->type.pointerDepth == 0);
  // 64-bit values default unsigned until a signed use says otherwise.
  CHECK(!a0->type.isSigned);
  CHECK(table.argumentFor(f.reg("x1")) == nullptr);
}

TEST_CASE("locals are one variable per slot, widest access wins", "[analysis][vars]") {
  Fixture f;
  const ExprId zero32 = f.function.constant(Type::integer(32), 0);
  const ExprId one64 = f.i64(1);
  // Two slots: an i32 at -0x10, an i64 at -0x18. A narrower later access to
  // -0x18 does not shrink it.
  f.function.appendStore(f.entry, 0x1000, Type::integer(32), f.slot(-0x10), zero32);
  f.function.appendStore(f.entry, 0x1004, Type::integer(64), f.slot(-0x18), one64);
  f.function.appendStore(f.entry, 0x1008, Type::integer(8), f.slot(-0x18), one64);
  // A positive delta is a stack-passed incoming argument.
  f.function.appendStore(f.entry, 0x100c, Type::integer(64), f.slot(0x20), one64);

  const StackFrame frame = StackFrame::compute(f.function);
  const VariableTable table = VariableTable::recover(f.function, frame);
  REQUIRE(table.locals().size() == 3);
  const Variable* narrow = table.localAt(-0x10);
  const Variable* wide = table.localAt(-0x18);
  const Variable* incoming = table.localAt(0x20);
  REQUIRE(narrow != nullptr);
  REQUIRE(wide != nullptr);
  REQUIRE(incoming != nullptr);
  CHECK(narrow->name == "var_10");
  CHECK(narrow->type.width == 32);
  CHECK(wide->name == "var_18");
  CHECK(wide->type.width == 64);
  CHECK(incoming->name == "starg_20");
}

TEST_CASE("an argument used as an address base becomes a pointer",
          "[analysis][vars]") {
  Fixture f;
  const ExprId x1 = f.function.entryReg(f.reg("x1"));
  const ExprId address =
      f.function.binary(ExprOp::Add, x1, f.i64(0x18));  // a1 + 0x18
  f.function.appendLoad(f.entry, 0x1000, Type::integer(32), address);

  const StackFrame frame = StackFrame::compute(f.function);
  const VariableTable table = VariableTable::recover(f.function, frame);
  const Variable* a1 = table.argumentFor(f.reg("x1"));
  REQUIRE(a1 != nullptr);
  CHECK(a1->type.pointerDepth == 1);
  CHECK(a1->type.pointeeWidth == 32);
  CHECK(a1->type.format() == "uint32_t*");
}

TEST_CASE("signed operations promote the leaf's variable", "[analysis][vars]") {
  Fixture f;
  const ExprId x0 = f.function.entryReg(f.reg("x0"));
  const ExprId cond = f.function.binary(ExprOp::CmpLtS, x0, f.i64(0));
  const BlockId other = f.function.createBlock(0x2000);
  f.function.appendCondBranch(f.entry, 0x1000, cond, other, other);
  f.function.appendReturn(other, 0x2000);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const VariableTable table = VariableTable::recover(f.function, frame);
  const Variable* a0 = table.argumentFor(f.reg("x0"));
  REQUIRE(a0 != nullptr);
  CHECK(a0->type.isSigned);
  CHECK(a0->type.format() == "int64_t");
}

TEST_CASE("an argument read only through a truncation is that narrow",
          "[analysis][vars]") {
  Fixture f;
  // The shape of a 32-bit parameter on AArch64: the body reads `w0`, never `x0`.
  const ExprId w0 = f.function.cast(ExprOp::Trunc, Type::integer(32),
                                    f.function.entryReg(f.reg("x0")));
  f.function.appendStore(f.entry, 0x1000, Type::integer(32), f.i64(0x9000), w0);

  const StackFrame frame = StackFrame::compute(f.function);
  const VariableTable table = VariableTable::recover(f.function, frame);
  const Variable* a0 = table.argumentFor(f.reg("x0"));
  REQUIRE(a0 != nullptr);
  CHECK(a0->type.width == 32);
  CHECK(a0->type.format() == "uint32_t");
}

TEST_CASE("an argument read at its full width stays wide", "[analysis][vars]") {
  Fixture f;
  const ExprId x0 = f.function.entryReg(f.reg("x0"));
  const ExprId narrow = f.function.cast(ExprOp::Trunc, Type::integer(32), x0);
  // Read both ways: the wide read is the one that has to be satisfied.
  f.function.appendStore(f.entry, 0x1000, Type::integer(32), f.i64(0x9000), narrow);
  f.function.appendStore(f.entry, 0x1004, Type::integer(64), f.i64(0x9008), x0);

  const StackFrame frame = StackFrame::compute(f.function);
  const VariableTable table = VariableTable::recover(f.function, frame);
  const Variable* a0 = table.argumentFor(f.reg("x0"));
  REQUIRE(a0 != nullptr);
  CHECK(a0->type.width == 64);
}

TEST_CASE("an argument that only survives into a merge is as wide as the merge",
          "[analysis][vars]") {
  Fixture f;
  // A state-machine parameter: x0 is never touched directly, only merged with the
  // next state, and every reader of the merge truncates to 32 bits.
  const BlockId header = f.function.createBlock(0x2000);
  const BlockId exit = f.function.createBlock(0x3000);
  f.function.appendBranch(f.entry, 0x1000, header);
  const il::OpId phiOp = f.function.appendPhi(
      header, 0x2000, Type::integer(64),
      std::vector<il::ExprId>{f.function.entryReg(f.reg("x0")), f.i64(3)});
  const ExprId state = f.function.valueRef(f.function.op(phiOp).result);
  const ExprId narrow = f.function.cast(ExprOp::Trunc, Type::integer(32), state);
  const ExprId cond =
      f.function.binary(ExprOp::CmpEq, narrow, f.function.constant(Type::integer(32), 0));
  f.function.appendCondBranch(header, 0x2004, cond, exit, header);
  f.function.appendReturn(exit, 0x3000);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const VariableTable table = VariableTable::recover(f.function, frame);
  const Variable* a0 = table.argumentFor(f.reg("x0"));
  REQUIRE(a0 != nullptr);
  CHECK(a0->type.width == 32);
}

TEST_CASE("a merge chain wide enough to be a dispatcher's still resolves",
          "[analysis][vars]") {
  Fixture f;
  // The shape a resolved dispatcher leaves behind: one phi per live register at
  // the merge point with an operand per case, most of those operands reading the
  // same value, and the phis chained because the cases merge again. Following
  // every operand separately makes the walk this fan-out raised to the chain's
  // length, which on the real thing does not finish; the recovered width has to
  // come out of a walk that visits each merge once.
  constexpr unsigned kCases = 16;
  constexpr unsigned kDepth = 9;  // deeper than the old depth cap, on purpose
  BlockId previous = f.entry;
  ExprId carried = f.function.entryReg(f.reg("x0"));
  for (unsigned level = 0; level < kDepth; ++level) {
    const BlockId merge = f.function.createBlock(0x2000 + level * 0x100);
    f.function.appendBranch(previous, 0x1000 + level * 0x100, merge);
    const std::vector<ExprId> incoming(kCases, carried);
    const il::OpId phiOp = f.function.appendPhi(merge, 0x2000 + level * 0x100,
                                                Type::integer(64), incoming);
    carried = f.function.valueRef(f.function.op(phiOp).result);
    previous = merge;
  }
  // The only reader of the last merge truncates, which is the fact that has to
  // travel back up the chain to the argument.
  const ExprId narrow = f.function.cast(ExprOp::Trunc, Type::integer(32), carried);
  f.returnValue(previous, 0x9000, narrow);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const VariableTable table = VariableTable::recover(f.function, frame);
  const Variable* a0 = table.argumentFor(f.reg("x0"));
  REQUIRE(a0 != nullptr);
  CHECK(a0->type.width == 32);
}

TEST_CASE("the return type looks through the convention's widening",
          "[analysis][vars]") {
  Fixture f;
  // AAPCS64 hands a 32-bit result back in w0 and leaves the rest of x0
  // unspecified, which the IL models as a zero-extension to the register width.
  const ExprId w0 = f.function.cast(ExprOp::Trunc, Type::integer(32),
                                    f.function.entryReg(f.reg("x0")));
  const ExprId sum =
      f.function.binary(ExprOp::Add, w0, f.function.constant(Type::integer(32), 1));
  const ExprId widened = f.function.cast(ExprOp::ZExt, Type::integer(64), sum);
  f.returnValue(f.entry, 0x1000, widened);

  const StackFrame frame = StackFrame::compute(f.function);
  const VariableTable table = VariableTable::recover(f.function, frame);
  REQUIRE(table.returnType().has_value());
  CHECK(table.returnType()->width == 32);
  CHECK(table.returnType()->format() == "uint32_t");
}

TEST_CASE("a path returning a literal does not widen the return type",
          "[analysis][vars]") {
  Fixture f;
  // `return 0` fits whatever the other paths need, so it must not be read as a
  // full-register return that drags the prototype back out to 64 bits.
  const BlockId wide = f.function.createBlock(0x2000);
  const BlockId zero = f.function.createBlock(0x3000);
  const ExprId w0 = f.function.cast(ExprOp::Trunc, Type::integer(32),
                                    f.function.entryReg(f.reg("x0")));
  const ExprId cond =
      f.function.binary(ExprOp::CmpEq, w0, f.function.constant(Type::integer(32), 0));
  f.function.appendCondBranch(f.entry, 0x1000, cond, zero, wide);
  f.returnValue(wide, 0x2000, f.function.cast(ExprOp::ZExt, Type::integer(64), w0));
  f.returnValue(zero, 0x3000, f.i64(0));
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const VariableTable table = VariableTable::recover(f.function, frame);
  REQUIRE(table.returnType().has_value());
  CHECK(table.returnType()->width == 32);
}

TEST_CASE("a function whose returns carry nothing returns void", "[analysis][vars]") {
  Fixture f;
  f.function.appendReturn(f.entry, 0x1000);

  const StackFrame frame = StackFrame::compute(f.function);
  const VariableTable table = VariableTable::recover(f.function, frame);
  CHECK(!table.returnType().has_value());
}

TEST_CASE("a pointer flowing through a phi types the argument it started at",
          "[analysis][vars]") {
  Fixture f;
  // An array walk after SSA: the parameter is never dereferenced, only the phi
  // that starts at it and advances by the element size.
  const BlockId header = f.function.createBlock(0x2000);
  const BlockId exit = f.function.createBlock(0x3000);
  f.function.appendBranch(f.entry, 0x1000, header);
  const il::OpId phiOp = f.function.appendPhi(
      header, 0x2000, Type::integer(64),
      std::vector<il::ExprId>{f.function.entryReg(f.reg("x0")), f.i64(0)});
  const ExprId cursor = f.function.valueRef(f.function.op(phiOp).result);
  f.function.appendLoad(header, 0x2004, Type::integer(32), cursor);
  const std::vector<ExprId> incoming{f.function.entryReg(f.reg("x0")),
                                     f.function.binary(ExprOp::Add, cursor, f.i64(4))};
  f.function.setOperands(phiOp, incoming);
  f.function.appendCondBranch(header, 0x2008,
                              f.function.binary(ExprOp::CmpNe, cursor, f.i64(0)),
                              header, exit);
  f.function.appendReturn(exit, 0x3000);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const VariableTable table = VariableTable::recover(f.function, frame);
  const Variable* a0 = table.argumentFor(f.reg("x0"));
  REQUIRE(a0 != nullptr);
  CHECK(a0->type.pointerDepth == 1);
  CHECK(a0->type.format() == "uint32_t*");
}

TEST_CASE("phis become temps in block order", "[analysis][vars]") {
  Fixture f;
  const BlockId left = f.function.createBlock(0x2000);
  const BlockId right = f.function.createBlock(0x3000);
  const BlockId join = f.function.createBlock(0x4000);
  const ExprId cond = f.function.binary(
      ExprOp::CmpNe, f.function.entryReg(f.reg("x0")), f.i64(0));
  f.function.appendCondBranch(f.entry, 0x1000, cond, left, right);
  f.function.appendBranch(left, 0x2000, join);
  f.function.appendBranch(right, 0x3000, join);
  const ExprId one = f.i64(1);
  const ExprId two = f.i64(2);
  const il::OpId phiOp = f.function.appendPhi(join, 0x4000, Type::integer(64),
                                              std::vector<il::ExprId>{one, two});
  const il::ValueId phi = f.function.op(phiOp).result;
  f.function.appendReturn(join, 0x4004);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const VariableTable table = VariableTable::recover(f.function, frame);
  REQUIRE(table.temps().size() == 1);
  const Variable* temp = table.tempFor(phi);
  REQUIRE(temp != nullptr);
  CHECK(temp->kind == VarKind::Temp);
  CHECK(temp->name == "t0");
  CHECK(temp->type.width == 64);
}

}  // namespace
