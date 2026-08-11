// Recognises a Load from a stack slot whose result is read from exactly one
// place in the whole function, with nothing between the load and that one
// use able to have overwritten the slot -- the shape that, left to the
// ordinary pipeline, still ends up printed as `t0 = var_984; ...;
// f(t0);` even though nothing needs `t0` to outlive that single read.
//
// This is deliberately not an IL rewrite. The redundancy here is not an IL
// fact -- the Load is exactly the one memory read the machine code performs,
// and its result is exactly one value used exactly once, which is already as
// minimal as SSA gets. What is redundant is the *emitted C*: `nameResultTemps`
// (emit/c_printer.cpp) materializes every Load's result into a temporary
// unconditionally, on the assumption that a use might be arbitrarily far
// away. When a use turns out to be the very next reader and nothing between
// the two could have changed what the slot holds, the temporary buys
// nothing, and the emitter can print the slot's own name at that one use
// instead -- the same "dead once recognised, nothing about the IL changes"
// shape docs/09-expression-reuse.md's shape E already uses for a resolved
// jump table's own address computation. See emit/c_context.cpp's
// `stackSlotLvalue` (the text this analysis's findings are turned into) and
// `ExprPrinter::value` (emit/c_expr.cpp, where a use of the folded value is
// substituted).
#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include "xdec/analysis/stack_frame.h"
#include "xdec/il/function.h"

namespace xdec::analysis {

/// What is needed to print a folded load's slot directly in place of its
/// temporary: the stack displacement and the width it was read at (a load
/// narrower than the slot's declared width still needs its own cast, same as
/// the ordinary memoryLvalue path).
struct FoldableStackLoad {
  int64_t delta = 0;
  uint32_t width = 0;
  /// True when the load's one use is the *address* operand of another Load
  /// or Store -- the shape a spilled pointer takes -- rather than an
  /// ordinary value operand. Lets variables.cpp's pointer refinement treat
  /// the slot itself as a pointer instead of a plain integer (see its own
  /// note on why that is restricted to exactly this case).
  bool usedAsAddress = false;
};

/// Every Load in `function` eligible to fold, keyed by its `OpId::index()`.
/// Safety rules, all required:
///
///  1. The load's address classifies as a StackSlot (never Global or Other:
///     an image address can be observed by something outside this function,
///     and an unclassified pointer's aliasing is unknown by definition).
///  2. It has at least one *live* reader -- an op not already in `deadOps`
///     (one nothing will ever print is not a real use to fold against) --
///     and zero live readers means dce's own fault-conservative policy on
///     Load applies, not this one.
///  3. Every live reader is an op in the *same* block as the load, after it
///     in the block's own op order (SSA guarantees a definition dominates
///     its use; disagreement here is untrusted rather than acted on). A
///     value read again from a different block -- most often the far side
///     of a merge a flattened dispatcher's loop-carried state closes
///     through -- is exactly the cross-block forwarding this analysis does
///     not attempt (see the file comment's non-goal); such a load keeps its
///     ordinary temporary.
///  4. Nothing between the load and any of its live readers could have
///     overwritten the slot: no Store the frame cannot rule out as
///     aliasing, and no Call/Intrinsic/Unimplemented, which may write
///     through any pointer at all (the same barrier stack_prop.cpp gives a
///     reload it is forwarding).
///
/// A load with two or more live readers is folded exactly like one with a
/// single reader, each reader's own use substituted independently: nothing
/// here requires there to be only one, only that every one of them is safe.
[[nodiscard]] std::unordered_map<uint32_t, FoldableStackLoad> findFoldableStackLoads(
    const il::Function& function, const StackFrame& frame,
    const std::unordered_set<uint32_t>& deadOps);

}  // namespace xdec::analysis
