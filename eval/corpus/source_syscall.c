// Ground truth for AArch64 syscall recovery: `svc #0` written by hand, because
// no libc call would produce it.
//
// Every case here goes through the inline-asm wrappers below rather than
// through libc, so what the decompiler sees is exactly what an obfuscated
// binary shows: a number moved into x8, some arguments in x0..x5, and an
// instruction that leaves the kernel's answer in x0. Whether that reads back as
// `sys_write(1, buf, n)` or as `__xdec_syscall(nr, ...)` is the whole subject of
// these cases, and the manifest says which each one must be.
//
// The wrappers are `static inline` on purpose: at -O1 the compiler folds the
// number into the caller, which is the shape recovery is designed for. The one
// case that deliberately does *not* fold is eval_svc_nr_from_arg, whose number
// is a parameter and can therefore not be known from the function alone.

#include <stdint.h>
#include <stddef.h>

#if defined(__aarch64__)

static inline long eval_svc0(long nr) {
  register long x8 __asm__("x8") = nr;
  register long x0 __asm__("x0");
  __asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory", "cc");
  return x0;
}

static inline long eval_svc1(long nr, long a0) {
  register long x8 __asm__("x8") = nr;
  register long x0 __asm__("x0") = a0;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
  return x0;
}

static inline long eval_svc2(long nr, long a0, long a1) {
  register long x8 __asm__("x8") = nr;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory", "cc");
  return x0;
}

static inline long eval_svc3(long nr, long a0, long a1, long a2) {
  register long x8 __asm__("x8") = nr;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  __asm__ volatile("svc #0"
                   : "+r"(x0)
                   : "r"(x8), "r"(x1), "r"(x2)
                   : "memory", "cc");
  return x0;
}

// __NR_write: three arguments, so x3..x5 must come off the call.
long eval_svc_write(const void* buf, unsigned long n) {
  return eval_svc3(64, 1, (long)buf, (long)n);
}

// __NR_gettimeofday: two pointer arguments, which the table knows the types of.
int eval_svc_gettimeofday(void* tv, void* tz) {
  return (int)eval_svc2(169, (long)tv, (long)tz);
}

// __NR_getpid: no arguments at all, so every argument register comes off and
// the call prints empty. The strongest single check that trimming happened.
long eval_svc_getpid(void) { return eval_svc0(172); }

// A number no kernel defines. Nothing may be invented for it.
long eval_svc_unknown(long a0) { return eval_svc1(9999, a0); }

// The number is this function's argument, so it is unknowable from the function
// alone. The case exists to pin the degradation: the instruction is still a
// syscall, and the expression that produced the number is still printed.
long eval_svc_nr_from_arg(long nr, long a0) { return eval_svc1(nr, a0); }

// The idiom every real syscall site has: the kernel returns -errno, so the
// caller tests the sign. B4 folds the obfuscated form of this; here it is the
// plain one, which must read as an ordinary comparison.
long eval_svc_errno(const void* buf, unsigned long n) {
  const long written = eval_svc3(64, 1, (long)buf, (long)n);
  if (written < 0) {
    return -1;
  }
  return written;
}

#else

// The corpus is built for arm64-v8a; these keep a host build compiling.
long eval_svc_write(const void* buf, unsigned long n) { (void)buf; return (long)n; }
int eval_svc_gettimeofday(void* tv, void* tz) { (void)tv; (void)tz; return 0; }
long eval_svc_getpid(void) { return 0; }
long eval_svc_unknown(long a0) { return a0; }
long eval_svc_nr_from_arg(long nr, long a0) { return nr + a0; }
long eval_svc_errno(const void* buf, unsigned long n) { (void)buf; return (long)n; }

#endif
