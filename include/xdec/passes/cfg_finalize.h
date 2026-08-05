// The Local -> Cfg gate pass.
//
// liftFunction already builds a complete direct-edge CFG: every real block
// ends in a terminator, edges are rebuilt, external stubs are marked, and
// unresolved indirect branches sit as IndirectBranch ops with empty target
// lists — which is precisely what "marked as such rather than left dangling"
// means at this level. This pass is the *gate* that audits that contract
// before the function is allowed to carry the Cfg label, and the repair of
// last resort for stale edge caches (a hand-built function, a plugin that
// skipped rebuildEdges). A violation it cannot repair — an unterminated
// block — stops the pipeline with the block named, because that is the
// lifter's contract broken, not something to paper over mid-pipeline.
#pragma once

#include <memory>

#include "xdec/pass/pass.h"

namespace xdec::passes {

[[nodiscard]] std::unique_ptr<pass::Pass> makeCfgFinalizePass();

}  // namespace xdec::passes
