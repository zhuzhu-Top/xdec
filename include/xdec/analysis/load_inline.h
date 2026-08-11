// Recognises a Load whose address is NOT a stack slot (a Global or an
// "Other" address such as an argument-plus-constant-offset chain) but still
// has exactly the shape findFoldableStackLoads targets: every live reader in
// the same block, after the load, with nothing between able to have changed
// what the address holds -- see that file for why this is a text
// substitution rather than an IL rewrite, and why safety turns entirely on
// that same-block/ordered/no-clobber condition, not on what kind of address
// this is.
//
// findFoldableStackLoads reserves itself to StackSlot because a stack
// slot's substitution text is already known ahead of printing: the local's
// own declared name. A Global or Other address has no such name -- the text
// that replaces the temporary is the address expression's own printed
// form, which only exists once ExprPrinter renders it. So this analysis
// records just enough (the address ExprId and the width the load read at)
// for ExprPrinter::value to regenerate, in place at the load's one use,
// exactly the `(*(T*)...)` text StmtPrinter::memoryLvalue would otherwise
// have printed once, into a temporary, at the load's original position.
// `frame.mayAlias` already reasons about Global and Other addresses the
// same conservative way it reasons about a stack slot -- an intervening
// Store it cannot rule out as aliasing, or any Call/Intrinsic at all, blocks
// the fold exactly as it would for a stack slot -- so nothing about the
// clobber check below needs to change for these address kinds.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include "xdec/analysis/stack_frame.h"
#include "xdec/il/function.h"

namespace xdec::analysis {

/// What is needed to reprint a folded non-stack load's address expression
/// directly in place of its temporary.
struct FoldableMemoryLoad {
  il::ExprId address;
  uint32_t width = 0;  // bits, matching the load's own result width
};

/// Every Load in `function` eligible to fold whose address does NOT
/// classify as a StackSlot (see stack_load_fold.h for that case), keyed by
/// `OpId::index()`. Same safety rules as findFoldableStackLoads, minus the
/// stack-only restriction:
///
///  1. The load's address classifies as Global or Other.
///  2. It has at least one *live* reader (an op not already in `deadOps`).
///  3. Every live reader is an op in the load's own block, after it in the
///     block's own op order.
///  4. Nothing between the load and any of its live readers could have
///     overwritten the address: no Store `frame.mayAlias` cannot rule out,
///     and no Call/Intrinsic/Unimplemented.
///  5. No live reader consumes the result as the *address* operand of
///     another Load or Store. That shape is a spilled pointer one hop of a
///     struct chain reads through (see c_context.cpp's `fieldAccess`, which
///     recognises `n->next->value` by looking up a *name* for the base
///     value); folding it away would replace that name with raw pointer
///     arithmetic, which is strictly less readable, not more.
[[nodiscard]] std::unordered_map<uint32_t, FoldableMemoryLoad> findFoldableMemoryLoads(
    const il::Function& function, const StackFrame& frame,
    const std::unordered_set<uint32_t>& deadOps);

}  // namespace xdec::analysis
