// The helper declarations the emitted C leans on.
//
// Most helper keys the body used (rotate, byte swap, population count, the
// overflow-exact condition codes) are defined once in xdec_helpers.h: this
// only decides whether the body's `#include` of it is worth emitting, plus
// the two helpers that are not header material because a fixed prototype
// cannot describe them -- `intrin` is a family of differently-named calls,
// one per instruction, and `syscall`'s declaration already lives here as
// the single case a fixed signature does cover.
#pragma once

#include <set>
#include <string>

namespace xdec::emit {

/// Declarations for the helper keys collected during emission, in a stable
/// order. Empty when nothing needs one. `helpersHeader` is the path the
/// `#include` names (see COptions::helpersHeader); passing an empty string
/// suppresses the include even when the body used a header-defined helper.
[[nodiscard]] std::string helperDeclarations(const std::set<std::string>& helpers,
                                             const std::string& helpersHeader);

}  // namespace xdec::emit
