#include "xdec/support/compiler.h"

namespace xdec::detail {

void fatalError(const char* file, int line, const char* what) noexcept {
  std::fflush(stdout);
  std::fprintf(stderr, "xdec: fatal: %s:%d: %s\n", file, line, what);
  std::fflush(stderr);
  std::abort();
}

}  // namespace xdec::detail
