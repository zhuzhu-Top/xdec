// resolve-indirect: turn computed branches into edges, Ssa -> Resolved.
//
// An unresolved IndirectBranch is a promise the lifter could not keep: the
// target is computed, and the lifter does not guess. This pass keeps the
// promise where the evidence allows. For each such branch it evaluates the
// target expression through the image evaluator (analysis/image_eval.h):
//
//   - A jump table `load(base + index*scale)` with a bounded index reads its
//     entries from the image and enumerates them.
//   - A `select(cond, tableA, tableB)` over an unanalysable condition unions
//     both tables' entries — the obfuscator's choice is between pointers we
//     can read either way.
//   - A global function pointer `load(const_addr)` is one entry.
//
// A branch resolves only when every candidate address lands on an existing
// block's start: partial CFGs are worse than unresolved ones, so the pass is
// all-or-nothing per branch and never invents targets. Candidates that do
// not read as mapped memory are dropped the same way; whatever stays
// unresolved keeps its `-> unresolved` marker in the dumps, visible as ever.
//
// What it cannot resolve it leaves for the verifier to fail on, because an
// unresolved branch at Resolved is a hole in the CFG and everything above it
// would be reasoning about a function it cannot see all of. A caller that would
// rather have the rest can say so (pass::Context::setSealUnresolvedBranches):
// each such branch becomes an opaque terminator naming the address and the
// expression that could not be evaluated, which is legal IL at Resolved and
// true — control leaves there, and where it goes is not a question the image
// answers.
//
// Every branch this sees is one that goes somewhere in this function, because
// the branches that leave it were already rewritten into calls at Ssa (see
// recover_tailcall.h). Which is why resolving one drops the argument-register
// snapshot SSA construction left on it: that snapshot was the tail-call
// question, and this pass answering "it is a jump" answers it.
//
// The pass needs the image: wire Manager::setImage or it fails loudly.
#pragma once

#include <memory>

#include "xdec/pass/pass.h"

namespace xdec::passes {

[[nodiscard]] std::unique_ptr<pass::Pass> makeResolveIndirectPass();

}  // namespace xdec::passes
