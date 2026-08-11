#include "options.h"

#include <cstdint>

#include "common.h"

namespace xdec::cli {

bool parseRoundCap(std::string_view text, RoundCap& out) {
  uint64_t parsed = 0;
  if (!parseNumber(text, parsed) || parsed == 0 || parsed > 1024) {
    return false;
  }
  out.value = static_cast<unsigned>(parsed);
  out.pinned = true;
  return true;
}

}  // namespace xdec::cli
