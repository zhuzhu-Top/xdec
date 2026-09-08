// Recognises a run of consecutive constant Stores into contiguous stack
// bytes whose concatenation decodes to a printable, NUL-terminated C
// string -- the shape a compiler leaves behind when it inlines
// `strcpy(dst, "short literal")` as a handful of immediate stores instead of
// a real call. IDA's Hex-Rays re-synthesizes exactly this shape back into a
// `strcpy(...)` line; left as-is, xdec instead prints every store
// individually (`var_6b = 0x6c7070612e6d6f63; var_63 = 0x62612e65; ...`),
// which is the literal semantics but hides the one fact a reader actually
// wants: that this is `"com.apple.absd"` being written.
//
// Deliberately not an IL rewrite, the same reasoning stack_load_fold.h and
// stack_store_fold.h give for their own folds: every Store in a matched run
// is exactly one memory write the machine code performs, and none of them
// are deleted here (contrast stack_store_fold.h, which does delete a
// store, but only one already proven dead). This is a pure presentation
// fold -- see emit/c_stmt.cpp's printFoldedStringStore, which prints the
// whole run as a single synthesized `strcpy` at the first Store's position
// and folds the rest into `deadOps` the same "printed elsewhere" way
// load_inline.h's folds are.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "xdec/analysis/stack_frame.h"
#include "xdec/il/function.h"

namespace xdec::analysis {

/// One maximal run of constant Stores folded into a single synthesized
/// string copy.
struct FoldableStringStore {
  /// The lowest stack delta the run writes -- where the destination
  /// pointer (the run's first Store's own address) is taken from.
  int64_t delta = 0;
  /// The decoded string, NUL terminator not included (see quoteCString for
  /// turning this into a C string literal).
  std::string text;
  /// Every Store in the run after the first, in ascending-delta order.
  /// These are the ones that fold into `deadOps`; the run's first Store
  /// (lowest delta, this map's own key) is never marked dead itself -- the
  /// synthesized `strcpy` prints in its place, not instead of it.
  std::vector<uint32_t> continuationOps;
};

/// Every maximal run of two or more constant Stores in `function` whose
/// concatenated little-endian bytes decode to a printable, NUL-terminated C
/// string, keyed by the run's first (lowest-delta) Store's `OpId::index()`.
/// Safety rules, all required:
///
///  1. Every Store in the run is adjacent in its block's own op list --
///     nothing of any kind between two Stores of the same run, not just
///     nothing that could alias. A compiler that spells one string literal
///     as a handful of immediate stores emits them back-to-back; anything
///     else interleaved means these are not one initializer, and this
///     analysis would otherwise be reordering an observable side effect
///     (whatever sits between them) behind the synthesized line's position.
///  2. Every Store's address classifies as a StackSlot (see
///     analysis::StackFrame), and each one's delta picks up exactly where
///     the previous one's byte range left off: no gap, no overlap.
///  3. Every Store's value is a compile-time constant
///     (`Function::asConstantThroughCasts`).
///  4. The concatenated bytes contain exactly one NUL byte, as the very
///     last byte, and every byte before it is printable ASCII
///     (0x20..0x7e) -- the same threshold image_literals.h uses for a
///     read-only global's string recovery. Anything else is some other
///     constant-initialized buffer (a GUID, packed integers, ...), not a
///     string, and this analysis leaves it alone rather than guess.
///  5. The run is at least two Stores long: a single constant Store already
///     prints fine on its own and gains nothing from this.
///
/// This never inspects whether the destination escapes, is ever read, or
/// has a name -- those questions belong to findDeadStackStores and to the
/// emit layer's own local recovery (see CContext::addressOfLocal, which
/// c_context.cpp consults before accepting one of these candidates: no
/// local recovered at the run's own delta means the fold is left
/// unapplied, same policy `StackLoadFilter` already gives
/// findFoldableStackLoads).
[[nodiscard]] std::unordered_map<uint32_t, FoldableStringStore> findFoldableStringStores(
    const il::Function& function, const StackFrame& frame);

}  // namespace xdec::analysis
