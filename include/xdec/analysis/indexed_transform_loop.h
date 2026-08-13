// Recognising an indexed-transform loop: `dst[i] = f(src[i], key)` walked
// under an induction variable, however the surrounding control flow got
// there.
//
// A flattening obfuscator's cipher/checksum loops keep exactly the same
// per-element computation an unobfuscated loop would, wrapped in a state
// machine that turns the increment and the array accesses each into their
// own dispatcher case. None of that changes what the loop IL-level actually
// does: one induction phi at the header, and somewhere reachable from it a
// load and a store at the same scaled index, related by one invertible
// binary op. Recovering that triple lets emission say what the loop is
// shaped like even when nothing here decides what array or key means.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "xdec/analysis/loops.h"
#include "xdec/il/function.h"

namespace xdec::analysis {

/// The per-element operation relating a loop's load to its store. Every one
/// of these has an obvious inverse (xor undoes itself, add/sub swap, a
/// rotate reverses by rotating the other way) -- the property that lets a
/// reader tell a real transform loop from an unrelated copy loop just from
/// the op name, without knowing what the loop is for.
enum class TransformOp : uint8_t { Xor, Add, Sub, RotateLeft, RotateRight };

[[nodiscard]] std::string_view toString(TransformOp op) noexcept;

/// One `dst[base + idx*scale] = src[base + idx*scale] <op> key` triple,
/// recovered purely from IL shape: an induction phi at `loop`'s header, and
/// a load/store pair sharing a block that both index off it. Says nothing
/// about what the arrays or key mean -- only that the loop's body walks two
/// of them in lockstep through one named, invertible operation.
struct IndexedTransformLoop {
  il::BlockId header;
  il::ValueId index;
  /// The index's value on the loop's first iteration, when every edge into
  /// the header outside the loop agrees on it. Unset when they do not --
  /// still a real induction variable, just not one with a single known
  /// start.
  std::optional<uint64_t> indexStart;
  /// Net change to the index each iteration: +1 for `idx = idx + 1`, -1 for
  /// `idx = idx - 1`.
  int64_t indexStride = 0;
  il::ExprId srcBase{};
  il::ExprId dstBase{};
  /// The multiplier applied to the index in both addresses (shared -- a
  /// mismatched pair is not this pattern).
  uint64_t elementScale = 0;
  TransformOp op = TransformOp::Xor;
  /// `op`'s other operand, whatever it is built from: often loop-invariant,
  /// sometimes itself indexed by the same `index` (a second array read as a
  /// key stream).
  il::ExprId key{};
  /// Whether the loaded element is `op`'s first operand (`src[i] - key`) or
  /// second (`key - src[i]`). Meaningless for the commutative ops.
  bool loadIsFirstOperand = true;
  il::BlockId block;
  il::OpId loadOp;
  il::OpId storeOp;
};

/// Matches an `IndexedTransformLoop` against `loop`, or nullopt when no
/// induction phi at its header has a load and a store, sharing one block,
/// both scaled by it and related through one of the ops above. Deliberately
/// restricted to a single block: that is what the pattern's own "one case
/// body per iteration" shape keeps together, so a transform actually split
/// across blocks is left alone rather than guessed at.
[[nodiscard]] std::optional<IndexedTransformLoop> matchIndexedTransformLoop(
    const il::Function& function, const NaturalLoop& loop);

}  // namespace xdec::analysis
