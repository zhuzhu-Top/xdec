// 128-bit unsigned arithmetic on (lo, hi) pairs — internal to the IL library.
//
// The IL's expression widths run to 128 (a q register, the concat inside
// `extr`). Everything here is defined for a width parameter rather than a C++
// type, and everything masks on the way out, so a value never carries garbage
// above its width into the next operation.
//
// Shared by the interpreter (src/il/interp.cpp) and the constant evaluator
// (src/il/ceval.cpp) so that folding and executing can never drift apart.
#pragma once

#include <bit>
#include <cstdint>
#include <utility>

#include "xdec/support/bits.h"

namespace xdec::il {

struct U128 {
  uint64_t lo = 0;
  uint64_t hi = 0;
};

[[nodiscard]] constexpr U128 maskOf(unsigned width) noexcept {
  return U128{width >= 64 ? ~uint64_t{0} : lowMask(width),
              width <= 64 ? 0 : lowMask(width - 64)};
}

[[nodiscard]] constexpr U128 mask(U128 v, unsigned width) noexcept {
  const U128 m = maskOf(width);
  return U128{v.lo & m.lo, v.hi & m.hi};
}

[[nodiscard]] constexpr bool signBit(U128 v, unsigned width) noexcept {
  return width > 64 ? ((v.hi >> (width - 65)) & 1) != 0
                    : ((v.lo >> (width - 1)) & 1) != 0;
}

[[nodiscard]] constexpr U128 add(U128 a, U128 b, unsigned width, bool& carryOut) noexcept {
  U128 r{a.lo + b.lo, 0};
  const bool carryLo = r.lo < a.lo;
  const uint64_t hiSum = a.hi + b.hi;
  const bool c1 = hiSum < a.hi;
  r.hi = hiSum + (carryLo ? 1 : 0);
  const bool c2 = r.hi < hiSum;
  carryOut = c1 || c2;
  return mask(r, width);
}

[[nodiscard]] constexpr U128 sub(U128 a, U128 b, unsigned width) noexcept {
  return mask(U128{a.lo - b.lo, a.hi - b.hi - (a.lo < b.lo ? 1 : 0)}, width);
}

[[nodiscard]] constexpr U128 shl(U128 v, unsigned width, unsigned amount) noexcept {
  if (amount == 0) {
    return mask(v, width);
  }
  if (amount >= width) {
    return U128{};
  }
  if (amount >= 64) {
    return mask(U128{0, v.lo << (amount - 64)}, width);
  }
  return mask(U128{v.lo << amount, (v.hi << amount) | (v.lo >> (64 - amount))}, width);
}

[[nodiscard]] constexpr U128 shrU(U128 v, unsigned width, unsigned amount) noexcept {
  if (amount == 0) {
    return mask(v, width);
  }
  if (amount >= width) {
    return U128{};
  }
  if (amount >= 64) {
    return U128{v.hi >> (amount - 64), 0};
  }
  return U128{(v.lo >> amount) | (v.hi << (64 - amount)), v.hi >> amount};
}

[[nodiscard]] constexpr U128 shrS(U128 v, unsigned width, unsigned amount) noexcept {
  const bool sign = signBit(v, width);
  if (amount >= width) {
    return sign ? maskOf(width) : U128{};
  }
  U128 r = shrU(v, width, amount);
  if (sign && amount > 0) {
    // Fill the vacated high bits with ones.
    const U128 fill = shl(maskOf(width), width, width - amount);
    r = U128{r.lo | fill.lo, r.hi | fill.hi};
  }
  return mask(r, width);
}

[[nodiscard]] constexpr U128 rotR(U128 v, unsigned width, unsigned amount) noexcept {
  const unsigned n = amount % width;
  if (n == 0) {
    return mask(v, width);
  }
  const U128 right = shrU(v, width, n);
  const U128 left = shl(v, width, width - n);
  return mask(U128{right.lo | left.lo, right.hi | left.hi}, width);
}

[[nodiscard]] constexpr U128 rotL(U128 v, unsigned width, unsigned amount) noexcept {
  return rotR(v, width, width - (amount % width));
}

[[nodiscard]] constexpr int cmpU(U128 a, U128 b) noexcept {
  if (a.hi != b.hi) {
    return a.hi < b.hi ? -1 : 1;
  }
  if (a.lo != b.lo) {
    return a.lo < b.lo ? -1 : 1;
  }
  return 0;
}

[[nodiscard]] constexpr int cmpS(U128 a, U128 b, unsigned width) noexcept {
  // Flip the sign bit of both operands and compare unsigned.
  const U128 flip = shl(U128{1, 0}, width, width - 1);
  return cmpU(U128{a.lo ^ flip.lo, a.hi ^ flip.hi}, U128{b.lo ^ flip.lo, b.hi ^ flip.hi});
}

/// Portable 64 x 64 -> 128 multiply, in 32-bit limbs.
[[nodiscard]] constexpr std::pair<uint64_t, uint64_t> mul64x64(uint64_t a, uint64_t b) noexcept {
  const uint64_t a0 = static_cast<uint32_t>(a);
  const uint64_t a1 = a >> 32;
  const uint64_t b0 = static_cast<uint32_t>(b);
  const uint64_t b1 = b >> 32;
  const uint64_t p0 = a0 * b0;
  const uint64_t p1 = a0 * b1;
  const uint64_t p2 = a1 * b0;
  const uint64_t p3 = a1 * b1;
  const uint64_t mid = p1 + p2;
  const uint64_t midCarry = mid < p1 ? 1 : 0;
  const uint64_t lo = p0 + (mid << 32);
  const uint64_t loCarry = lo < p0 ? 1 : 0;
  const uint64_t hi = p3 + (mid >> 32) + (midCarry << 32) + loCarry;
  return {hi, lo};
}

/// Low half of a 128-bit product. Only the low 64 bits of each cross term
/// contribute, so this is three 64-bit multiplies.
[[nodiscard]] constexpr U128 mulLow(U128 a, U128 b, unsigned width) noexcept {
  const auto [hi, lo] = mul64x64(a.lo, b.lo);
  return mask(U128{lo, hi + a.lo * b.hi + a.hi * b.lo}, width);
}

/// Field extraction: bits [offset, offset + width) of the 128-bit source.
[[nodiscard]] constexpr U128 extract(U128 source, unsigned sourceWidth, unsigned offset,
                                     unsigned width) noexcept {
  return mask(shrU(source, sourceWidth, offset), width);
}

[[nodiscard]] constexpr U128 insert(U128 dest, U128 field, unsigned offset, unsigned width,
                                    unsigned destWidth) noexcept {
  const U128 shifted = shl(field, destWidth, offset);
  const U128 cleared = mask(shl(maskOf(width), destWidth, offset), destWidth);
  const U128 kept{dest.lo & ~cleared.lo, dest.hi & ~cleared.hi};
  return mask(U128{kept.lo | shifted.lo, kept.hi | shifted.hi}, destWidth);
}

[[nodiscard]] constexpr U128 zext(U128 v, unsigned fromWidth, unsigned toWidth) noexcept {
  (void)toWidth;  // the mask at fromWidth is the extension
  return mask(v, fromWidth);
}

[[nodiscard]] constexpr U128 sext(U128 v, unsigned fromWidth, unsigned toWidth) noexcept {
  v = mask(v, fromWidth);
  if (!signBit(v, fromWidth)) {
    return v;
  }
  const U128 fill{maskOf(fromWidth).lo ^ maskOf(toWidth).lo,
                  maskOf(fromWidth).hi ^ maskOf(toWidth).hi};
  return mask(U128{v.lo | fill.lo, v.hi | fill.hi}, toWidth);
}

[[nodiscard]] inline unsigned clzOf(U128 v, unsigned width) noexcept {
  v = mask(v, width);
  if (width > 64) {
    if (v.hi != 0) {
      return static_cast<unsigned>(std::countl_zero(v.hi)) - (128 - width);
    }
    return width - 64 + static_cast<unsigned>(std::countl_zero(v.lo));
  }
  return countLeadingZeros(v.lo, width);
}

[[nodiscard]] inline unsigned ctzOf(U128 v, unsigned width) noexcept {
  v = mask(v, width);
  if (v.lo != 0) {
    return static_cast<unsigned>(std::countr_zero(v.lo));
  }
  if (v.hi != 0 && width > 64) {
    return 64 + static_cast<unsigned>(std::countr_zero(v.hi));
  }
  return width;
}

[[nodiscard]] inline U128 bswapOf(U128 v, unsigned width) noexcept {
  // Reverse the order of the width/8 bytes.
  U128 result{};
  const unsigned bytes = width / 8;
  for (unsigned index = 0; index < bytes; ++index) {
    const unsigned shift = (bytes - 1 - index) * 8;
    const uint64_t byte = index < 8 ? (v.lo >> (index * 8)) & 0xFF : (v.hi >> ((index - 8) * 8)) & 0xFF;
    if (shift < 64) {
      result.lo |= byte << shift;
    } else {
      result.hi |= byte << (shift - 64);
    }
  }
  return mask(result, width);
}

[[nodiscard]] inline U128 brevOf(U128 v, unsigned width) noexcept {
  U128 result{};
  for (unsigned bit = 0; bit < width; ++bit) {
    const bool set = bit < 64 ? ((v.lo >> bit) & 1) != 0 : ((v.hi >> (bit - 64)) & 1) != 0;
    if (!set) {
      continue;
    }
    const unsigned target = width - 1 - bit;
    if (target < 64) {
      result.lo |= uint64_t{1} << target;
    } else {
      result.hi |= uint64_t{1} << (target - 64);
    }
  }
  return result;
}

}  // namespace xdec::il
