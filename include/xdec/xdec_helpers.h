// Semantics helpers for decompiled output.
//
// Every function xdec's C emitter can define portably and unambiguously in
// plain C lives here, under a short name (`rotr32`, `bswap64`, `cc_lt32`):
// bit rotation, byte swap, population count, and the overflow-exact
// condition codes a subtraction's N and V flags decide. A decompiled .c
// pulls this header in with one `#include` instead of repeating these
// definitions inline.
//
// The remaining declarations are stubs the emitter calls but does not
// define: count-leading/trailing-zeros at zero, multiply-high, bit
// reverse, a raw flag bit, an unresolved flag condition, and float
// arithmetic. Their exact behaviour depends on the target or on hardware
// this project does not model, so guessing would be worse than a link
// error -- the `xdec_` prefix marks them as exactly that, an embedder's
// contract to fill in, not part of this header.
#ifndef XDEC_HELPERS_H
#define XDEC_HELPERS_H

#include <stdbool.h>
#include <stdint.h>

/* ---- bit rotate: portable, fully-defined for any rotate amount ---- */

static inline uint8_t rotr8(uint8_t x, uint32_t n) {
  n &= 7u;
  return n == 0 ? x : (uint8_t)(x >> n) | (uint8_t)(x << (8 - n));
}
static inline uint8_t rotl8(uint8_t x, uint32_t n) {
  n &= 7u;
  return n == 0 ? x : (uint8_t)(x << n) | (uint8_t)(x >> (8 - n));
}
static inline uint16_t rotr16(uint16_t x, uint32_t n) {
  n &= 15u;
  return n == 0 ? x : (uint16_t)(x >> n) | (uint16_t)(x << (16 - n));
}
static inline uint16_t rotl16(uint16_t x, uint32_t n) {
  n &= 15u;
  return n == 0 ? x : (uint16_t)(x << n) | (uint16_t)(x >> (16 - n));
}
static inline uint32_t rotr32(uint32_t x, uint32_t n) {
  n &= 31u;
  return n == 0 ? x : (uint32_t)(x >> n) | (uint32_t)(x << (32 - n));
}
static inline uint32_t rotl32(uint32_t x, uint32_t n) {
  n &= 31u;
  return n == 0 ? x : (uint32_t)(x << n) | (uint32_t)(x >> (32 - n));
}
static inline uint64_t rotr64(uint64_t x, uint32_t n) {
  n &= 63u;
  return n == 0 ? x : (uint64_t)(x >> n) | (uint64_t)(x << (64 - n));
}
static inline uint64_t rotl64(uint64_t x, uint32_t n) {
  n &= 63u;
  return n == 0 ? x : (uint64_t)(x << n) | (uint64_t)(x >> (64 - n));
}

/* ---- byte swap: portable, no dependency on a compiler builtin ---- */

static inline uint8_t bswap8(uint8_t x) { return x; }
static inline uint16_t bswap16(uint16_t x) {
  return (uint16_t)((x << 8) | (x >> 8));
}
static inline uint32_t bswap32(uint32_t x) {
  return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) |
         ((x & 0x00FF0000u) >> 8) | ((x & 0xFF000000u) >> 24);
}
static inline uint64_t bswap64(uint64_t x) {
  return ((x & 0x00000000000000FFull) << 56) | ((x & 0x000000000000FF00ull) << 40) |
         ((x & 0x0000000000FF0000ull) << 24) | ((x & 0x00000000FF000000ull) << 8) |
         ((x & 0x000000FF00000000ull) >> 8) | ((x & 0x0000FF0000000000ull) >> 24) |
         ((x & 0x00FF000000000000ull) >> 40) | ((x & 0xFF00000000000000ull) >> 56);
}

/* ---- population count ---- */

static inline unsigned popcount64(uint64_t x) {
  unsigned count = 0;
  while (x) {
    x &= x - 1;
    ++count;
  }
  return count;
}

/* ---- overflow-exact condition codes ----
 * N and V from a subtraction cannot be spelled as one C comparison without
 * repeating both operands, and repeating them would double any
 * side-effect-free but large expression at every use.
 */

static inline bool cc_ge8(uint8_t a, uint8_t b) {
  const int8_t r = (int8_t)(a - b);
  const bool n = r < 0;
  const bool v = (((int8_t)a >= 0) != ((int8_t)b >= 0)) && ((r < 0) != ((int8_t)a >= 0));
  return n == v;
}
static inline bool cc_lt8(uint8_t a, uint8_t b) {
  const int8_t r = (int8_t)(a - b);
  const bool n = r < 0;
  const bool v = (((int8_t)a >= 0) != ((int8_t)b >= 0)) && ((r < 0) != ((int8_t)a >= 0));
  return n != v;
}
static inline bool cc_gt8(uint8_t a, uint8_t b) {
  const int8_t r = (int8_t)(a - b);
  const bool n = r < 0;
  const bool v = (((int8_t)a >= 0) != ((int8_t)b >= 0)) && ((r < 0) != ((int8_t)a >= 0));
  return r != 0 && n == v;
}
static inline bool cc_le8(uint8_t a, uint8_t b) {
  const int8_t r = (int8_t)(a - b);
  const bool n = r < 0;
  const bool v = (((int8_t)a >= 0) != ((int8_t)b >= 0)) && ((r < 0) != ((int8_t)a >= 0));
  return r == 0 || n != v;
}
static inline bool cc_vs8(uint8_t a, uint8_t b) {
  const int8_t r = (int8_t)(a - b);
  (void)r;
  return (((int8_t)a >= 0) != ((int8_t)b >= 0)) && ((r < 0) != ((int8_t)a >= 0));
}
static inline bool cc_vc8(uint8_t a, uint8_t b) { return !cc_vs8(a, b); }

static inline bool cc_ge16(uint16_t a, uint16_t b) {
  const int16_t r = (int16_t)(a - b);
  const bool n = r < 0;
  const bool v = (((int16_t)a >= 0) != ((int16_t)b >= 0)) && ((r < 0) != ((int16_t)a >= 0));
  return n == v;
}
static inline bool cc_lt16(uint16_t a, uint16_t b) {
  const int16_t r = (int16_t)(a - b);
  const bool n = r < 0;
  const bool v = (((int16_t)a >= 0) != ((int16_t)b >= 0)) && ((r < 0) != ((int16_t)a >= 0));
  return n != v;
}
static inline bool cc_gt16(uint16_t a, uint16_t b) {
  const int16_t r = (int16_t)(a - b);
  const bool n = r < 0;
  const bool v = (((int16_t)a >= 0) != ((int16_t)b >= 0)) && ((r < 0) != ((int16_t)a >= 0));
  return r != 0 && n == v;
}
static inline bool cc_le16(uint16_t a, uint16_t b) {
  const int16_t r = (int16_t)(a - b);
  const bool n = r < 0;
  const bool v = (((int16_t)a >= 0) != ((int16_t)b >= 0)) && ((r < 0) != ((int16_t)a >= 0));
  return r == 0 || n != v;
}
static inline bool cc_vs16(uint16_t a, uint16_t b) {
  const int16_t r = (int16_t)(a - b);
  (void)r;
  return (((int16_t)a >= 0) != ((int16_t)b >= 0)) && ((r < 0) != ((int16_t)a >= 0));
}
static inline bool cc_vc16(uint16_t a, uint16_t b) { return !cc_vs16(a, b); }

static inline bool cc_ge32(uint32_t a, uint32_t b) {
  const int32_t r = (int32_t)(a - b);
  const bool n = r < 0;
  const bool v = (((int32_t)a >= 0) != ((int32_t)b >= 0)) && ((r < 0) != ((int32_t)a >= 0));
  return n == v;
}
static inline bool cc_lt32(uint32_t a, uint32_t b) {
  const int32_t r = (int32_t)(a - b);
  const bool n = r < 0;
  const bool v = (((int32_t)a >= 0) != ((int32_t)b >= 0)) && ((r < 0) != ((int32_t)a >= 0));
  return n != v;
}
static inline bool cc_gt32(uint32_t a, uint32_t b) {
  const int32_t r = (int32_t)(a - b);
  const bool n = r < 0;
  const bool v = (((int32_t)a >= 0) != ((int32_t)b >= 0)) && ((r < 0) != ((int32_t)a >= 0));
  return r != 0 && n == v;
}
static inline bool cc_le32(uint32_t a, uint32_t b) {
  const int32_t r = (int32_t)(a - b);
  const bool n = r < 0;
  const bool v = (((int32_t)a >= 0) != ((int32_t)b >= 0)) && ((r < 0) != ((int32_t)a >= 0));
  return r == 0 || n != v;
}
static inline bool cc_vs32(uint32_t a, uint32_t b) {
  const int32_t r = (int32_t)(a - b);
  (void)r;
  return (((int32_t)a >= 0) != ((int32_t)b >= 0)) && ((r < 0) != ((int32_t)a >= 0));
}
static inline bool cc_vc32(uint32_t a, uint32_t b) { return !cc_vs32(a, b); }

static inline bool cc_ge64(uint64_t a, uint64_t b) {
  const int64_t r = (int64_t)(a - b);
  const bool n = r < 0;
  const bool v = (((int64_t)a >= 0) != ((int64_t)b >= 0)) && ((r < 0) != ((int64_t)a >= 0));
  return n == v;
}
static inline bool cc_lt64(uint64_t a, uint64_t b) {
  const int64_t r = (int64_t)(a - b);
  const bool n = r < 0;
  const bool v = (((int64_t)a >= 0) != ((int64_t)b >= 0)) && ((r < 0) != ((int64_t)a >= 0));
  return n != v;
}
static inline bool cc_gt64(uint64_t a, uint64_t b) {
  const int64_t r = (int64_t)(a - b);
  const bool n = r < 0;
  const bool v = (((int64_t)a >= 0) != ((int64_t)b >= 0)) && ((r < 0) != ((int64_t)a >= 0));
  return r != 0 && n == v;
}
static inline bool cc_le64(uint64_t a, uint64_t b) {
  const int64_t r = (int64_t)(a - b);
  const bool n = r < 0;
  const bool v = (((int64_t)a >= 0) != ((int64_t)b >= 0)) && ((r < 0) != ((int64_t)a >= 0));
  return r == 0 || n != v;
}
static inline bool cc_vs64(uint64_t a, uint64_t b) {
  const int64_t r = (int64_t)(a - b);
  (void)r;
  return (((int64_t)a >= 0) != ((int64_t)b >= 0)) && ((r < 0) != ((int64_t)a >= 0));
}
static inline bool cc_vc64(uint64_t a, uint64_t b) { return !cc_vs64(a, b); }

/* ---- embedder-supplied stubs ----
 * xdec calls these but does not define them: the zero-input result of
 * clz/ctz, multiply-high, bit reverse, a raw flag bit, an unresolved flag
 * condition, and float arithmetic all depend on the target or on hardware
 * this project does not model. Link a definition of whichever of these the
 * decompiled body actually calls.
 */

uint8_t xdec_clz8(uint8_t x);
uint16_t xdec_clz16(uint16_t x);
uint32_t xdec_clz32(uint32_t x);
uint64_t xdec_clz64(uint64_t x);

uint8_t xdec_ctz8(uint8_t x);
uint16_t xdec_ctz16(uint16_t x);
uint32_t xdec_ctz32(uint32_t x);
uint64_t xdec_ctz64(uint64_t x);

uint8_t xdec_brev8(uint8_t x);
uint16_t xdec_brev16(uint16_t x);
uint32_t xdec_brev32(uint32_t x);
uint64_t xdec_brev64(uint64_t x);

uint8_t xdec_mulhiu8(uint8_t a, uint8_t b);
uint16_t xdec_mulhiu16(uint16_t a, uint16_t b);
uint32_t xdec_mulhiu32(uint32_t a, uint32_t b);
uint64_t xdec_mulhiu64(uint64_t a, uint64_t b);

int8_t xdec_mulhis8(int8_t a, int8_t b);
int16_t xdec_mulhis16(int16_t a, int16_t b);
int32_t xdec_mulhis32(int32_t a, int32_t b);
int64_t xdec_mulhis64(int64_t a, int64_t b);

/* A raw NZCV bit the machine code read directly, by FlagBitIndex. */
bool xdec_flagbit(uint64_t flags, unsigned index);
/* A flag condition none of the analyses could fold into a comparison. */
bool xdec_flagcond_stub(uint64_t a, uint64_t b);

float xdec_fadd32(float a, float b);
double xdec_fadd64(double a, double b);
float xdec_fsub32(float a, float b);
double xdec_fsub64(double a, double b);
float xdec_fmul32(float a, float b);
double xdec_fmul64(double a, double b);
float xdec_fdiv32(float a, float b);
double xdec_fdiv64(double a, double b);
float xdec_fneg32(float a);
double xdec_fneg64(double a);

#endif  // XDEC_HELPERS_H
