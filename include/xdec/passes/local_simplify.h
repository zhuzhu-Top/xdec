// The Lifted -> Local pass: the whole block-local cleanup contract in one
// fixpointed unit. Each run sweeps constant folding, lazy-flag condition
// folding, copy propagation and dead-code elimination over every block; the
// manager repeats until a run changes nothing. Folding exposes copies,
// copies expose constants, and both expose dead writes, so the transforms
// are deliberately one pass with a fixpoint rather than three passes hoping
// the scheduler interleaves them.
#pragma once

#include <memory>

#include "xdec/pass/pass.h"

namespace xdec::passes {

[[nodiscard]] std::unique_ptr<pass::Pass> makeLocalSimplifyPass();

}  // namespace xdec::passes
