// helperDeclarations (see the header for the emit-only-what-is-used rule).
#include "c_helpers.h"

#include <format>

namespace xdec::emit {

namespace {

/// Whether any collected key needs xdec_helpers.h: everything does except
/// `intrin` and `syscall`, which keep their own ad hoc declarations below
/// because a fixed prototype cannot name every intrinsic and the syscall
/// one already lived here before this header existed.
[[nodiscard]] bool needsHeader(const std::set<std::string>& helpers) {
  for (const std::string& helper : helpers) {
    if (helper != "intrin" && helper != "syscall") {
      return true;
    }
  }
  return false;
}

}  // namespace

std::string helperDeclarations(const std::set<std::string>& helpers,
                               const std::string& helpersHeader) {
  std::string out;
  if (!helpersHeader.empty() && needsHeader(helpers)) {
    out += std::format("#include \"{}\"\n", helpersHeader);
  }
  if (helpers.contains("syscall")) {
    out +=
        "/* a syscall whose number the analyses could not name: the number is the\n"
        "   first argument, the rest are x0..x5 as the instruction found them */\n"
        "long __xdec_syscall(long nr, ...);\n";
  }
  if (helpers.contains("intrin")) {
    out += "/* __xdec_intrin_*: target intrinsics, supplied by the embedder */\n";
  }
  return out;
}

}  // namespace xdec::emit
