// Obfuscation profiling: what a function is defended with, measured rather
// than guessed.
//
// The profile exists so deobfuscation passes are chosen by evidence. Every
// signal here is a number with a definition; the judgements (flattened or
// not, MBA-heavy or not) are thresholds over those numbers, documented where
// they are set and reported with the numbers behind them — when a judgement
// is wrong, the fix is a threshold, not archaeology.
#pragma once

#include <cstdint>
#include <string>

#include "xdec/il/function.h"

namespace xdec::analysis {

struct ObfuscationProfile {
  /// Indirect branches (computed jumps), and how many still have no targets.
  uint32_t indirectBranches = 0;
  uint32_t unresolvedIndirect = 0;
  /// Calls through a value rather than an address.
  uint32_t indirectCalls = 0;

  /// Blocks in the largest strongly connected component. Post-resolution this
  /// carries the flattening signal; pre-resolution the dispatcher's edges are
  /// exactly what is missing, so see dispatcherFanIn instead.
  uint32_t blocks = 0;
  uint32_t largestScc = 0;
  /// The highest in-degree of any block ending in an unresolved indirect
  /// branch. A flattened dispatcher is defined by its fan-in: every state
  /// block loops back to it while its own edges out stay unresolved.
  uint32_t dispatcherFanIn = 0;

  /// Expressions matching the MBA signature: arithmetic and bitwise ops mixed
  /// in one tree, deeper than honest arithmetic bothers to be.
  uint32_t mbaExpressions = 0;

  /// Judgements, each a documented threshold over the numbers above.
  [[nodiscard]] bool likelyFlattened() const noexcept;
  [[nodiscard]] bool likelyMba() const noexcept;

  /// One line per signal, for observe output and the profile unit test.
  [[nodiscard]] std::string format() const;
};

[[nodiscard]] ObfuscationProfile profile(const il::Function& function);

}  // namespace xdec::analysis
