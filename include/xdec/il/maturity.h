// Maturity levels.
//
// A maturity is a contract, not a label: at each level a specific set of
// invariants holds, the verifier enforces them, and passes declare the level
// they operate at. This is what keeps a long pass pipeline debuggable -- when
// output is wrong you bisect by dumping at each level rather than by reading
// every pass.
#pragma once

#include <cstdint>
#include <string_view>

namespace xdec::il {

enum class Maturity : uint8_t {
  /// One-to-one with machine instructions. Values are block-local, every op
  /// carries the address of the instruction it came from, and no analysis has
  /// run. Nothing has been simplified, so this level is the reference that
  /// semantic differential testing compares against.
  Lifted = 0,

  /// Block-local cleanup: dead value elimination inside a block, constant
  /// folding, and lazy-flag folding where both operands are known.
  Local,

  /// The control flow graph is complete for direct edges. Every block has an
  /// explicit terminator, successor and predecessor lists agree, and unresolved
  /// indirect branches are marked as such rather than left dangling.
  Cfg,

  /// Static single assignment over machine registers and memory. Cross-block
  /// dataflow becomes explicit; phi nodes exist.
  Ssa,

  /// Indirect branches and calls have been resolved, or explicitly recorded as
  /// unresolvable with the evidence for why. Control-flow flattening is undone
  /// at this level, when the profile said the function was flattened.
  Resolved,

  /// Optimised: constant and copy propagation, dead code elimination, and
  /// algebraic and MBA simplification have reached a fixed point.
  Optimized,

  /// Stack slots and registers have become variables, calling convention and
  /// prologue idioms are recognised, and call signatures are known.
  Vars,

  /// Control flow has been structured into a high-level AST.
  Structured,

  /// Types have been inferred.
  Typed,
};

inline constexpr unsigned kMaturityCount = 9;

/// Round-trippable lowercase name.
[[nodiscard]] std::string_view toString(Maturity maturity) noexcept;
[[nodiscard]] bool parseMaturity(std::string_view text, Maturity& out) noexcept;

/// A short description of the invariants the level guarantees.
[[nodiscard]] std::string_view describe(Maturity maturity) noexcept;

}  // namespace xdec::il
