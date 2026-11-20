// findDeadStackStores: the safety rules that decide when a Store through a
// stack slot is provably unobservable (see the header for the full rule
// list).
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

#include "il/il_test_support.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/analysis/stack_store_fold.h"
#include "xdec/analysis/variables.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::StackFrame;
using xdec::analysis::VariableTable;
using xdec::analysis::findDeadStackStores;
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

TEST_CASE("a store to a slot nothing ever reads is dead", "[analysis][stack-store-fold]") {
  Fixture f;
  const OpId store = f.function.appendStore(f.entry, 0x1000, Type::integer(32), f.slot(-0x10),
                                            f.function.entryReg(f.reg("x0")));
  f.function.appendReturn(f.entry, 0x1004);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto dead = findDeadStackStores(f.function, frame, VariableTable{});
  CHECK(dead.contains(store.index()));
}

TEST_CASE("a store to a slot a load later reads is not dead", "[analysis][stack-store-fold]") {
  Fixture f;
  const OpId store = f.function.appendStore(f.entry, 0x1000, Type::integer(32), f.slot(-0x10),
                                            f.function.entryReg(f.reg("x0")));
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1004, Type::integer(32), f.slot(-0x10));
  f.function.appendStore(f.entry, 0x1008, Type::integer(32), f.i64(0x9000),
                         f.function.valueRef(loaded));
  f.function.appendReturn(f.entry, 0x100c);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto dead = findDeadStackStores(f.function, frame, VariableTable{});
  CHECK(!dead.contains(store.index()));
}

TEST_CASE("a store whose slot address is passed to an intrinsic is not dead",
          "[analysis][stack-store-fold]") {
  Fixture f;
  const ExprId address = f.slot(-0x10);
  const OpId store = f.function.appendStore(f.entry, 0x1000, Type::integer(32), address,
                                            f.function.entryReg(f.reg("x0")));
  const std::array<ExprId, 1> args{address};
  f.function.appendIntrinsic(f.entry, 0x1004, "memset", Type::voidType(), args);
  f.function.appendReturn(f.entry, 0x1008);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto dead = findDeadStackStores(f.function, frame, VariableTable{});
  CHECK(!dead.contains(store.index()));
}

TEST_CASE("a store whose slot address is stored elsewhere as a value is not dead",
          "[analysis][stack-store-fold]") {
  Fixture f;
  const ExprId address = f.slot(-0x10);
  const OpId store = f.function.appendStore(f.entry, 0x1000, Type::integer(32), address,
                                            f.function.entryReg(f.reg("x0")));
  // Another slot's own store, but its *value* is this slot's raw address --
  // the "spilled pointer to a local" shape, which can reach the slot through
  // whatever reads that pointer back later, invisibly to this analysis.
  f.function.appendStore(f.entry, 0x1004, Type::integer(64), f.slot(-0x20), address);
  f.function.appendReturn(f.entry, 0x1008);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto dead = findDeadStackStores(f.function, frame, VariableTable{});
  CHECK(!dead.contains(store.index()));
}

TEST_CASE("two stores to the same unread slot are both dead", "[analysis][stack-store-fold]") {
  Fixture f;
  const OpId first = f.function.appendStore(f.entry, 0x1000, Type::integer(32), f.slot(-0x10),
                                            f.function.entryReg(f.reg("x0")));
  const OpId second = f.function.appendStore(f.entry, 0x1004, Type::integer(32), f.slot(-0x10),
                                             f.function.entryReg(f.reg("x1")));
  f.function.appendReturn(f.entry, 0x1008);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto dead = findDeadStackStores(f.function, frame, VariableTable{});
  CHECK(dead.contains(first.index()));
  CHECK(dead.contains(second.index()));
}

TEST_CASE("a store to the promoted 'state' slot is left alone even though nothing reads it back",
          "[analysis][stack-store-fold]") {
  Fixture f;
  // Several distinct small-literal stores to one slot is exactly what
  // VariableTable::recover's own heuristic promotes to the "state" name
  // (see variables.cpp) -- and, per that promotion's own note, deliberately
  // without requiring a read, since a real dispatcher's state often lives
  // in a register/phi instead.
  const OpId store = f.function.appendStore(f.entry, 0x1000, Type::integer(32), f.slot(-0x10),
                                            f.i64(1));
  f.function.appendStore(f.entry, 0x1004, Type::integer(32), f.slot(-0x10), f.i64(2));
  f.function.appendStore(f.entry, 0x1008, Type::integer(32), f.slot(-0x10), f.i64(3));
  f.function.appendStore(f.entry, 0x100c, Type::integer(32), f.slot(-0x10), f.i64(4));
  f.function.appendReturn(f.entry, 0x1010);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const VariableTable variables = VariableTable::recover(f.function, frame);
  REQUIRE(variables.localAt(-0x10) != nullptr);
  REQUIRE(variables.localAt(-0x10)->name == "state");
  const auto dead = findDeadStackStores(f.function, frame, variables);
  CHECK(!dead.contains(store.index()));
}

TEST_CASE(
    "stores adjacent to a slot passed to a call are not dead, even though nothing reads "
    "them back",
    "[analysis][stack-store-fold]") {
  Fixture f;
  // Mirrors bc_lib's sub_2f9a38, block b4: the callee is only ever handed
  // `&var_70`, but the three stores together lay out an 0x18-byte aggregate
  // it reads through that one pointer (see analysis::StackEscapeMap).
  const ExprId address = f.slot(-0x70);
  const OpId first = f.function.appendStore(f.entry, 0x1000, Type::integer(64), address,
                                            f.i64(0x18));
  const OpId second =
      f.function.appendStore(f.entry, 0x1004, Type::integer(64), f.slot(-0x68), f.i64(1));
  const OpId third =
      f.function.appendStore(f.entry, 0x1008, Type::integer(64), f.slot(-0x60), f.i64(2));
  const OpId call = f.function.appendCall(f.entry, 0x100c, f.i64(0x9000));
  f.function.setOperands(call, std::vector<ExprId>{f.i64(0x9000), address});
  f.function.appendReturn(f.entry, 0x1010);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto dead = findDeadStackStores(f.function, frame, VariableTable{});
  CHECK(!dead.contains(first.index()));
  CHECK(!dead.contains(second.index()));
  CHECK(!dead.contains(third.index()));
}

TEST_CASE("a store to a global address is left alone", "[analysis][stack-store-fold]") {
  Fixture f;
  const OpId store = f.function.appendStore(f.entry, 0x1000, Type::integer(32), f.i64(0x30c420),
                                            f.function.entryReg(f.reg("x0")));
  f.function.appendReturn(f.entry, 0x1004);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto dead = findDeadStackStores(f.function, frame, VariableTable{});
  CHECK(dead.empty());
  (void)store;
}

TEST_CASE("callee-saved prologue spills adjacent to an escaped struct are still dead",
          "[analysis][stack-store-fold]") {
  il::RegisterFile regs = xdec::test::arm64Registers();
  const RegId x24 = regs.add("x24", 64, il::RegClass::General);
  const RegId x29 = regs.add("x29", 64, il::RegClass::General);
  Function fn(Arch::AArch64, regs, 0x1000);
  const BlockId entry = fn.createBlock(0x1000);
  fn.setEntryBlock(entry);
  const auto slot = [&](int64_t delta) {
    const ExprId sp = fn.entryReg(regs.find("sp"));
    return delta < 0
               ? fn.binary(ExprOp::Sub, sp, fn.constant(Type::integer(64), static_cast<uint64_t>(-delta)))
               : fn.binary(ExprOp::Add, sp, fn.constant(Type::integer(64), static_cast<uint64_t>(delta)));
  };

  // An escaped block struct at [-0x78, -0x68) immediately below callee-saved
  // spills at -0x68..-0x58. The aggregate fields stay live; the prologue
  // callee-saved spills must not be pulled into the struct's escape footprint.
  const ExprId blockAddr = slot(-0x78);
  const OpId field0 =
      fn.appendStore(entry, 0x1000, Type::integer(64), blockAddr, fn.constant(Type::integer(64), 0x1234));
  const OpId field1 =
      fn.appendStore(entry, 0x1004, Type::integer(64), slot(-0x70), fn.constant(Type::integer(64), 0x5678));
  const OpId spill24 =
      fn.appendStore(entry, 0x1008, Type::integer(64), slot(-0x68), fn.entryReg(x24));
  const OpId spill29 =
      fn.appendStore(entry, 0x100c, Type::integer(64), slot(-0x60), fn.entryReg(x29));
  const OpId call = fn.appendCall(entry, 0x1010, fn.constant(Type::integer(64), 0x9000));
  fn.setOperands(call, std::vector<ExprId>{fn.constant(Type::integer(64), 0x9000), blockAddr});
  fn.appendReturn(entry, 0x1014);
  fn.rebuildEdges();

  const StackFrame frame = StackFrame::compute(fn);
  const auto dead = findDeadStackStores(fn, frame, VariableTable{});
  CHECK(!dead.contains(field0.index()));
  CHECK(!dead.contains(field1.index()));
  CHECK(dead.contains(spill24.index()));
  CHECK(dead.contains(spill29.index()));
}

TEST_CASE("callee-saved vector prologue spills adjacent to an escaped struct are still dead",
          "[analysis][stack-store-fold]") {
  il::RegisterFile regs = xdec::test::arm64Registers();
  const RegId q8 = regs.add("q8", 128, il::RegClass::Vector);
  Function fn(Arch::AArch64, regs, 0x1000);
  const BlockId entry = fn.createBlock(0x1000);
  fn.setEntryBlock(entry);
  const auto slot = [&](int64_t delta) {
    const ExprId sp = fn.entryReg(regs.find("sp"));
    return fn.binary(ExprOp::Sub, sp, fn.constant(Type::integer(64), static_cast<uint64_t>(-delta)));
  };

  const ExprId blockAddr = slot(-0x78);
  const OpId field0 =
      fn.appendStore(entry, 0x1000, Type::integer(64), blockAddr, fn.constant(Type::integer(64), 0x1234));
  const il::ValueId readQ8 = fn.appendReadReg(entry, 0x1004, q8);
  const OpId spillQ8 = fn.appendStore(
      entry, 0x1008, Type::integer(64), slot(-0x70),
      fn.cast(ExprOp::Trunc, Type::integer(64), fn.valueRef(readQ8)));
  const OpId call = fn.appendCall(entry, 0x100c, fn.constant(Type::integer(64), 0x9000));
  fn.setOperands(call, std::vector<ExprId>{fn.constant(Type::integer(64), 0x9000), blockAddr});
  fn.appendReturn(entry, 0x1010);
  fn.rebuildEdges();

  const StackFrame frame = StackFrame::compute(fn);
  const auto dead = findDeadStackStores(fn, frame, VariableTable{});
  CHECK(!dead.contains(field0.index()));
  CHECK(dead.contains(spillQ8.index()));
}
