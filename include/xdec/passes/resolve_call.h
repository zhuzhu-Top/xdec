// The Ssa-level indirect-call pass: what is this call actually calling?
//
// resolve-indirect answers that question for computed *branches*, and only for
// them — its filter names IndirectBranch and nothing else. Calls were left out,
// so a binary that reaches every one of its callees through a pointer table
// decompiles to a page of `((uint64_t (*)(uint64_t))t399)(t398)`, with no
// statement anywhere about what is being called or why it cannot be said.
//
// This pass makes one of two statements about each such call, and never
// anything in between:
//
//   1. It proves the target. The target expression is evaluated over a memory
//      that consists only of ranges the program can never write, so a value
//      derived from it is a constant of the program rather than a guess about
//      what memory will hold at run time. If that yields exactly one address
//      and the address is in executable memory, the call is rewritten to a
//      direct call on that constant. This is the only case that changes the IL.
//      const-fold-memory has already handled the easy half of this — a pointer
//      at a *constant* immutable address is folded before this pass runs — so
//      what is left here is the half that needs a value set: a pointer fetched
//      through a computed address, or through a merge of several, which no
//      single-load fold can see through.
//
//   2. It describes the shape. When the target does not converge, the *form* of
//      the computation is still knowable and worth saying: that the target is
//      read from a table at a known base, that the index is a runtime value,
//      that the loaded pointer is transformed before use, or that the table
//      lives in writable memory and therefore cannot be read from the file at
//      all. That goes into an il::Function note (see Function::annotate), which
//      the C emitter prints above the call. No IL changes, no guess is made,
//      and a reader learns what the obfuscation is doing instead of staring at
//      a cast.
//
// What this pass will not do is pick a plausible target out of several. One
// Call op has one target; a set of candidates is not a target, and writing one
// of them down would be inventing a call the program may never make.
#pragma once

#include <memory>

#include "xdec/pass/pass.h"

namespace xdec::passes {

[[nodiscard]] std::unique_ptr<pass::Pass> makeResolveCallPass();

}  // namespace xdec::passes
