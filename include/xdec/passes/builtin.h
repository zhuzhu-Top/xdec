// Registration of the built-in passes.
//
// One call site, one list: the CLI and the tests register the same pipeline
// stock, so a pass that only works in one of them is impossible by
// construction.
#pragma once

#include "xdec/pass/registry.h"

namespace xdec::passes {

/// Adds every built-in pass to the registry. Plugins register separately and
/// can depend on these by name.
void registerBuiltinPasses(pass::Registry& registry);

}  // namespace xdec::passes
