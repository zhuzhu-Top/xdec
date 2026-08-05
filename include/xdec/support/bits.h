// Bit manipulation primitives shared by the ELF reader, the instruction
// decoder and the IL constant folder.
//
// Every operation is defined for a caller-supplied bit width rather than a C++
// type, because IL values carry their width as data (a 12-bit immediate, a
// 33-bit intermediate). Widths are always in [0, 64].
#pragma once

#include <bit>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "xdec/support/compiler.h"

namespace xdec {

inline constexpr unsigned kMaxIntWidth = 64;

/// Mask with the low `width` bits set; `width >= 64` yields all ones.
[[nodiscard]] constexpr uint64_t lowMask(unsigned width) noexcept {
  return width >= 64 ? ~uint64_t{0} : ((uint64_t{1} << width) - 1);
}

[[nodiscard]] constexpr uint64_t zeroExtend(uint64_t value, unsigned width) noexcept {
  return value & lowMask(width);
}

/// Sign-extends the low `width` bits of `value` to a full 64-bit signed value.
[[nodiscard]] constexpr int64_t signExtend(uint64_t value, unsigned width) noexcept {
  if (width == 0 || width >= 64) {
    return static_cast<int64_t>(value);
  }
  const uint64_t masked = value & lowMask(width);
  const uint64_t signBit = uint64_t{1} << (width - 1);
  return static_cast<int64_t>((masked ^ signBit) - signBit);
}

/// Sign-extends and re-truncates to `toWidth`, the IL's `sext` semantics.
[[nodiscard]] constexpr uint64_t signExtendTo(uint64_t value, unsigned fromWidth,
                                              unsigned toWidth) noexcept {
  return zeroExtend(static_cast<uint64_t>(signExtend(value, fromWidth)), toWidth);
}

[[nodiscard]] constexpr uint64_t extractBits(uint64_t value, unsigned lo,
                                             unsigned width) noexcept {
  return lo >= 64 ? uint64_t{0} : zeroExtend(value >> lo, width);
}

/// Single-bit test; bits at or above 64 read as zero.
[[nodiscard]] constexpr bool testBit(uint64_t value, unsigned bit) noexcept {
  return bit < 64 && ((value >> bit) & 1) != 0;
}

[[nodiscard]] constexpr uint64_t insertBits(uint64_t dest, uint64_t field, unsigned lo,
                                            unsigned width) noexcept {
  const uint64_t mask = lowMask(width) << lo;
  return (dest & ~mask) | ((field << lo) & mask);
}

[[nodiscard]] constexpr uint64_t rotateRight(uint64_t value, unsigned width,
                                             unsigned amount) noexcept {
  if (width == 0) {
    return 0;
  }
  const uint64_t v = zeroExtend(value, width);
  const unsigned amt = amount % width;
  if (amt == 0) {
    return v;
  }
  return zeroExtend((v >> amt) | (v << (width - amt)), width);
}

[[nodiscard]] constexpr uint64_t rotateLeft(uint64_t value, unsigned width,
                                            unsigned amount) noexcept {
  if (width == 0) {
    return 0;
  }
  return rotateRight(value, width, width - (amount % width));
}

/// Repeats the low `elementWidth` bits of `value` to fill `totalWidth` bits.
/// Used by the AArch64 logical-immediate decoder.
[[nodiscard]] constexpr uint64_t replicate(uint64_t value, unsigned elementWidth,
                                           unsigned totalWidth) noexcept {
  if (elementWidth == 0 || elementWidth > 64) {
    return 0;
  }
  const uint64_t element = zeroExtend(value, elementWidth);
  uint64_t result = 0;
  for (unsigned shift = 0; shift < totalWidth; shift += elementWidth) {
    result |= element << shift;
  }
  return zeroExtend(result, totalWidth);
}

[[nodiscard]] constexpr bool isPowerOfTwo(uint64_t value) noexcept {
  return value != 0 && (value & (value - 1)) == 0;
}

[[nodiscard]] constexpr uint64_t alignUp(uint64_t value, uint64_t alignment) noexcept {
  XDEC_DASSERT(isPowerOfTwo(alignment), "alignment must be a power of two");
  return (value + alignment - 1) & ~(alignment - 1);
}

[[nodiscard]] constexpr uint64_t alignDown(uint64_t value, uint64_t alignment) noexcept {
  XDEC_DASSERT(isPowerOfTwo(alignment), "alignment must be a power of two");
  return value & ~(alignment - 1);
}

[[nodiscard]] constexpr unsigned countOnes(uint64_t value) noexcept {
  return static_cast<unsigned>(std::popcount(value));
}

[[nodiscard]] constexpr unsigned countTrailingZeros(uint64_t value) noexcept {
  return value == 0 ? 64u : static_cast<unsigned>(std::countr_zero(value));
}

[[nodiscard]] constexpr unsigned countLeadingZeros(uint64_t value, unsigned width) noexcept {
  if (width == 0 || width > 64) {
    return 0;
  }
  const uint64_t v = zeroExtend(value, width);
  if (v == 0) {
    return width;
  }
  return static_cast<unsigned>(std::countl_zero(v)) - (64 - width);
}

/// Smallest number of bits needed to hold `value` unsigned; 0 for value 0.
[[nodiscard]] constexpr unsigned significantBits(uint64_t value) noexcept {
  return value == 0 ? 0u : static_cast<unsigned>(64 - std::countl_zero(value));
}

// ---------------------------------------------------------------------------
// Byte order
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr uint8_t byteSwap(uint8_t value) noexcept { return value; }

[[nodiscard]] constexpr uint16_t byteSwap(uint16_t value) noexcept {
  return static_cast<uint16_t>((value >> 8) | (value << 8));
}

[[nodiscard]] constexpr uint32_t byteSwap(uint32_t value) noexcept {
  return ((value & 0x000000FFu) << 24) | ((value & 0x0000FF00u) << 8) |
         ((value & 0x00FF0000u) >> 8) | ((value & 0xFF000000u) >> 24);
}

[[nodiscard]] constexpr uint64_t byteSwap(uint64_t value) noexcept {
  return (static_cast<uint64_t>(byteSwap(static_cast<uint32_t>(value))) << 32) |
         static_cast<uint64_t>(byteSwap(static_cast<uint32_t>(value >> 32)));
}

/// Reverses the low `width` bytes of `value`; `width` must be a whole number of
/// bytes. Backs the IL `bswap` operation.
[[nodiscard]] constexpr uint64_t byteSwapWidth(uint64_t value, unsigned width) noexcept {
  XDEC_DASSERT(width % 8 == 0 && width <= 64, "bswap width must be a byte multiple");
  return byteSwap(value) >> (64 - width);
}

template <class T>
[[nodiscard]] T loadUnaligned(const std::byte* source) noexcept {
  static_assert(std::is_trivially_copyable_v<T>, "loadUnaligned requires a trivial type");
  T value{};
  std::memcpy(&value, source, sizeof(T));
  return value;
}

template <class T>
void storeUnaligned(std::byte* dest, T value) noexcept {
  static_assert(std::is_trivially_copyable_v<T>, "storeUnaligned requires a trivial type");
  std::memcpy(dest, &value, sizeof(T));
}

template <class T>
[[nodiscard]] T loadLittle(const std::byte* source) noexcept {
  T value = loadUnaligned<T>(source);
  if constexpr (std::endian::native == std::endian::big) {
    value = static_cast<T>(byteSwap(value));
  }
  return value;
}

template <class T>
[[nodiscard]] T loadBig(const std::byte* source) noexcept {
  T value = loadUnaligned<T>(source);
  if constexpr (std::endian::native == std::endian::little) {
    value = static_cast<T>(byteSwap(value));
  }
  return value;
}

}  // namespace xdec
