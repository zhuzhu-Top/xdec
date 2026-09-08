// annotate-stack-canary: drops a note on a proven `-fstack-protector`
// save/check round trip (see analysis/stack_canary.h for the shape and why
// it stops short of naming the guard address).
//
// Pure presentation, same as recover-syscall's and apply-types' own note
// calls: nothing about the IL changes, only what the emitter prints ahead of
// the save's statement. Runs at Vars so analysis/stack_frame.h's stack-slot
// deltas and const-fold-memory's Global addresses have both already settled,
// and last in the pipeline (see builtin.cpp) so no later pass needs to keep
// the note in step with a rewrite.
#pragma once

#include <memory>

#include "xdec/pass/pass.h"

namespace xdec::passes {

[[nodiscard]] std::unique_ptr<pass::Pass> makeAnnotateStackCanaryPass();

}  // namespace xdec::passes
