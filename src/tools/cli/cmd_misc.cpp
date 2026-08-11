// log-categories: everything else that does not fit another domain.
#include "common.h"
#include "xdec/support/log.h"

namespace xdec::cli {

int commandLogCategories() {
  printLine("logging categories (set XDEC_LOG=name=level):");
  for (const auto* category : xdec::logCategories()) {
    print("  {:<12} {}", category->name(), toString(category->level()));
  }
  return 0;
}

}  // namespace xdec::cli
