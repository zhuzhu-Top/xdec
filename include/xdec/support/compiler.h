// Compiler abstraction macros. Kept deliberately small: anything that can be
// expressed in portable C++20 should be.
#pragma once

#include <cstdio>
#include <cstdlib>

#if defined(__GNUC__) || defined(__clang__)
#  define XDEC_ALWAYS_INLINE inline __attribute__((always_inline))
#  define XDEC_NOINLINE __attribute__((noinline))
#  define XDEC_UNREACHABLE_IMPL() __builtin_unreachable()
#  define XDEC_EXPORT __attribute__((visibility("default")))
#elif defined(_MSC_VER)
#  define XDEC_ALWAYS_INLINE __forceinline
#  define XDEC_NOINLINE __declspec(noinline)
#  define XDEC_UNREACHABLE_IMPL() __assume(false)
#  define XDEC_EXPORT __declspec(dllexport)
#else
#  define XDEC_ALWAYS_INLINE inline
#  define XDEC_NOINLINE
#  define XDEC_UNREACHABLE_IMPL() ((void)0)
#  define XDEC_EXPORT
#endif

namespace xdec::detail {

[[noreturn]] void fatalError(const char* file, int line, const char* what) noexcept;

}  // namespace xdec::detail

/// Aborts with a located message. Used for broken internal invariants only;
/// anything a malformed input can trigger must go through Diag instead.
#define XDEC_FATAL(what) ::xdec::detail::fatalError(__FILE__, __LINE__, (what))

/// Marks a branch the code is structured to never take. Unlike a bare
/// __builtin_unreachable this still traps in debug builds, so a wrong
/// assumption surfaces as a crash with a location instead of silent UB.
#ifdef NDEBUG
#  define XDEC_UNREACHABLE(what) XDEC_UNREACHABLE_IMPL()
#else
#  define XDEC_UNREACHABLE(what) XDEC_FATAL("unreachable: " what)
#endif

/// Always-on invariant check. Decompiler passes corrupt IR in ways that are
/// far cheaper to catch at the source than to debug downstream, so these stay
/// enabled in release builds too.
#define XDEC_ASSERT(cond, what)             \
  do {                                      \
    if (!(cond)) [[unlikely]] {             \
      XDEC_FATAL("assertion failed: " what);\
    }                                       \
  } while (0)

/// Debug-only check for hot paths (bounds checks in handle lookups, etc.).
#ifdef NDEBUG
#  define XDEC_DASSERT(cond, what) ((void)0)
#else
#  define XDEC_DASSERT(cond, what) XDEC_ASSERT(cond, what)
#endif
