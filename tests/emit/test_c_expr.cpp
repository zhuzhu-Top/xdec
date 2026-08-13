// The C the expression stage must emit where C's own rules differ from the
// IL's: sign extension, one-bit logic, signed operations, and the registers
// that stay in op form.
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
  il::RegId reg(std::string_view name) { return function.registers().find(name); }

  /// Stores `value` to a global, which is the shortest way to get an
  /// expression into the printed body verbatim.
  void observe(ExprId value, uint32_t width = 64) {
    function.appendStore(entry, 0x1000, Type::integer(width), i64(0x400000), value);
  }

  std::string emit() {
    function.appendReturn(entry, 0x1010);
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

TEST_CASE("sign extension goes through the source width's signed type",
          "[emit][expr]") {
  Fixture f;
  // A cast straight to int64_t would zero-extend: the operand's C type is
  // unsigned, so the sign bit at bit 31 would never reach bits 32..63.
  const ExprId narrow =
      f.function.cast(ExprOp::Trunc, Type::integer(32), f.entryReg("x0"));
  f.observe(f.function.cast(ExprOp::SExt, Type::integer(64), narrow));

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "((int64_t)(int32_t)(arg1))"));
}

TEST_CASE("sign extension from one bit negates the condition", "[emit][expr]") {
  Fixture f;
  // i1 has no signed C type; the value is 0 or 1, and its extension is 0 or -1.
  const ExprId cond = f.function.binary(ExprOp::CmpEq, f.entryReg("x0"), f.i64(0));
  f.observe(f.function.cast(ExprOp::SExt, Type::integer(64), cond));

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "(-((int64_t)((arg1 == 0x0))))"));
}

TEST_CASE("a one-bit not is logical, not bitwise", "[emit][expr]") {
  Fixture f;
  // `~` on a bool yields -1 or -2, both true: every negated condition would
  // become a tautology.
  const ExprId cond = f.function.binary(ExprOp::CmpEq, f.entryReg("x0"), f.i64(0));
  f.observe(f.function.unary(ExprOp::Not, cond), 8);

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "(!((arg1 == 0x0)))"));
  CHECK(!contains(text, "~"));
}

TEST_CASE("a wide not stays bitwise and wraps to its width", "[emit][expr]") {
  Fixture f;
  const ExprId narrow =
      f.function.cast(ExprOp::Trunc, Type::integer(32), f.entryReg("x0"));
  f.observe(f.function.unary(ExprOp::Not, narrow), 32);

  const std::string text = f.emit();
  INFO(text);
  // No outer cast: `int`/`uint32_t` already share a rank, so `~` on an
  // already-uint32_t operand wraps to 32 bits on its own, with no
  // promotion for a cast to undo. `arg1` is declared uint32_t itself, so
  // the inner cast Trunc would otherwise add is redundant too.
  CHECK(contains(text, "(~(arg1))"));
}

TEST_CASE("a zero-extension assigned straight to a store drops its cast",
          "[emit][expr]") {
  Fixture f;
  // observe() stores the value directly: a genuine C assignment converts
  // an unsigned source to a wider type correctly on its own, for any
  // source width, so the ZExt's own cast is pure noise here.
  const ExprId narrow =
      f.function.cast(ExprOp::Trunc, Type::integer(8), f.entryReg("x0"));
  f.observe(f.function.cast(ExprOp::ZExt, Type::integer(64), narrow));

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "= arg1;"));
  CHECK(!contains(text, "(uint64_t)(arg1)"));
}

TEST_CASE("a zero-extension embedded in another operator keeps its cast",
          "[emit][expr]") {
  Fixture f;
  // Nested inside an Add (not the root value handed to an assignment),
  // this text can be read at its own promoted width by the surrounding
  // operator, so the cast must stay -- dropping it here (rather than only
  // at a root call) is what let a shift misread a still-32-bit value as
  // already 64 bits and shift it past its promoted width.
  const ExprId narrow =
      f.function.cast(ExprOp::Trunc, Type::integer(8), f.entryReg("x0"));
  const ExprId widened = f.function.cast(ExprOp::ZExt, Type::integer(64), narrow);
  f.observe(f.function.binary(ExprOp::Add, widened, f.i64(1)));

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "(uint64_t)(arg1)"));
}

TEST_CASE("a shift forces its shifted operand to the shift's own width",
          "[emit][expr]") {
  Fixture f;
  // A 32-bit value shifted by 40 would be undefined behaviour in C if left
  // at its own (promoted) 32-bit width: the shift amount exceeds it. The
  // shifted operand must be cast up to the shift's declared 64-bit width
  // first, regardless of any cast-elision that would be valid elsewhere.
  const ExprId narrow =
      f.function.cast(ExprOp::Trunc, Type::integer(32), f.entryReg("x0"));
  const ExprId widened = f.function.cast(ExprOp::ZExt, Type::integer(64), narrow);
  f.observe(f.function.binary(ExprOp::Shl, widened, f.i64(40)));

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "((uint64_t)(arg1)) << 0x28"));
}

TEST_CASE("a shift already at its own width adds no extra cast",
          "[emit][expr]") {
  Fixture f;
  f.observe(f.function.binary(ExprOp::Shl, f.entryReg("x0"), f.i64(5)));

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "(arg1 << 0x5)"));
}

TEST_CASE("a signed comparison casts both operands", "[emit][expr]") {
  Fixture f;
  // Casting only the left operand leaves C's usual arithmetic conversions to
  // turn the whole comparison unsigned again.
  const ExprId left =
      f.function.cast(ExprOp::Trunc, Type::integer(32), f.entryReg("x0"));
  const ExprId right =
      f.function.cast(ExprOp::Trunc, Type::integer(32), f.entryReg("x1"));
  f.observe(f.function.binary(ExprOp::CmpLtS, left, right), 8);

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "((int32_t)(arg1) < (int32_t)(arg2))"));
}

TEST_CASE("a zero-extended value compared to zero drops the cast",
          "[emit][expr]") {
  Fixture f;
  // Zero-extension never changes whether a value is zero, so the ZExt this
  // builds around an 8-bit value -- the shape a byte load compared at a
  // wider width gets -- is exactly the cast a `== 0`/`!= 0` comparison does
  // not need: `byte == 0` already means what `(uint32_t)(byte) == 0` does.
  const ExprId narrow =
      f.function.cast(ExprOp::Trunc, Type::integer(8), f.entryReg("x0"));
  const ExprId widened = f.function.cast(ExprOp::ZExt, Type::integer(32), narrow);
  f.observe(f.function.binary(ExprOp::CmpEq, widened, f.i64(0)), 8);

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "(arg1 == 0x0)"));
  CHECK(!contains(text, "(uint32_t)"));
}

TEST_CASE("a zero-extended value compared to a nonzero constant keeps the cast",
          "[emit][expr]") {
  Fixture f;
  const ExprId narrow =
      f.function.cast(ExprOp::Trunc, Type::integer(8), f.entryReg("x0"));
  const ExprId widened = f.function.cast(ExprOp::ZExt, Type::integer(32), narrow);
  f.observe(f.function.binary(ExprOp::CmpEq, widened, f.i64(5)), 8);

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "((uint32_t)(arg1)) == 0x5)"));
}

TEST_CASE("signed division casts both operands", "[emit][expr]") {
  Fixture f;
  f.observe(f.function.binary(ExprOp::DivS, f.entryReg("x0"), f.entryReg("x1")));

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "((int64_t)(arg1) / (int64_t)(arg2))"));
}

TEST_CASE("an arithmetic shift casts only the shifted operand", "[emit][expr]") {
  Fixture f;
  f.observe(f.function.binary(ExprOp::ShrS, f.entryReg("x0"), f.i64(3)));

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "((int64_t)(arg1) >> 0x3)"));
}

TEST_CASE("an untracked register reads and writes through a named variable",
          "[emit][expr]") {
  Fixture f;
  // The vector class is outside register SSA, so these stay ops. Printing the
  // read as a value of a declared variable is what keeps the data flow
  // visible; the alternative is a zero standing in for a real register.
  const il::ValueId read = f.function.appendReadReg(f.entry, 0x1000, f.reg("q0"));
  f.function.appendWriteReg(f.entry, 0x1004, f.reg("q0"),
                            f.function.valueRef(read));

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "uint128_t q0 = __entry_q0; // vector register"));
  CHECK(contains(text, "extern const uint128_t __entry_q0;"));
  CHECK(contains(text, "t0 = q0;"));
  CHECK(contains(text, "q0 = (uint128_t)(t0);"));
  CHECK(!contains(text, "dead-value"));
}

TEST_CASE("a zero-extending register view writes the whole register",
          "[emit][expr]") {
  Fixture f;
  // Writing s0 zeroes the rest of q0, which a plain assignment expresses.
  f.function.appendWriteReg(f.entry, 0x1000, f.reg("s0"), f.entryReg("x0"));

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "q0 = (uint128_t)(arg1);"));
}

TEST_CASE("a bit rotate calls a real, defined helper", "[emit][expr]") {
  Fixture f;
  // Rotate is common enough in obfuscated code that it must never fall
  // through to the generic "no case for this op" 0-stub.
  f.observe(f.function.binary(ExprOp::RotR, f.entryReg("x0"), f.i64(0x3f)));
  f.observe(f.function.binary(ExprOp::RotL, f.entryReg("x1"), f.i64(0x7)));

  const std::string text = f.emit();
  INFO(text);
  // Defined once in xdec_helpers.h, not inline in every decompiled file --
  // the body just pulls the header in and calls the short name.
  CHECK(contains(text, "#include \"xdec_helpers.h\""));
  CHECK(!contains(text, "static inline uint64_t rotr64"));
  CHECK(contains(text, "rotr64(arg1, 0x3f)"));
  CHECK(contains(text, "rotl64(arg2, 0x7)"));
  CHECK(!contains(text, "__xdec_rotr64"));
  CHECK(!contains(text, "__xdec_rotl64"));
  CHECK(!contains(text, "rotr?"));
  CHECK(!contains(text, "rotl?"));
}

TEST_CASE("a jump-table index clamp prints as a plain ternary",
          "[emit][expr]") {
  Fixture f;
  // `bound < index ? replacement : index` -- an OLLVM jump table's
  // out-of-range guard once its compare survives folding. Wrapped in a
  // harmless `+ 0` so it prints through `inner()`'s ordinary operand path
  // rather than `printOp`'s Store case, which flattens a bare top-level
  // select into if/else on its own (see `flattenSelect`) before the ternary
  // ever gets a look at it.
  const ExprId index = f.entryReg("x0");
  const ExprId clamp = f.function.binary(ExprOp::CmpLtS, f.i64(0x9), index);
  const ExprId select = f.function.select(clamp, f.i64(0x4), index);
  f.observe(f.function.binary(ExprOp::Add, select, f.i64(0)));

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "? 0x4 : arg1"));
  CHECK(!contains(text, "xdec_dispatch_index"));
}

TEST_CASE("an unsigned jump-table index clamp also prints as a plain ternary",
          "[emit][expr]") {
  Fixture f;
  const ExprId index = f.entryReg("x0");
  const ExprId clamp = f.function.binary(ExprOp::CmpLtU, f.i64(0x9), index);
  const ExprId select = f.function.select(clamp, f.i64(0x4), index);
  f.observe(f.function.binary(ExprOp::Add, select, f.i64(0)));

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "? 0x4 : arg1"));
  CHECK(!contains(text, "xdec_dispatch_index"));
}

TEST_CASE("a select that is not an index clamp still prints as a plain "
          "ternary",
          "[emit][expr]") {
  Fixture f;
  // Same compare, but the *true* arm reads the index too (not a
  // replacement) -- an ordinary min/max idiom.
  const ExprId index = f.entryReg("x0");
  const ExprId clamp = f.function.binary(ExprOp::CmpLtS, f.i64(0x9), index);
  const ExprId select = f.function.select(clamp, index, f.i64(0x4));
  f.observe(f.function.binary(ExprOp::Add, select, f.i64(0)));

  const std::string text = f.emit();
  INFO(text);
  CHECK(!contains(text, "xdec_dispatch_index"));
  CHECK(contains(text, "? arg1 : 0x4"));
}

TEST_CASE("count-trailing-zeros is a labelled embedder stub, not a silent zero",
          "[emit][expr]") {
  Fixture f;
  f.observe(f.function.unary(ExprOp::Ctz, f.entryReg("x0")));

  const std::string text = f.emit();
  INFO(text);
  // Declared in xdec_helpers.h (a real prototype, not a comment): the
  // header is the one place an embedder needs to look to see the whole
  // list of stubs a decompiled body might call.
  CHECK(contains(text, "#include \"xdec_helpers.h\""));
  CHECK(contains(text, "xdec_ctz64(arg1)"));
  CHECK(!contains(text, "__xdec_ctz64"));
  CHECK(!contains(text, "ctz?"));
}

TEST_CASE("concat places the high operand above the low operand's width",
          "[emit][expr]") {
  Fixture f;
  const ExprId lo = f.function.cast(ExprOp::Trunc, Type::integer(32), f.entryReg("x0"));
  const ExprId hi = f.function.cast(ExprOp::Trunc, Type::integer(32), f.entryReg("x1"));
  f.observe(f.function.concat(Type::integer(64), hi, lo));

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "((uint64_t)(arg2) << 32) | (uint64_t)(arg1)"));
}

TEST_CASE("a preserving register view writes through a masked insert",
          "[emit][expr]") {
  Fixture f;
  // `ins v0.d[1]` keeps the low half, so the view goes back through its root
  // instead of becoming a variable of its own that would silently diverge.
  f.function.appendWriteReg(f.entry, 0x1000, f.reg("q0_hi"), f.entryReg("x0"));
  const il::ValueId read = f.function.appendReadReg(f.entry, 0x1004, f.reg("q0_hi"));
  f.function.appendStore(f.entry, 0x1008, Type::integer(64), f.i64(0x400000),
                         f.function.valueRef(read));

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text,
                 "q0 = (q0 & ~(((uint128_t)((((uint128_t)1) << 64) - 1)) << 64)) |"));
  CHECK(contains(text, "t0 = ((uint64_t)((q0 >> 64)));"));
}

}  // namespace
