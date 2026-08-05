// The interpreter, checked against hand-computed expectations.
//
// These tests use values worked out by hand, not values produced by the code
// under test: an oracle that shares its arithmetic with the thing it checks
// agrees with every bug. The independent machine oracle (Unicorn) is P5c; this
// file is what makes the interpreter trustworthy enough to serve as the other
// half of that comparison.
#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdint>
#include <cstring>
#include <format>

#include "il/il_test_support.h"
#include "xdec/il/interp.h"

namespace Catch {
template <>
struct StringMaker<xdec::il::ConcreteValue> {
  static std::string convert(const xdec::il::ConcreteValue& value) {
    return value.hi == 0 ? std::format("{:#x}", value.lo)
                         : std::format("{:#x}:{:#x}", value.hi, value.lo);
  }
};
}  // namespace Catch

using xdec::Arch;
using xdec::il::BlockId;
using xdec::il::ConcreteValue;
using xdec::il::ConditionCode;
using xdec::il::ExecStop;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::FlagBitIndex;
using xdec::il::FlagOp;
using xdec::il::Function;
using xdec::il::Interpreter;
using xdec::il::RegId;
using xdec::il::Type;

namespace {

// One block per test: `write <reg>, <expr>; ret`, so any expression's value is
// observable as a register afterwards.
struct Rig {
  Rig() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000), interp(function) {
    block = function.createBlock(0x1000);
  }

  RegId reg(std::string_view name) const {
    const RegId id = function.registers().find(name);
    REQUIRE(id.valid());
    return id;
  }

  ExprId k(uint64_t value, unsigned width = 64) {
    return function.constant(Type::integer(width), value);
  }

  /// Appends `write <reg>, expr; ret` in a fresh block and runs it.
  ExecStop run(ExprId expr, std::string_view dest = "x0") {
    const uint64_t va = nextVa;
    nextVa += 8;
    const BlockId fresh = function.createBlock(va);
    function.appendWriteReg(fresh, va, reg(dest), expr);
    function.appendReturn(fresh, va + 4);
    return interp.runBlock(fresh).stop;
  }

  /// The value of an integer expression, via the x0 round trip.
  ConcreteValue val(ExprId expr) {
    REQUIRE(run(expr) == ExecStop::Return);
    return interp.readRegister(reg("x0"));
  }

  Function function;
  BlockId block;
  Interpreter interp;
  uint64_t nextVa = 0x4000;
};

[[nodiscard]] ConcreteValue c(uint64_t lo, uint64_t hi = 0) { return ConcreteValue{lo, hi}; }

}  // namespace

TEST_CASE("integer arithmetic executes at the right width", "[il][interp]") {
  Rig rig;
  CHECK(rig.val(rig.function.binary(ExprOp::Add, rig.k(5), rig.k(7))) == c(12));
  CHECK(rig.val(rig.function.binary(ExprOp::Sub, rig.k(5), rig.k(7))) == c(~1ull));
  CHECK(rig.val(rig.function.binary(ExprOp::Mul, rig.k(1ull << 40), rig.k(1ull << 40))) ==
        c(0));  // wraps at 2^64
  CHECK(rig.val(rig.function.unary(ExprOp::Neg, rig.k(1))) == c(~0ull));

  SECTION("narrow widths wrap where they must") {
    const ExprId sum8 = rig.function.binary(ExprOp::Add, rig.k(0xFF, 8), rig.k(1, 8));
    CHECK(rig.val(sum8) == c(0));
    const ExprId product16 =
        rig.function.binary(ExprOp::Mul, rig.k(0xFFFF, 16), rig.k(0xFFFF, 16));
    CHECK(rig.val(product16) == c(1));
  }

  SECTION("division by zero produces zero, the AArch64 rule") {
    CHECK(rig.val(rig.function.binary(ExprOp::DivU, rig.k(7), rig.k(0))) == c(0));
    CHECK(rig.val(rig.function.binary(ExprOp::DivS, rig.k(~6ull), rig.k(0))) == c(0));
    CHECK(rig.val(rig.function.binary(ExprOp::RemU, rig.k(7), rig.k(0))) == c(7));
    CHECK(rig.val(rig.function.binary(ExprOp::RemS, rig.k(~6ull), rig.k(0))) == c(~6ull));
  }

  SECTION("signed division wraps instead of trapping") {
    const ExprId min = rig.k(uint64_t{1} << 63);
    CHECK(rig.val(rig.function.binary(ExprOp::DivS, min, rig.k(~0ull))) ==
          c(uint64_t{1} << 63));
    CHECK(rig.val(rig.function.binary(ExprOp::RemS, min, rig.k(~0ull))) == c(0));
    CHECK(rig.val(rig.function.binary(ExprOp::DivS, rig.k(~6ull), rig.k(2))) == c(~2ull));
    CHECK(rig.val(rig.function.binary(ExprOp::RemS, rig.k(~6ull), rig.k(2))) == c(~0ull));
  }

  SECTION("the high half of a multiply") {
    CHECK(rig.val(rig.function.binary(ExprOp::MulHiU, rig.k(~0ull), rig.k(2))) == c(1));
    CHECK(rig.val(rig.function.binary(ExprOp::MulHiS, rig.k(~0ull), rig.k(2))) ==
          c(~0ull));
    CHECK(rig.val(rig.function.binary(ExprOp::MulHiS, rig.k(~0ull), rig.k(~0ull))) ==
          c(0));
    // (-2^63) * 2 = -2^64, whose signed high half is -1.
    CHECK(rig.val(rig.function.binary(ExprOp::MulHiS, rig.k(uint64_t{1} << 63),
                                      rig.k(2))) == c(~0ull));
  }
}

TEST_CASE("bitwise and shift operations", "[il][interp]") {
  Rig rig;
  CHECK(rig.val(rig.function.binary(ExprOp::And, rig.k(0xF0), rig.k(0x3C))) == c(0x30));
  CHECK(rig.val(rig.function.binary(ExprOp::Or, rig.k(0xF0), rig.k(0x3C))) == c(0xFC));
  CHECK(rig.val(rig.function.binary(ExprOp::Xor, rig.k(0xF0), rig.k(0x3C))) == c(0xCC));
  CHECK(rig.val(rig.function.unary(ExprOp::Not, rig.k(0xFF, 8))) == c(0xFF00 & 0xFF));  // at i8

  SECTION("shifts at or past the width do not invoke undefined behaviour") {
    CHECK(rig.val(rig.function.binary(ExprOp::Shl, rig.k(1), rig.k(64))) == c(0));
    CHECK(rig.val(rig.function.binary(ExprOp::ShrU, rig.k(~0ull), rig.k(64))) == c(0));
    CHECK(rig.val(rig.function.binary(ExprOp::ShrS, rig.k(~0ull), rig.k(64))) ==
          c(~0ull));
    CHECK(rig.val(rig.function.binary(ExprOp::Shl, rig.k(1), rig.k(63))) ==
          c(uint64_t{1} << 63));
  }

  SECTION("arithmetic shift replicates the sign at the operand's width") {
    const ExprId top = rig.k(0x80000000, 32);
    CHECK(rig.val(rig.function.binary(ExprOp::ShrS, top, rig.k(31, 32))) == c(0xFFFFFFFF));
    CHECK(rig.val(rig.function.binary(ExprOp::ShrU, top, rig.k(31, 32))) == c(1));
  }

  SECTION("rotates wrap around the width") {
    CHECK(rig.val(rig.function.binary(ExprOp::RotR, rig.k(0x11, 8), rig.k(4, 8))) ==
          c(0x11));
    CHECK(rig.val(rig.function.binary(ExprOp::RotL, rig.k(0x81, 8), rig.k(1, 8))) ==
          c(0x03));
    CHECK(rig.val(rig.function.binary(ExprOp::RotR, rig.k(1), rig.k(1))) ==
          c(uint64_t{1} << 63));
    // A rotate by the width is the identity, not UB.
    CHECK(rig.val(rig.function.binary(ExprOp::RotR, rig.k(0x1234), rig.k(64))) ==
          c(0x1234));
  }
}

TEST_CASE("width-changing operations", "[il][interp]") {
  Rig rig;
  CHECK(rig.val(rig.function.cast(ExprOp::ZExt, Type::integer(64), rig.k(0x80, 8))) ==
        c(0x80));
  CHECK(rig.val(rig.function.cast(ExprOp::SExt, Type::integer(64), rig.k(0x80, 8))) ==
        c(~0x7Full));
  CHECK(rig.val(rig.function.cast(ExprOp::Trunc, Type::integer(32),
                                  rig.k(0x100000005ull))) == c(5));

  SECTION("extract and concat move fields across the 64-bit boundary") {
    const ExprId both =
        rig.function.concat(Type::integer(128), rig.k(0xABCDEF), rig.k(0x1122334455667788));
    rig.function.appendWriteReg(rig.block, 0x1000, rig.reg("x0"), both);
    rig.function.appendReturn(rig.block, 0x1004);
    // x0 is 64 bits; the write truncates to the register's width.
    CHECK(rig.interp.runBlock(rig.block).stop == ExecStop::Return);
    CHECK(rig.interp.readRegister(rig.reg("x0")) == c(0x1122334455667788));

    Rig rig2;
    const ExprId both2 =
        rig2.function.concat(Type::integer(128), rig2.k(0xABCDEF), rig2.k(0));
    const ExprId high =
        rig2.function.extract(Type::integer(64), both2, 64);
    CHECK(rig2.val(high) == c(0xABCDEF));
  }
}

TEST_CASE("bit-count operations", "[il][interp]") {
  Rig rig;
  CHECK(rig.val(rig.function.unary(ExprOp::Clz, rig.k(1))) == c(63));
  CHECK(rig.val(rig.function.unary(ExprOp::Clz, rig.k(0, 32))) == c(32));
  CHECK(rig.val(rig.function.unary(ExprOp::Ctz, rig.k(0x10))) == c(4));
  CHECK(rig.val(rig.function.unary(ExprOp::Ctz, rig.k(0, 32))) == c(32));
  CHECK(rig.val(rig.function.unary(ExprOp::PopCount, rig.k(0xFF))) == c(8));
  CHECK(rig.val(rig.function.unary(ExprOp::ByteSwap, rig.k(0x11223344, 32))) ==
        c(0x44332211));
  CHECK(rig.val(rig.function.unary(ExprOp::BitReverse, rig.k(1))) == c(uint64_t{1} << 63));
  CHECK(rig.val(rig.function.unary(ExprOp::BitReverse, rig.k(0x80, 8))) == c(1));
}

TEST_CASE("compares and select", "[il][interp]") {
  Rig rig;
  const ExprId minusOne = rig.k(~0ull);
  CHECK(rig.val(rig.function.binary(ExprOp::CmpLtU, minusOne, rig.k(0))) == c(0));
  CHECK(rig.val(rig.function.binary(ExprOp::CmpLtS, minusOne, rig.k(0))) == c(1));
  CHECK(rig.val(rig.function.binary(ExprOp::CmpLeU, minusOne, minusOne)) == c(1));
  CHECK(rig.val(rig.function.binary(ExprOp::CmpEq, rig.k(7), rig.k(7))) == c(1));
  CHECK(rig.val(rig.function.binary(ExprOp::CmpNe, rig.k(7), rig.k(7))) == c(0));

  const ExprId cond = rig.function.binary(ExprOp::CmpLtS, minusOne, rig.k(0));
  CHECK(rig.val(rig.function.select(cond, rig.k(11), rig.k(22))) == c(11));
}

TEST_CASE("lazy flags materialise to the right four bits", "[il][interp]") {
  Rig rig;
  const auto nzcvOf = [&](FlagOp op, std::initializer_list<uint64_t> args, unsigned w = 64) {
    ExprId operands[3];
    unsigned count = 0;
    for (const uint64_t arg : args) {
      operands[count++] = rig.k(arg, w);
    }
    const ExprId flags = rig.function.flagDef(op, w, std::span{operands, count});
    const ExprId n = rig.function.flagBitOf(flags, FlagBitIndex::Negative);
    const ExprId z = rig.function.flagBitOf(flags, FlagBitIndex::Zero);
    const ExprId cbit = rig.function.flagBitOf(flags, FlagBitIndex::Carry);
    const ExprId v = rig.function.flagBitOf(flags, FlagBitIndex::Overflow);
    const ExprId bits = rig.function.binary(
        ExprOp::Add,
        rig.function.binary(
            ExprOp::Add, rig.function.cast(ExprOp::ZExt, Type::integer(64), n),
            rig.function.binary(ExprOp::Shl,
                                rig.function.cast(ExprOp::ZExt, Type::integer(64), z),
                                rig.k(1))),
        rig.function.binary(
            ExprOp::Add,
            rig.function.binary(ExprOp::Shl,
                                rig.function.cast(ExprOp::ZExt, Type::integer(64), cbit),
                                rig.k(2)),
            rig.function.binary(ExprOp::Shl,
                                rig.function.cast(ExprOp::ZExt, Type::integer(64), v),
                                rig.k(3))));
    return rig.val(bits).lo;
  };

  // Packing here is N in bit 0, Z in bit 1, C in bit 2, V in bit 3 (the test's
  // own order, so the expectations below spell out which bit is which).
  CHECK(nzcvOf(FlagOp::Sub, {5, 5}) == 0b0110);          // r=0: Z, C (no borrow)
  CHECK(nzcvOf(FlagOp::Sub, {0, 1}) == 0b0001);          // r<0: N, borrow clears C
  CHECK(nzcvOf(FlagOp::Add, {~0ull, 1}) == 0b0110);      // r=0: Z, carry out
  CHECK(nzcvOf(FlagOp::Add, {0x7FFFFFFFFFFFFFFFull, 1}) == 0b1001);  // N and V
  CHECK(nzcvOf(FlagOp::AddCarry, {~0ull, 0, 1}) == 0b0110);  // carry-in tips it over
  CHECK(nzcvOf(FlagOp::SubCarry, {0, 0, 0}) == 0b0001);  // 0 - 0 - borrow = -1
  CHECK(nzcvOf(FlagOp::SubCarry, {5, 5, 1}) == 0b0110);
  CHECK(nzcvOf(FlagOp::Logical, {0}) == 0b0010);         // Z, C and V cleared
  CHECK(nzcvOf(FlagOp::Logical, {uint64_t{1} << 63}) == 0b0001);

  SECTION("the operand width decides where carry and sign live") {
    CHECK(nzcvOf(FlagOp::Sub, {0, 1}, 32) == 0b0001);      // N from bit 31
    CHECK(nzcvOf(FlagOp::Add, {0xFFFFFFFF, 1}, 32) == 0b0110);  // carry out of bit 31
    CHECK(nzcvOf(FlagOp::Add, {0x7FFFFFFF, 1}, 32) == 0b1001);
  }

  SECTION("a literal bundle is read as NZCV in bits 3..0") {
    CHECK(nzcvOf(FlagOp::Const, {0b1010}) == 0b0101);  // N and C set
  }
}

TEST_CASE("every condition code against a known bundle", "[il][interp]") {
  Rig rig;
  // nzcv = N=1, Z=0, C=1, V=0 (sub 0,1): exercise the full table.
  const ExprId flags = [&] {
    const ExprId ops[] = {rig.k(0), rig.k(1)};
    return rig.function.flagDef(FlagOp::Sub, 64, ops);
  }();
  const auto check = [&](ConditionCode code, bool expected) {
    const ExprId cond = rig.function.flagCondition(flags, code);
    INFO(toString(code));
    CHECK(rig.val(cond).lo == (expected ? 1 : 0));
  };
  check(ConditionCode::Equal, false);
  check(ConditionCode::NotEqual, true);
  check(ConditionCode::CarrySet, false);
  check(ConditionCode::CarryClear, true);
  check(ConditionCode::Negative, true);
  check(ConditionCode::NonNegative, false);
  check(ConditionCode::Overflow, false);
  check(ConditionCode::NoOverflow, true);
  check(ConditionCode::UnsignedGreater, false);
  check(ConditionCode::UnsignedLessEqual, true);
  check(ConditionCode::SignedGreaterEqual, false);
  check(ConditionCode::SignedLess, true);
  check(ConditionCode::SignedGreater, false);
  check(ConditionCode::SignedLessEqual, true);
  check(ConditionCode::Always, true);
  check(ConditionCode::Never, false);
}

TEST_CASE("the flags register round-trips through a read", "[il][interp]") {
  Rig rig;
  // write nzcv = flagdef; read nzcv; flagcond on the read. The condition must
  // see the materialised bits, not re-derive them.
  const ExprId ops[] = {rig.k(5), rig.k(5)};
  const ExprId flags = rig.function.flagDef(FlagOp::Sub, 64, ops);
  rig.function.appendWriteReg(rig.block, 0x1000, rig.reg("nzcv"), flags);
  const xdec::il::ValueId read = rig.function.appendReadReg(rig.block, 0x1004, rig.reg("nzcv"));
  const ExprId eq = rig.function.flagCondition(rig.function.valueRef(read),
                                               ConditionCode::Equal);
  rig.function.appendWriteReg(rig.block, 0x1008, rig.reg("x0"), eq);
  rig.function.appendReturn(rig.block, 0x100C);
  REQUIRE(rig.interp.runBlock(rig.block).stop == ExecStop::Return);
  CHECK(rig.interp.readRegister(rig.reg("x0")) == c(1));
  // And the register itself reports the materialised bundle: Z and C set.
  CHECK(rig.interp.readRegister(rig.reg("nzcv")) == c(0b0110));
}

TEST_CASE("floating point", "[il][interp]") {
  Rig rig;
  const auto f64 = [](double value) { return std::bit_cast<uint64_t>(value); };
  const auto f32 = [](float value) { return std::bit_cast<uint32_t>(value); };
  const ExprId oneHalf = rig.function.constant(Type::floating(64), f64(1.5));
  const ExprId twoQuarter = rig.function.constant(Type::floating(64), f64(2.25));
  const ExprId sum =
      rig.function.binary(ExprOp::FAdd, oneHalf, twoQuarter);
  CHECK(rig.val(sum) == c(f64(3.75)));
  CHECK(rig.val(rig.function.binary(ExprOp::FDiv, oneHalf, twoQuarter)) ==
        c(f64(1.5 / 2.25)));
  CHECK(rig.val(rig.function.unary(ExprOp::FSqrt, rig.function.constant(
                                                    Type::floating(64), f64(2.25)))) ==
        c(f64(1.5)));
  CHECK(rig.val(rig.function.unary(ExprOp::FNeg, oneHalf)) == c(f64(-1.5)));
  CHECK(rig.val(rig.function.unary(ExprOp::FAbs, rig.function.constant(
                                                    Type::floating(64), f64(-1.5)))) ==
        c(f64(1.5)));

  SECTION("comparisons are ordered, NaN is not less than anything") {
    const ExprId nan = rig.function.constant(Type::floating(64),
                                             uint64_t{0x7FF8000000000000});
    CHECK(rig.val(rig.function.binary(ExprOp::FCmpLt, nan, oneHalf)) == c(0));
    CHECK(rig.val(rig.function.binary(ExprOp::FCmpUnordered, nan, oneHalf)) == c(1));
    CHECK(rig.val(rig.function.binary(ExprOp::FCmpEq, oneHalf, oneHalf)) == c(1));
  }

  SECTION("float-to-int saturates like fcvtzs, NaN converts to zero") {
    const ExprId big = rig.function.constant(Type::floating(64), f64(1.0e30));
    CHECK(rig.val(rig.function.cast(ExprOp::FpToIntS, Type::integer(32), big)) ==
          c(0x7FFFFFFF));
    const ExprId negBig = rig.function.constant(Type::floating(64), f64(-1.0e30));
    CHECK(rig.val(rig.function.cast(ExprOp::FpToIntS, Type::integer(32), negBig)) ==
          c(0x80000000));
    CHECK(rig.val(rig.function.cast(ExprOp::FpToIntU, Type::integer(32),
                                    rig.function.constant(Type::floating(64),
                                                          f64(-1.5)))) == c(0));
    const ExprId nan = rig.function.constant(Type::floating(64),
                                             uint64_t{0x7FF8000000000000});
    CHECK(rig.val(rig.function.cast(ExprOp::FpToIntS, Type::integer(32), nan)) == c(0));
    CHECK(rig.val(rig.function.cast(
                      ExprOp::FpToIntS, Type::integer(64),
                      rig.function.constant(Type::floating(64), f64(-3.9)))) ==
          c(~2ull));  // truncates toward zero
  }

  SECTION("int-to-float and width conversion") {
    const ExprId minusTwo = rig.k(~1ull);
    CHECK(rig.val(rig.function.cast(ExprOp::IntToFpS, Type::floating(64), minusTwo)) ==
          c(f64(-2.0)));
    CHECK(rig.val(rig.function.cast(ExprOp::IntToFpU, Type::floating(64),
                                    rig.k(~0ull))) == c(f64(18446744073709551616.0)));
    const ExprId f32oneHalf = rig.function.constant(Type::floating(32), f32(1.5F));
    CHECK(rig.val(rig.function.cast(ExprOp::FpConvert, Type::floating(64),
                                    f32oneHalf)) == c(f64(1.5)));
    CHECK(rig.val(rig.function.cast(ExprOp::FpConvert, Type::floating(32),
                                    oneHalf)) == c(f32(1.5F)));
  }
}

TEST_CASE("memory: seed, delta, faults and the write set", "[il][interp]") {
  Rig rig;
  const std::array<std::byte, 8> initial{std::byte{0x11}, std::byte{0x22}, std::byte{0x33},
                                         std::byte{0x44}, std::byte{0x55}, std::byte{0x66},
                                         std::byte{0x77}, std::byte{0x88}};
  rig.interp.memory().seed(0x4000, initial);
  CHECK(rig.interp.memory().read(0x4000, 8).value() == c(0x8877665544332211));
  CHECK(rig.interp.memory().read(0x4001, 2).value() == c(0x3322));  // unaligned

  SECTION("a store goes to the delta and is reported by range") {
    REQUIRE(rig.interp.memory().write(0x4002, 4, c(0xDEADBEEF)));
    CHECK(rig.interp.memory().read(0x4000, 8).value() == c(0x8877DEADBEEF2211));
    const auto ranges = rig.interp.memory().writtenRanges();
    REQUIRE(ranges.size() == 1);
    CHECK(ranges[0].address == 0x4002);
    CHECK(ranges[0].size == 4);
  }

  SECTION("adjacent stores coalesce in the write set") {
    REQUIRE(rig.interp.memory().write(0x4002, 2, c(0xBEEF)));
    REQUIRE(rig.interp.memory().write(0x4004, 2, c(0xCAFE)));
    REQUIRE(rig.interp.memory().write(0x4010, 1, c(0x99)));
    const auto ranges = rig.interp.memory().writtenRanges();
    REQUIRE(ranges.size() == 2);
    CHECK(ranges[0].address == 0x4002);
    CHECK(ranges[0].size == 4);
    CHECK(ranges[1].address == 0x4010);
    CHECK(ranges[1].size == 1);
  }

  SECTION("clearing the delta restores the seed") {
    REQUIRE(rig.interp.memory().write(0x4000, 8, c(~0ull)));
    rig.interp.memory().clearDelta();
    CHECK(rig.interp.memory().read(0x4000, 8).value() == c(0x8877665544332211));
    CHECK(rig.interp.memory().writtenRanges().empty());
  }

  SECTION("per-run setup bytes live in the delta and stay out of the write set") {
    const std::array<std::byte, 4> setup{std::byte{1}, std::byte{2}, std::byte{3},
                                         std::byte{4}};
    rig.interp.memory().fillDelta(0x4000, setup);
    CHECK(rig.interp.memory().read(0x4000, 4).value() == c(0x04030201));
    CHECK(rig.interp.memory().writtenRanges().empty());
  }

  SECTION("unmapped accesses fault rather than invent zero") {
    CHECK_FALSE(rig.interp.memory().read(0x9000, 4).hasValue());
    CHECK_FALSE(rig.interp.memory().write(0x9000, 4, c(0)).hasValue());
    CHECK(rig.interp.memory().mapped(0x4000, 8));
    CHECK_FALSE(rig.interp.memory().mapped(0x4000, 0x2000));  // spills off the page
  }
}

TEST_CASE("register views apply on write", "[il][interp]") {
  Rig rig;
  // w1 is the low half of x1 and zero-extends on write.
  rig.interp.writeRegister(rig.reg("x1"), c(~0ull));
  rig.interp.writeRegister(rig.reg("w1"), c(0x12345678));
  CHECK(rig.interp.readRegister(rig.reg("x1")) == c(0x12345678));
  CHECK(rig.interp.readRegister(rig.reg("w1")) == c(0x12345678));

  // Writes to the zero register are discarded, reads produce zero.
  rig.interp.writeRegister(rig.reg("xzr"), c(~0ull));
  CHECK(rig.interp.readRegister(rig.reg("xzr")) == c(0));
}

TEST_CASE("block execution stops at control flow", "[il][interp]") {
  Rig rig;
  const BlockId taken = rig.function.createBlock(0x2000);
  const BlockId fall = rig.function.createBlock(0x3000);
  rig.function.block(taken).endVa = 0x2000;
  rig.function.block(fall).endVa = 0x3000;

  SECTION("an unconditional branch reports the target address") {
    rig.function.appendBranch(rig.block, 0x1000, taken);
    const auto outcome = rig.interp.runBlock(rig.block);
    CHECK(outcome.stop == ExecStop::Branch);
    CHECK(outcome.target == 0x2000);
    CHECK(outcome.va == 0x1000);
  }

  SECTION("a conditional branch reports both edges and the decision") {
    const ExprId ops[] = {rig.k(5), rig.k(5)};
    const ExprId flags = rig.function.flagDef(FlagOp::Sub, 64, ops);
    const ExprId eq = rig.function.flagCondition(flags, ConditionCode::Equal);
    rig.function.appendCondBranch(rig.block, 0x1000, eq, taken, fall);
    const auto outcome = rig.interp.runBlock(rig.block);
    CHECK(outcome.stop == ExecStop::CondBranch);
    CHECK(outcome.condition);
    CHECK(outcome.target == 0x2000);
    CHECK(outcome.fallthrough == 0x3000);
  }

  SECTION("an indirect branch reports the evaluated target") {
    rig.function.appendIndirectBranch(rig.block, 0x1000, rig.k(0xDEADBEEF));
    const auto outcome = rig.interp.runBlock(rig.block);
    CHECK(outcome.stop == ExecStop::IndirectBranch);
    CHECK(outcome.target == 0xDEADBEEF);
  }

  SECTION("a call reports its target and stops") {
    rig.function.appendCall(rig.block, 0x1000, rig.k(0x4000));
    const auto outcome = rig.interp.runBlock(rig.block);
    CHECK(outcome.stop == ExecStop::Call);
    CHECK(outcome.target == 0x4000);
  }

  SECTION("an undecodable instruction stops by name") {
    rig.function.appendUnimplemented(rig.block, 0x1000, ".word");
    const auto outcome = rig.interp.runBlock(rig.block);
    CHECK(outcome.stop == ExecStop::Unimplemented);
    CHECK(outcome.detail == ".word");
  }
}

TEST_CASE("intrinsics go through the hook or stop the block", "[il][interp]") {
  Rig rig;
  const ExprId args[] = {rig.k(7)};
  rig.function.appendIntrinsic(rig.block, 0x1000, "aarch64.dmb", Type::voidType(), args);
  rig.function.appendReturn(rig.block, 0x1004);

  SECTION("declined by default") {
    const auto outcome = rig.interp.runBlock(rig.block);
    CHECK(outcome.stop == ExecStop::Intrinsic);
    CHECK(outcome.detail == "aarch64.dmb");
  }

  SECTION("the hook can wave one through") {
    rig.interp.setIntrinsicHook(
        [](std::string_view, Type, std::span<const ConcreteValue>, ConcreteValue&) {
          return true;
        });
    CHECK(rig.interp.runBlock(rig.block).stop == ExecStop::Return);
  }

  SECTION("a result-bearing intrinsic defines a value via the hook") {
    Rig rig2;
    const ExprId args2[] = {rig2.k(0x10)};
    const xdec::il::OpId intrinsic = rig2.function.appendIntrinsic(
        rig2.block, 0x1000, "aarch64.tls.base", Type::integer(64), args2);
    const xdec::il::ValueId produced = rig2.function.op(intrinsic).result;
    rig2.function.appendWriteReg(rig2.block, 0x1004, rig2.reg("x0"),
                                 rig2.function.valueRef(produced));
    rig2.function.appendReturn(rig2.block, 0x1008);
    rig2.interp.setIntrinsicHook(
        [](std::string_view, Type, std::span<const ConcreteValue>, ConcreteValue& out) {
          out = ConcreteValue{0xCAFE, 0};
          return true;
        });
    CHECK(rig2.interp.runBlock(rig2.block).stop == ExecStop::Return);
    CHECK(rig2.interp.readRegister(rig2.reg("x0")) == c(0xCAFE));
  }
}

TEST_CASE("a load and store round trip through the IL", "[il][interp]") {
  Rig rig;
  rig.interp.memory().seed(0x8000, std::array<std::byte, 16>{});
  rig.interp.writeRegister(rig.reg("x1"), c(0x8004));
  rig.interp.writeRegister(rig.reg("x0"), c(0x1122334455667788));

  // store x0, [x1]; load x2, [x1]
  const xdec::il::ValueId address = rig.function.appendReadReg(rig.block, 0x1000, rig.reg("x1"));
  const xdec::il::ValueId value = rig.function.appendReadReg(rig.block, 0x1000, rig.reg("x0"));
  rig.function.appendStore(rig.block, 0x1000, Type::integer(64),
                           rig.function.valueRef(address), rig.function.valueRef(value));
  const xdec::il::ValueId address2 =
      rig.function.appendReadReg(rig.block, 0x1004, rig.reg("x1"));
  const xdec::il::ValueId loaded = rig.function.appendLoad(rig.block, 0x1004, Type::integer(64),
                                                           rig.function.valueRef(address2));
  rig.function.appendWriteReg(rig.block, 0x1008, rig.reg("x2"),
                              rig.function.valueRef(loaded));
  rig.function.appendReturn(rig.block, 0x100C);
  REQUIRE(rig.interp.runBlock(rig.block).stop == ExecStop::Return);
  CHECK(rig.interp.readRegister(rig.reg("x2")) == c(0x1122334455667788));

  const auto ranges = rig.interp.memory().writtenRanges();
  REQUIRE(ranges.size() == 1);
  CHECK(ranges[0].address == 0x8004);  // unaligned, as written
  CHECK(ranges[0].size == 8);
}

TEST_CASE("a memory fault inside a block is an error, not a guess", "[il][interp]") {
  Rig rig;
  const xdec::il::ValueId address = rig.function.appendReadReg(rig.block, 0x1000, rig.reg("x1"));
  const xdec::il::ValueId loaded = rig.function.appendLoad(rig.block, 0x1000, Type::integer(64),
                                                           rig.function.valueRef(address));
  rig.function.appendWriteReg(rig.block, 0x1004, rig.reg("x0"),
                              rig.function.valueRef(loaded));
  rig.function.appendReturn(rig.block, 0x1008);
  const auto outcome = rig.interp.runBlock(rig.block);
  CHECK(outcome.stop == ExecStop::Error);
  CHECK(outcome.va == 0x1000);
  CHECK(outcome.detail.find("unmapped") != std::string::npos);
}
