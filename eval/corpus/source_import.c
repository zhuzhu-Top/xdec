// Ground truth for import resolution itself (docs/10-import-resolution.md),
// as opposed to source_env.c's subject of *which* Bionic API a call reaches.
// Every shape the document names gets its own case: the errno idiom read and
// folded, the two names on the noreturn list that motivated PLT-stub decoding
// in the first place, a genuine GOT-indirect call that is not also a tail
// call (source_tailcall.c's case always is one), and the arity limits a
// variadic import leaves behind. Baseline only, same as source_env.c and for
// the same reason: TargetProfile supplies android-ndk without anyone asking
// for it.

#include <stddef.h>
#include <stdint.h>

#if defined(__ANDROID__)

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// The errno idiom: read, folded write, and the write a Phi still reads.
// ---------------------------------------------------------------------------

int32_t eval_import_errno_call(void) {
  return errno;
}

int32_t eval_import_errno_fold(long raw_ret) {
  if (raw_ret < 0) {
    errno = (int)(-raw_ret);
    return -1;
  }
  return (int32_t)raw_ret;
}

// The sub_199214 shape: a dispatcher whose arms carry one variable to a
// shared exit, one of them through the errno store -- a `goto` and a real
// merge block, not eval_import_errno_fold's single guard. The decline this
// idiom can require -- the fold withheld because a Phi at that merge still
// reads the call's result on some other edge -- is pinned deterministically
// at the IL level, where register allocation is under the test's control
// (tests/emit/test_c_import_call.cpp); a compiler is free to return a known
// constant like -1 straight from its own block instead of threading it
// through the merge, which is what this specific compilation does. What
// this case is load-bearing for is the fold applying correctly at all
// once a real goto sits between the store and the label the other arms
// share, not the decline itself.
int32_t eval_import_errno_dispatch(int32_t op, long a, long b) {
  long ret = a;
  switch (op) {
    case 0:
      ret = a - b;
      if (ret < 0) {
        errno = (int)(-ret);
        ret = -1;
      }
      break;
    case 1:
      ret = b;
      break;
    default:
      break;
  }
  return (int32_t)ret;
}

// ---------------------------------------------------------------------------
// The noreturn pair PLT-stub decoding was written to recognise.
// ---------------------------------------------------------------------------

int32_t eval_import_stack_chk(const char* src) {
  char buf[64];
  int32_t i = 0;
  while (src[i] != '\0' && i < 63) {
    buf[i] = src[i];
    i++;
  }
  buf[i] = '\0';
  int32_t sum = 0;
  for (int32_t j = 0; j < i; j++) {
    sum += buf[j];
  }
  return sum;
}

void eval_import_abort(int32_t code) {
  if (code < 0) {
    abort();
  }
}

// ---------------------------------------------------------------------------
// The three call shapes: direct PLT, GOT-indirect, and the arity a variadic
// import leaves untrimmed.
// ---------------------------------------------------------------------------

int32_t eval_import_write_direct(void) {
  const ssize_t n = write(1, "x", 1);
  return n < 0 ? -1 : (int32_t)n;
}

// A function pointer initialised to an imported symbol's address, read back
// through a `volatile` global so -O1 cannot fold the indirection away and
// turn this into eval_import_write_direct's shape. Unlike
// eval_tailcall_import (source_tailcall.c), the call here is not also a tail
// call: what is pinned is Shape B on its own, one indirect call in the
// middle of a function that does something with the result afterwards.
typedef int32_t (*GetpidFn)(void);
static volatile GetpidFn g_getpid_fn = (GetpidFn)getpid;

int32_t eval_import_got_indirect(void) {
  const GetpidFn fn = g_getpid_fn;
  const int32_t pid = fn();
  return pid + 1;
}

int32_t eval_import_snprintf(void) {
  char buf[32];
  const int32_t n = snprintf(buf, sizeof(buf), "%d", 42);
  return n;
}

int32_t eval_import_strlen(const char* s) {
  const size_t len = strlen(s);
  // >= rather than > 0: unsigned, so a bound against a constant other than
  // zero is what keeps this from collapsing back to plain identity (`len`
  // itself already reads as 0 when the string is empty).
  return len >= 8 ? 1 : 0;
}

int32_t eval_import_memcpy(void* dst, const void* src, size_t n) {
  memcpy(dst, src, n);
  return (int32_t)n;
}

int32_t eval_import_strcmp(const char* a, const char* b) {
  return strcmp(a, b) == 0;
}

int32_t eval_import_fopen(void) {
  FILE* f = fopen("/proc/self/status", "r");
  if (f == NULL) {
    return -1;
  }
  char buf[64];
  const size_t n = fread(buf, 1, sizeof(buf), f);
  fclose(f);
  return (int32_t)n;
}

#else

// The corpus is built for arm64-v8a; these keep a host build compiling.
int32_t eval_import_errno_call(void) { return 0; }
int32_t eval_import_errno_fold(long raw_ret) { return (int32_t)raw_ret; }
int32_t eval_import_errno_dispatch(int32_t op, long a, long b) { return (int32_t)(op + a + b); }
int32_t eval_import_stack_chk(const char* src) { (void)src; return 0; }
void eval_import_abort(int32_t code) { (void)code; }
int32_t eval_import_write_direct(void) { return 0; }
int32_t eval_import_got_indirect(void) { return 0; }
int32_t eval_import_snprintf(void) { return 0; }
int32_t eval_import_strlen(const char* s) { (void)s; return 0; }
int32_t eval_import_memcpy(void* dst, const void* src, size_t n) {
  (void)dst; (void)src; return (int32_t)n;
}
int32_t eval_import_strcmp(const char* a, const char* b) { (void)a; (void)b; return 0; }
int32_t eval_import_fopen(void) { return 0; }

#endif
