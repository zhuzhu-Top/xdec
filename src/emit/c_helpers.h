// The helper declarations the emitted C leans on.
//
// The printer only ever emits a helper the body actually used: an unused
// declaration is noise in a file whose whole purpose is to be read. The
// overflow-exact condition helpers are generated per width and condition, the
// rest are notes naming what the embedder must supply.
#pragma once

#include <set>
#include <string>

namespace xdec::emit {

/// Declarations for the helper keys collected during emission, in a stable
/// order. Empty when nothing needs one.
[[nodiscard]] std::string helperDeclarations(const std::set<std::string>& helpers);

}  // namespace xdec::emit
