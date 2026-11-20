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
constexpr std::array<std::string_view, 16> kNoreturnSymbols = {
    "__stack_chk_fail", "abort",          "exit",              "_exit",
    "__assert_fail",    "__assert2",      "quick_exit",        "longjmp",
    "__assert_rtn",     "objc_exception_throw", "_Unwind_Resume", "__cxa_throw",
    "abort_with_reason", "abort_with_payload", "__chk_fail",    "__fortify_fail",
};

[[nodiscard]] bool matchesNoreturnSymbol(std::string_view name) noexcept {
  for (const std::string_view candidate : kNoreturnSymbols) {
    if (name == candidate) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool isKnownNoreturnSymbol(std::string_view name) noexcept {
  if (matchesNoreturnSymbol(name)) {
    return true;
  }
  // Mach-O C symbols carry one leading underscore (`___stack_chk_fail`,
  // `_abort`, `__exit`). Strip one underscore and check again so Darwin
  // binaries share the same runtime noreturn contract without duplicating
  // every entry.
  if (name.size() > 1 && name.front() == '_') {
    return matchesNoreturnSymbol(name.substr(1));
  }
  return false;
}

bool isNoreturnCallTarget(uint64_t target, const ByteReader& reader, const MemoryFacts& facts) {
  const std::optional<std::string> imported = importNameForPltStub(target, reader, facts);
  return imported.has_value() && isKnownNoreturnSymbol(*imported);
}

}  // namespace xdec::analysis
