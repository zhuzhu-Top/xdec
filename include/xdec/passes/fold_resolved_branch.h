// fold-resolved-branch: IndirectBranch with one target becomes Branch,
// Resolved -> Resolved.
//
// resolve-indirect proves a computed branch's destinations and stops there:
// it says what the targets are, not whether there is only one. When there is,
// the branch is no longer computed in any sense a reader cares about -- both
// candidate paths that could produce a singleton (analysis/image_eval.h's
// value set, or a jump table whose bound proves exactly one entry) are
// exhaustive by construction, so the one candidate is not a sample of several
// but the only address the branch can ever compute.
//
// Left as an IndirectBranch, the emitter has no choice but to print that as a
// runtime check: `if (v == addr) goto label; label: ...`, a compare that can
// never fail wrapped around a goto to the very next thing anyway. This pass
// turns it into what it actually is, an unconditional Branch, so the
// structurizer folds it into a plain fallthrough the same as any other jump
// -- no compare, no goto, no label.
//
// Kept separate from resolve-indirect (rather than folded into it) so the
// driver's own use of resolve-indirect, to probe an in-progress function for
// new addresses across discovery rounds, is unaffected: that probe wants
// every computed branch left as IndirectBranch so it keeps re-deriving
// targets each round the same way, regardless of how many this pass would
// have collapsed by the end.
#pragma once

#include <memory>

#include "xdec/pass/pass.h"

namespace xdec::passes {

[[nodiscard]] std::unique_ptr<pass::Pass> makeFoldResolvedBranchPass();

}  // namespace xdec::passes
