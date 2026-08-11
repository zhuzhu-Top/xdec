// Ground truth for AArch64 exclusive-access and compare-and-swap recovery:
// ldaxr/stlxr and casal written by hand, because no ordinary C the compiler
// would choose to emit on its own reliably produces them -- __atomic_* at
// -O1 can just as easily lower to an out-of-line runtime call (NDK's
// outline-atomics), and casal needs an LSE-enabled target the corpus does
// not otherwise build for. Inline asm makes the instruction the decompiler
// has to recover independent of either.
//
// See eval/manifest.json's eval_atomic_llsc and eval_atomic_cas for what
// each is checked against, and docs/11-helpers-header.md for the emitter
// side (__ldaxrN/__stlxrN, printCas) these exercise.

#include <stdint.h>

#if defined(__aarch64__)

// The LL/SC loop every pre-LSE AArch64 core needs for a compare-and-swap:
// ldaxr/stlxr split into a reservation-plus-load and a store-plus-status
// pair in xdec's IL (specs/arm64/loadstore.xspec), which the emitter fuses
// back into __ldaxr32/__stlxr32.
uint32_t eval_atomic_llsc(uint32_t* p, uint32_t expected, uint32_t desired) {
  uint32_t result, status;
  do {
    __asm__ volatile("ldaxr %w0, [%1]" : "=&r"(result) : "r"(p) : "memory");
    if (result != expected) {
      break;
    }
    __asm__ volatile("stlxr %w0, %w1, [%2]"
                     : "=&r"(status)
                     : "r"(desired), "r"(p)
                     : "memory");
  } while (status);
  return result;
}

// The single-instruction form ARMv8.1's LSE extension adds. `.arch_extension
// lse` enables the mnemonic for this one asm block without raising the
// whole translation unit's target, the same trick the Linux kernel uses to
// keep an LSE fast path in a binary that still runs on cores without it.
uint32_t eval_atomic_cas(uint32_t* p, uint32_t expected, uint32_t desired) {
  uint32_t old = expected;
  __asm__ volatile(".arch_extension lse\n"
                   "casal %w0, %w1, [%2]"
                   : "+r"(old)
                   : "r"(desired), "r"(p)
                   : "memory");
  return old;
}

#else

// The corpus is built for arm64-v8a; these keep a host build compiling.
uint32_t eval_atomic_llsc(uint32_t* p, uint32_t expected, uint32_t desired) {
  uint32_t old = *p;
  if (old == expected) {
    *p = desired;
  }
  return old;
}
uint32_t eval_atomic_cas(uint32_t* p, uint32_t expected, uint32_t desired) {
  uint32_t old = *p;
  if (old == expected) {
    *p = desired;
  }
  return old;
}

#endif
