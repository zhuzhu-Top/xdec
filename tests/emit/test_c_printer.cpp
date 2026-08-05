// printFunction: the C text contract — variables, memory shapes, calls,
// structured statements, and the flag helpers.
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

TEST_CASE("a straight-line function: locals, a load temp, and the return",
          "[emit][print]") {
  Fixture f;
  // t0 = var_10; var_10 = t0 + 1; return t0;
  const il::ValueId loaded = f.function.appendLoad(
      f.entry, 0x1000, Type::integer(32), f.slot(-0x10));
  const ExprId bumped = f.function.binary(
      ExprOp::Add, f.function.valueRef(loaded),
      f.function.constant(Type::integer(32), 1));
  f.function.appendStore(f.entry, 0x1004, Type::integer(32), f.slot(-0x10),
                         bumped);
  const il::OpId ret = f.function.appendReturn(f.entry, 0x1008);
  f.function.setOperands(ret, std::vector<ExprId>{f.function.valueRef(loaded)});

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "uint32_t sub_1000(void)"));
  CHECK(contains(text, "int32_t var_10; // sp-16"));
  CHECK(contains(text, "uint32_t t0;"));
  CHECK(contains(text, "t0 = var_10;"));
  // No wrapping cast: t0 and the constant are both already 32-bit, the same
  // rank as `int`, so `+` already produces a 32-bit result on its own.
  CHECK(contains(text, "var_10 = (t0 + 0x1);"));
  CHECK(contains(text, "return t0;"));
}

TEST_CASE("arguments and a direct call print with the recovered prototype",
          "[emit][print]") {
  Fixture f;
  const ExprId x0 = f.entryReg("x0");
  const ExprId x1 = f.entryReg("x1");
  const il::OpId call = f.function.appendCall(f.entry, 0x1000, f.i64(0x5000));
  f.function.setOperands(call, std::vector<ExprId>{f.i64(0x5000), x0, x1});
  f.function.appendReturn(f.entry, 0x1004);

  const std::string text = f.emit();
  CHECK(contains(text, "void sub_1000(uint64_t a0, uint64_t a1)"));
  CHECK(contains(text, "sub_5000(a0, a1);"));
}

TEST_CASE("an indirect call prints a typed function pointer", "[emit][print]") {
  Fixture f;
  const ExprId x0 = f.entryReg("x0");
  const ExprId x1 = f.entryReg("x1");
  const il::OpId call = f.function.appendCall(f.entry, 0x1000, x0);
  f.function.setOperands(call, std::vector<ExprId>{x0, x1});
  f.function.appendReturn(f.entry, 0x1004);

  const std::string text = f.emit();
  CHECK(contains(text, "((void (*)(uint64_t))a0)(a1);"));
}

TEST_CASE("a diamond prints as if-else without gotos", "[emit][print]") {
  Fixture f;
  const BlockId left = f.function.createBlock(0x2000);
  const BlockId right = f.function.createBlock(0x3000);
  const BlockId merge = f.function.createBlock(0x4000);
  const ExprId cond =
      f.function.binary(ExprOp::CmpNe, f.entryReg("x0"), f.i64(0));
  f.function.appendCondBranch(f.entry, 0x1000, cond, left, right);
  f.function.appendStore(left, 0x2000, Type::integer(64), f.slot(-0x18),
                         f.i64(1));
  f.function.appendBranch(left, 0x2008, merge);
  f.function.appendStore(right, 0x3000, Type::integer(64), f.slot(-0x18),
                         f.i64(2));
  f.function.appendBranch(right, 0x3008, merge);
  f.function.appendReturn(merge, 0x4000);

  const std::string text = f.emit();
  CHECK(contains(text, "if ((a0 != 0x0)) {"));
  CHECK(contains(text, "var_18 = 0x1;"));
  CHECK(contains(text, "} else {"));
  CHECK(contains(text, "var_18 = 0x2;"));
  CHECK(!contains(text, "goto"));
}

TEST_CASE("global memory prints as a typed dereference", "[emit][print]") {
  Fixture f;
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1000, Type::integer(64), f.i64(0x400900));
  f.function.appendStore(f.entry, 0x1004, Type::integer(32), f.i64(0x400910),
                         f.function.cast(ExprOp::Trunc, Type::integer(32),
                                         f.function.valueRef(loaded)));
  f.function.appendReturn(f.entry, 0x1008);

  const std::string text = f.emit();
  CHECK(contains(text, "t0 = (*(uint64_t*)(0x400900));"));
  CHECK(contains(text, "(*(uint32_t*)(0x400910)) = ((uint32_t)(t0));"));
}

TEST_CASE("a residual flag condition prints its exact helper", "[emit][print]") {
  Fixture f;
  // flagcond:lt(flagdef:sub.32(a0, 1)) — the opaque-predicate shape.
  const ExprId a0 = f.entryReg("x0");
  const ExprId one = f.function.constant(Type::integer(32), 1);
  const ExprId def = f.function.flagDef(
      il::FlagOp::Sub, 32,
      std::vector<ExprId>{f.function.cast(ExprOp::Trunc, Type::integer(32), a0),
                          one});
  const ExprId cond = f.function.flagCondition(def, il::ConditionCode::SignedLess);
  const BlockId left = f.function.createBlock(0x2000);
  const BlockId right = f.function.createBlock(0x3000);
  f.function.appendCondBranch(f.entry, 0x1000, cond, left, right);
  f.function.appendReturn(left, 0x2000);
  f.function.appendReturn(right, 0x3000);

  const std::string text = f.emit();
  CHECK(contains(text, "__xdec_cc_lt_32(((uint32_t)(a0)), 0x1)"));
  // The helper is declared in the preamble, once, before the function.
  CHECK(contains(text, "static inline bool __xdec_cc_lt_32(uint32_t a, uint32_t b)"));
}

TEST_CASE("a returned select prints as a guard, and a nested one as a chain",
          "[emit][print]") {
  Fixture f;
  // What a clamp compiles to: two conditional selects, no branch, wrapped in the
  // convention's widening to the full register.
  const ExprId x = f.function.cast(ExprOp::Trunc, Type::integer(32), f.entryReg("x0"));
  const ExprId lo = f.function.cast(ExprOp::Trunc, Type::integer(32), f.entryReg("x1"));
  const ExprId hi = f.function.cast(ExprOp::Trunc, Type::integer(32), f.entryReg("x2"));
  const ExprId inner = f.function.select(
      f.function.binary(ExprOp::CmpLtS, hi, x), hi, x);
  const ExprId outer = f.function.select(
      f.function.binary(ExprOp::CmpLtS, x, lo), lo, inner);
  const il::OpId ret = f.function.appendReturn(f.entry, 0x1000);
  f.function.setOperands(
      ret, std::vector<ExprId>{f.function.cast(ExprOp::ZExt, Type::integer(64), outer)});

  const std::string text = f.emit();
  INFO(text);
  CHECK_FALSE(contains(text, "?"));
  // Two guards, one per select, and a bare return for the value neither claims.
  CHECK(contains(text, "if (((int32_t)(_cse1) < (int32_t)(_cse0))) {"));
  CHECK(contains(text, "return _cse0;"));
  CHECK(contains(text, "if (((int32_t)(_cse2) < (int32_t)(_cse1))) {"));
  CHECK(contains(text, "return _cse2;"));
  CHECK(contains(text, "return _cse1;"));
  // Every shared subexpression is named before the first guard. Named inside one,
  // the arm that does not run would leave the next guard reading nothing.
  CHECK(text.find("_cse2 = ") < text.find("if ("));
}

TEST_CASE("an assigned select prints as an if/else over the same lvalue",
          "[emit][print]") {
  Fixture f;
  // var_10 = cond ? x1 : x2 -- the same branchless form, in the position it
  // takes when the value it computes is kept rather than returned.
  const ExprId chosen =
      f.function.select(f.function.binary(ExprOp::CmpEq, f.entryReg("x0"), f.i64(0)),
                        f.entryReg("x1"), f.entryReg("x2"));
  f.function.appendStore(f.entry, 0x1000, Type::integer(64), f.slot(-0x10), chosen);
  f.function.appendReturn(f.entry, 0x1004);

  const std::string text = f.emit();
  INFO(text);
  CHECK_FALSE(contains(text, "?"));
  // The else is what makes this different from the returned form: an assignment
  // does not leave the statement, so the arms have to exclude each other.
  CHECK(contains(text, "if ((a0 == 0x0)) {"));
  CHECK(contains(text, "var_10 = a1;"));
  CHECK(contains(text, "} else {"));
  CHECK(contains(text, "var_10 = a2;"));
}

TEST_CASE("a select nested in a larger value stays a ternary", "[emit][print]") {
  Fixture f;
  // var_10 = x1 ^ (cond ? x2 : 0). Only the xor is being assigned, and there is
  // no lvalue to give the arms -- hoisting the select to a temporary of its own
  // would be inventing a statement the code did not have.
  const ExprId masked =
      f.function.select(f.function.binary(ExprOp::CmpEq, f.entryReg("x0"), f.i64(0)),
                        f.entryReg("x2"), f.i64(0));
  const ExprId combined = f.function.binary(ExprOp::Xor, f.entryReg("x1"), masked);
  f.function.appendStore(f.entry, 0x1000, Type::integer(64), f.slot(-0x10), combined);
  f.function.appendReturn(f.entry, 0x1004);

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "?"));
  CHECK(contains(text, "var_10 = (a1 ^ ((a0 == 0x0) ? a2 : 0x0));"));
}

}  // namespace
