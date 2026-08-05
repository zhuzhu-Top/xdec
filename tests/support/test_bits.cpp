#include <catch2/catch_test_macros.hpp>

#include "xdec/support/bits.h"

using namespace xdec;

TEST_CASE("lowMask saturates at 64 bits", "[bits]") {
  CHECK(lowMask(0) == 0);
  CHECK(lowMask(1) == 0x1);
  CHECK(lowMask(12) == 0xFFF);
  CHECK(lowMask(63) == 0x7FFFFFFFFFFFFFFFull);
  // A shift by 64 would be undefined behaviour, so the boundary matters.
  CHECK(lowMask(64) == ~uint64_t{0});
  CHECK(lowMask(200) == ~uint64_t{0});
}

TEST_CASE("signExtend widens the sign bit", "[bits]") {
  CHECK(signExtend(0x7F, 8) == 127);
  CHECK(signExtend(0x80, 8) == -128);
  CHECK(signExtend(0xFF, 8) == -1);
  CHECK(signExtend(0xFFF, 12) == -1);
  CHECK(signExtend(0x800, 12) == -2048);
  CHECK(signExtend(0x7FF, 12) == 2047);

  // Width 64 and above is already a full-width value.
  CHECK(signExtend(0xFFFFFFFFFFFFFFFFull, 64) == -1);
  // Width 0 has no sign bit to extend.
  CHECK(signExtend(0x1234, 0) == 0x1234);
}

TEST_CASE("signExtendTo truncates after extending", "[bits]") {
  // An imm9 of -1 extended into a 32-bit register is 0xFFFFFFFF, not -1.
  CHECK(signExtendTo(0x1FF, 9, 32) == 0xFFFFFFFFull);
  CHECK(signExtendTo(0x1FF, 9, 64) == 0xFFFFFFFFFFFFFFFFull);
  CHECK(signExtendTo(0x0FF, 9, 32) == 0xFF);
}

TEST_CASE("extractBits reads a field", "[bits]") {
  constexpr uint64_t encoding = 0xF1234567;
  CHECK(extractBits(encoding, 0, 4) == 0x7);
  CHECK(extractBits(encoding, 4, 4) == 0x6);
  CHECK(extractBits(encoding, 28, 4) == 0xF);
  CHECK(extractBits(encoding, 0, 64) == encoding);
  // Reads entirely above the value are zero rather than undefined.
  CHECK(extractBits(encoding, 64, 8) == 0);
  CHECK(extractBits(encoding, 100, 8) == 0);
}

TEST_CASE("insertBits replaces a field", "[bits]") {
  CHECK(insertBits(0xFFFF, 0x0, 4, 4) == 0xFF0F);
  CHECK(insertBits(0x0000, 0xA, 8, 4) == 0x0A00);
  // Bits of the field above the declared width are discarded.
  CHECK(insertBits(0x0000, 0xFF, 0, 4) == 0x000F);
}

TEST_CASE("testBit is defined beyond the value width", "[bits]") {
  CHECK(testBit(0x8000000000000000ull, 63));
  CHECK_FALSE(testBit(0x8000000000000000ull, 62));
  CHECK_FALSE(testBit(~uint64_t{0}, 64));
  CHECK_FALSE(testBit(~uint64_t{0}, 200));
}

TEST_CASE("rotateRight honours the declared width", "[bits]") {
  CHECK(rotateRight(0x1, 8, 1) == 0x80);
  CHECK(rotateRight(0x80, 8, 1) == 0x40);
  CHECK(rotateRight(0x12345678, 32, 8) == 0x78123456);
  CHECK(rotateRight(0x1, 32, 1) == 0x80000000);
  // Rotating by the full width, or a multiple of it, is the identity.
  CHECK(rotateRight(0xABCD, 16, 16) == 0xABCD);
  CHECK(rotateRight(0xABCD, 16, 32) == 0xABCD);
  CHECK(rotateRight(0xABCD, 16, 0) == 0xABCD);
}

TEST_CASE("rotateLeft is the inverse of rotateRight", "[bits]") {
  for (unsigned amount = 0; amount < 32; ++amount) {
    CHECK(rotateRight(rotateLeft(0xDEADBEEF, 32, amount), 32, amount) == 0xDEADBEEF);
  }
  CHECK(rotateLeft(0x80, 8, 1) == 0x01);
}

TEST_CASE("replicate tiles an element", "[bits]") {
  // This is what the AArch64 logical-immediate decoder needs.
  CHECK(replicate(0x1, 2, 8) == 0b01010101);
  CHECK(replicate(0xF, 8, 32) == 0x0F0F0F0F);
  CHECK(replicate(0x1, 1, 64) == ~uint64_t{0});
  CHECK(replicate(0xAB, 8, 64) == 0xABABABABABABABABull);
  // An element as wide as the total is copied once.
  CHECK(replicate(0x1234, 16, 16) == 0x1234);
}

TEST_CASE("counting helpers respect the declared width", "[bits]") {
  CHECK(countOnes(0xFF) == 8);
  CHECK(countTrailingZeros(0x100) == 8);
  // Zero has no set bit; report the full 64 rather than an undefined result.
  CHECK(countTrailingZeros(0) == 64);

  CHECK(countLeadingZeros(0x1, 8) == 7);
  CHECK(countLeadingZeros(0x1, 32) == 31);
  CHECK(countLeadingZeros(0x0, 32) == 32);
  CHECK(countLeadingZeros(0x80, 8) == 0);

  CHECK(significantBits(0) == 0);
  CHECK(significantBits(1) == 1);
  CHECK(significantBits(0xFF) == 8);
  CHECK(significantBits(0x100) == 9);
}

TEST_CASE("byteSwap reverses byte order", "[bits]") {
  CHECK(byteSwap(uint16_t{0x1234}) == 0x3412);
  CHECK(byteSwap(uint32_t{0x12345678}) == 0x78563412);
  CHECK(byteSwap(uint64_t{0x0123456789ABCDEFull}) == 0xEFCDAB8967452301ull);
  CHECK(byteSwapWidth(0x12345678, 32) == 0x78563412);
  CHECK(byteSwapWidth(0x1234, 16) == 0x3412);
}

TEST_CASE("loadLittle and loadBig read unaligned bytes", "[bits]") {
  // Deliberately offset by one to exercise the unaligned path.
  const std::byte raw[9] = {std::byte{0x00}, std::byte{0x78}, std::byte{0x56}, std::byte{0x34},
                            std::byte{0x12}, std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC},
                            std::byte{0xDD}};
  CHECK(loadLittle<uint32_t>(raw + 1) == 0x12345678);
  CHECK(loadBig<uint32_t>(raw + 1) == 0x78563412);
}

TEST_CASE("alignment helpers round to power-of-two boundaries", "[bits]") {
  CHECK(alignUp(0, 16) == 0);
  CHECK(alignUp(1, 16) == 16);
  CHECK(alignUp(16, 16) == 16);
  CHECK(alignUp(17, 16) == 32);
  CHECK(alignDown(17, 16) == 16);
  CHECK(alignDown(16, 16) == 16);
  CHECK(isPowerOfTwo(1));
  CHECK(isPowerOfTwo(4096));
  CHECK_FALSE(isPowerOfTwo(0));
  CHECK_FALSE(isPowerOfTwo(6));
}
