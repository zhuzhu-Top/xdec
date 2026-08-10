// isNoreturnCallTarget: PLT stub -> GOT slot -> known-noreturn symbol name.
// See the header for why the lifter needs this at all.
#include "xdec/analysis/noreturn.h"

#include <array>

namespace xdec::analysis {

namespace {

// The handful of C runtime / libc entry points every eval case gets to rely
// on never returning. Widening this list is safe (see the header); narrowing
// it is not, so nothing here is included on a guess -- each one is a
// standard part of glibc/bionic with that exact contract.
constexpr std::array<std::string_view, 8> kNoreturnSymbols = {
    "__stack_chk_fail", "abort",        "exit",   "_exit",
    "__assert_fail",    "__assert2",    "quick_exit", "longjmp",
};

}  // namespace

bool isKnownNoreturnSymbol(std::string_view name) noexcept {
  for (const std::string_view candidate : kNoreturnSymbols) {
    if (name == candidate) {
      return true;
    }
  }
  return false;
}

bool isNoreturnCallTarget(uint64_t target, const ByteReader& reader, const MemoryFacts& facts) {
  const std::optional<std::string> imported = importNameForPltStub(target, reader, facts);
  return imported.has_value() && isKnownNoreturnSymbol(*imported);
}

}  // namespace xdec::analysis
