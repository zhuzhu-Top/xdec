// Ground truth for AArch64 syscall recovery, same discipline as
// source_syscall.c but over the numbers a real environment probe hits:
// openat/read/close on /proc files, mmap, clock_gettime, ptrace, getuid,
// nanosleep, and gettimeofday's own errno-store idiom. Each case is written
// by hand as `svc #0` so the decompiler sees exactly what an obfuscated
// binary shows -- a number in x8, arguments in x0..x5, the kernel's answer
// back in x0 -- rather than a libc call the linker has already named.
//
// The wrappers are `static inline` on purpose: at -O1 the compiler folds the
// number into the caller, which is the shape recovery is designed for.

#include <stdint.h>
#include <stddef.h>

#if defined(__aarch64__)

#include <errno.h>
#include <sys/time.h>

static inline long eval_svcenv0(long nr) {
  register long x8 __asm__("x8") = nr;
  register long x0 __asm__("x0");
  __asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory", "cc");
  return x0;
}

static inline long eval_svcenv1(long nr, long a0) {
  register long x8 __asm__("x8") = nr;
  register long x0 __asm__("x0") = a0;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
  return x0;
}

static inline long eval_svcenv2(long nr, long a0, long a1) {
  register long x8 __asm__("x8") = nr;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory", "cc");
  return x0;
}

static inline long eval_svcenv3(long nr, long a0, long a1, long a2) {
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

static inline long eval_svcenv4(long nr, long a0, long a1, long a2, long a3) {
  register long x8 __asm__("x8") = nr;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  __asm__ volatile("svc #0"
                   : "+r"(x0)
                   : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
                   : "memory", "cc");
  return x0;
}

static inline long eval_svcenv6(long nr, long a0, long a1, long a2, long a3,
                                 long a4, long a5) {
  register long x8 __asm__("x8") = nr;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  register long x4 __asm__("x4") = a4;
  register long x5 __asm__("x5") = a5;
  __asm__ volatile("svc #0"
                   : "+r"(x0)
                   : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                   : "memory", "cc");
  return x0;
}

// __NR_openat (56): every real `open` on aarch64 is this syscall under a
// libc wrapper -- there is no bare __NR_open on this architecture -- so the
// raw form always carries the dirfd the wrapper hardcodes to AT_FDCWD.
long eval_svc_openat(const char* path, long flags) {
  return eval_svcenv4(56, -100, (long)path, flags, 0);
}

// __NR_read (63): three arguments, the same shape as write with the
// direction reversed.
long eval_svc_read(long fd, void* buf, unsigned long n) {
  return eval_svcenv3(63, fd, (long)buf, (long)n);
}

// __NR_close (57): a single descriptor argument, so five of six registers
// come off the call.
long eval_svc_close(long fd) { return eval_svcenv1(57, fd); }

// __NR_mmap (222): six arguments, the widest a syscall gets on this ABI --
// the case that proves the wrapper chain scales to x0..x5 without dropping
// any of them.
void* eval_svc_mmap(void* addr, unsigned long len, long prot, long flags,
                     long fd, long off) {
  return (void*)eval_svcenv6(222, (long)addr, (long)len, prot, flags, fd, off);
}

// __NR_clock_gettime (113): an int and a pointer, the anti-debug timing
// probe's raw form.
long eval_svc_clock_gettime(long clk_id, void* ts) {
  return eval_svcenv2(113, clk_id, (long)ts);
}

// __NR_ptrace (117): the anti-debugger's own syscall, spelled without the
// libc wrapper a tracer could breakpoint.
long eval_svc_ptrace(long request, long pid, void* addr, void* data) {
  return eval_svcenv4(117, request, pid, (long)addr, (long)data);
}

// __NR_getuid (174): no arguments, same trim as getpid but a different
// number -- the check that the table is being read, not a name assumed.
long eval_svc_getuid(void) { return eval_svcenv0(174); }

// __NR_nanosleep (101): two pointer arguments, the raw form of the sleep an
// anti-emulation probe times.
long eval_svc_nanosleep(const void* req, void* rem) {
  return eval_svcenv2(101, (long)req, (long)rem);
}

// The sub_199214 shape, but the value entering the store is a raw syscall's
// result rather than a libc call's: gettimeofday over `svc #0`, then the
// sign test every syscall site carries, so `-ret` lands in `errno` through
// __errno_location() the same way it would if a compiler had emitted this
// from the libc wrapper's own source.
int eval_svc_gettimeofday_errno(void* tv, void* tz) {
  long ret = eval_svcenv2(169, (long)tv, (long)tz);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

// The libc counterpart to the case above: the same gettimeofday, called the
// ordinary way, so a reader can set the raw-syscall form and the
// PLT-resolved form of the identical operation side by side.
int eval_svc_gettimeofday_libc(void* tv, void* tz) {
  int r = gettimeofday((struct timeval*)tv, (struct timezone*)tz);
  if (r < 0) {
    return -1;
  }
  return r;
}

#else

// The corpus is built for arm64-v8a; these keep a host build compiling.
long eval_svc_openat(const char* path, long flags) { (void)path; return flags; }
long eval_svc_read(long fd, void* buf, unsigned long n) { (void)buf; return fd + (long)n; }
long eval_svc_close(long fd) { return fd; }
void* eval_svc_mmap(void* addr, unsigned long len, long prot, long flags, long fd, long off) {
  (void)addr; (void)len; (void)prot; (void)flags; (void)fd; (void)off; return NULL;
}
long eval_svc_clock_gettime(long clk_id, void* ts) { (void)ts; return clk_id; }
long eval_svc_ptrace(long request, long pid, void* addr, void* data) {
  (void)addr; (void)data; return request + pid;
}
long eval_svc_getuid(void) { return 0; }
long eval_svc_nanosleep(const void* req, void* rem) { (void)rem; return (long)req; }
int eval_svc_gettimeofday_errno(void* tv, void* tz) { (void)tv; (void)tz; return 0; }
int eval_svc_gettimeofday_libc(void* tv, void* tz) { (void)tv; (void)tz; return 0; }

#endif
