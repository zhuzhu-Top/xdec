// Ground-truth functions for xdec evaluation. Each eval_* symbol is one
// isolated test case; the manifest lists what readable C should contain.
//
// Compiled with NDK -O1 so control flow is recognisable but not fully inlined.

#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Helpers (called from other cases — not listed in manifest)
// ---------------------------------------------------------------------------

static int32_t helper_double(int32_t x) { return x + x; }

static int32_t helper_triple(int32_t x) { return x + x + x; }

typedef int32_t (*binop_fn)(int32_t, int32_t);

static int32_t helper_add(int32_t a, int32_t b) { return a + b; }

static int32_t helper_sub(int32_t a, int32_t b) { return a - b; }

// ---------------------------------------------------------------------------
// Basic arithmetic
// ---------------------------------------------------------------------------

int32_t eval_add_three(int32_t a, int32_t b, int32_t c) { return a + b + c; }

uint32_t eval_bitwise_mix(uint32_t x, uint32_t y) {
  return (x & y) | ((x ^ y) << 1) | (x >> 3);
}

uint32_t eval_rotr32(uint32_t x, unsigned n) {
  n &= 31;
  return (x >> n) | (x << (32 - n));
}

// ---------------------------------------------------------------------------
// Conditionals
// ---------------------------------------------------------------------------

int32_t eval_abs(int32_t x) {
  if (x < 0) {
    return -x;
  }
  return x;
}

int32_t eval_max(int32_t a, int32_t b) {
  if (a >= b) {
    return a;
  }
  return b;
}

int32_t eval_clamp(int32_t x, int32_t lo, int32_t hi) {
  if (x < lo) {
    return lo;
  }
  if (x > hi) {
    return hi;
  }
  return x;
}

// ---------------------------------------------------------------------------
// Loops
// ---------------------------------------------------------------------------

int32_t eval_sum_array(const int32_t* arr, size_t n) {
  int32_t sum = 0;
  size_t i = 0;
  while (i < n) {
    sum += arr[i];
    ++i;
  }
  return sum;
}

uint32_t eval_factorial(unsigned n) {
  uint32_t acc = 1;
  unsigned i = 2;
  do {
    acc *= i;
    ++i;
  } while (i <= n);
  return acc;
}

int32_t eval_count_bits(uint32_t x) {
  int32_t count = 0;
  while (x != 0) {
    count += (int32_t)(x & 1u);
    x >>= 1;
  }
  return count;
}

// ---------------------------------------------------------------------------
// Switch (dense — should become switch or if/else chain, not goto soup)
// ---------------------------------------------------------------------------

int32_t eval_switch_arith(int32_t op, int32_t a, int32_t b) {
  switch (op) {
    case 0:
      return a + b;
    case 1:
      return a - b;
    case 2:
      return a * b;
    case 3:
      return b != 0 ? a / b : 0;
    case 4:
      return a ^ b;
    case 5:
      return a & b;
    case 6:
      return a | b;
    default:
      return -1;
  }
}

// Sparse switch — tests jump-table vs compare tree
int32_t eval_switch_sparse(int32_t code) {
  switch (code) {
    case 0x10:
      return 1;
    case 0x20:
      return 2;
    case 0x30:
      return 3;
    case 0x40:
      return 4;
    case 0x50:
      return 5;
    default:
      return 0;
  }
}

// ---------------------------------------------------------------------------
// Memory / pointers
// ---------------------------------------------------------------------------

typedef struct {
  int32_t x;
  int32_t y;
  int32_t z;
} Vec3;

int32_t eval_vec3_dot(const Vec3* v, const Vec3* w) {
  return v->x * w->x + v->y * w->y + v->z * w->z;
}

int32_t eval_array_max(const int32_t* arr, size_t n) {
  if (n == 0) {
    return 0;
  }
  int32_t best = arr[0];
  for (size_t i = 1; i < n; ++i) {
    if (arr[i] > best) {
      best = arr[i];
    }
  }
  return best;
}

// ---------------------------------------------------------------------------
// Calls
// ---------------------------------------------------------------------------

int32_t eval_call_chain(int32_t x) { return helper_triple(helper_double(x)); }

int32_t eval_indirect_binop(int32_t op, int32_t a, int32_t b) {
  binop_fn fn = (op & 1) ? helper_add : helper_sub;
  return fn(a, b);
}

// ---------------------------------------------------------------------------
// Signed comparisons and sign extension
// ---------------------------------------------------------------------------

int32_t eval_sign_extend_chain(int32_t x) {
  int16_t narrow = (int16_t)x;
  if (narrow < 0) {
    return (int32_t)narrow - 0x10000;
  }
  return (int32_t)narrow;
}

int32_t eval_cmp_signed(int32_t a, int32_t b) {
  if (a < b) {
    return -1;
  }
  if (a > b) {
    return 1;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Nested control flow
// ---------------------------------------------------------------------------

int32_t eval_nested(int32_t a, int32_t b, int32_t c) {
  int32_t out = 0;
  if (a > 0) {
    for (int32_t i = 0; i < b; ++i) {
      if ((i & 1) != 0) {
        out += c;
      } else {
        out -= c;
      }
    }
  } else {
    out = a + b + c;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Early return
// ---------------------------------------------------------------------------

int32_t eval_early_return(int32_t* ok, int32_t code) {
  if (ok == NULL) {
    return -1;
  }
  if (code == 0) {
    *ok = 0;
    return 0;
  }
  if (code < 0) {
    *ok = 0;
    return code;
  }
  *ok = 1;
  return code * 2;
}

// ---------------------------------------------------------------------------
// State-machine shape (flattened-dispatcher-like, but small and readable)
// ---------------------------------------------------------------------------

int32_t eval_state_machine(uint32_t state, int32_t acc) {
  while (state != 0) {
    switch (state) {
      case 1:
        acc += 3;
        state = 2;
        break;
      case 2:
        acc *= 2;
        state = 3;
        break;
      case 3:
        acc -= 1;
        state = 0;
        break;
      default:
        return -1;
    }
  }
  return acc;
}
