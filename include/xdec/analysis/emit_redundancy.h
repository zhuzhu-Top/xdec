// A quantified, IL-level snapshot of the redundant-intermediate-variable
// shapes the emit layer's own folding passes target -- see
// docs/09-expression-reuse.md's taxonomy (shapes F through J) and
// docs/14-emit-redundancy.md's framework overview.
//
// Deliberately IL-level, not text-level: this counts what stack_load_fold.h
// and stack_store_fold.h can prove from the IL and VariableTable alone,
// independent of how CContext or the emitter's own per-scope CSE end up
// spelling the result. That is the half a phase's own before/after is
// measured against here; the complementary text-level counts (`_cseN = ...`
// line counts, and the like, which are an emitted-scope fact these analyses
// do not have) come from counting the printed .c file instead (see
// tools/emit_metrics.ps1) -- the two are meant to be read side by side, not
// merged into one number.
#pragma once

#include <cstddef>
#include <string>

#include "xdec/analysis/stack_frame.h"
#include "xdec/analysis/variables.h"
#include "xdec/il/function.h"

namespace xdec::analysis {

struct EmitRedundancyReport {
  /// Loads whose address classifies as a stack slot.
  std::size_t stackLoads = 0;
  /// Of those, how many findFoldableStackLoads resolves (shape F).
  std::size_t stackLoadsFolded = 0;
  /// Stores whose address classifies as a stack slot.
  std::size_t stackStores = 0;
  /// Of those, how many findDeadStackStores resolves (shape H1).
  std::size_t stackStoresDead = 0;
  /// Recovered locals with zero Load readers anywhere in the function --
  /// write-only stack slots, the variable-level fact behind shape H1's
  /// dead stores. Not the same count as `stackStoresDead`: one such local
  /// can have several Stores, and this counts the local once.
  std::size_t writeOnlyLocals = 0;

  [[nodiscard]] std::string format() const;
};

[[nodiscard]] EmitRedundancyReport analyzeEmitRedundancy(const il::Function& function,
                                                         const StackFrame& frame,
                                                         const VariableTable& variables);

}  // namespace xdec::analysis
