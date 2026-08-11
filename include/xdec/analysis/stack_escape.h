// Where a stack address escapes the function -- and, when it does through a
// pointer passed on to a callee, how far past that one delta the callee may
// still read or write.
//
// findDeadStackStores (stack_store_fold.h) already treats a stack address
// handed to a Call/Intrinsic as an escape of the exact delta the pointer
// itself denotes -- but a pointer is rarely a promise about only the byte it
// points at. `sub_2f949c(flags, &var_70)` in bc_lib's `sub_2f9a38` reads
// `var_70`'s own qword *and* the two qwords above it (`arg2+0x8`,
// `arg2+0x10`); nothing in that function ever loads any of the three back,
// so without this, stack-store-fold would (correctly, by its own narrower
// rule) call only `var_70`'s own store live and quietly drop the other two --
// an aggregate initializer with a field silently missing, and that field's
// source value looking unused in the emitted C.
//
// This is deliberately a separate analysis from stack-store-fold's own
// escape detection, not a rule folded into it: `escapedDeltas` there answers
// "does *this* delta escape", one bit per delta. This answers a related but
// distinct question -- "how wide is the region a callee might touch,
// starting from an escaped delta" -- and is reusable anywhere else that
// question comes up, without stack-store-fold needing to grow a second
// responsibility.
#pragma once

#include <cstdint>
#include <vector>

#include "xdec/analysis/stack_frame.h"
#include "xdec/il/function.h"

namespace xdec::analysis {

enum class StackEscapeReason : uint8_t {
  /// The delta itself is an operand a Call/Intrinsic/other non-address use
  /// holds -- the base fact stack-store-fold's own escape scan already
  /// captured before this existed.
  Point,
  /// `Point`, closed over every Store whose own footprint touches or
  /// overlaps the region as it grows -- the aggregate-buffer case: a callee
  /// may read past the escaped pointer's own byte, and every write into
  /// that footprint is exactly as unobservable-from-here as the pointer's
  /// own slot is, not because anything proves the callee reads it, but
  /// because nothing here can rule it out either.
  StoreFootprint,
  // future: CalleeType -- widen a StoreFootprint region to a resolved
  // parameter type's own sizeof (types::TypeBinder), for a callee prototype
  // that claims more than its caller's own stores prove. Add the reason and
  // a resolution step to compute(); no consumer of isEscaped() would need
  // to change.
};

struct StackEscapeRegion {
  int64_t baseDelta = 0;
  uint64_t sizeBytes = 1;
  StackEscapeReason reason = StackEscapeReason::Point;
};

/// Every region of the frame `findDeadStackStores` must leave alone: at
/// least the one delta each pointer escape denotes, widened to cover every
/// Store whose own footprint is contiguous with it. A delta no escape ever
/// reaches -- including every delta in a function with no stack pointer
/// register at all (see StackFrame::compute's own degradation) -- answers
/// false, exactly as if this had never been consulted.
class StackEscapeMap {
 public:
  [[nodiscard]] static StackEscapeMap compute(const il::Function& function,
                                               const StackFrame& frame);

  [[nodiscard]] bool isEscaped(int64_t delta) const;
  [[nodiscard]] const std::vector<StackEscapeRegion>& regions() const { return regions_; }

 private:
  std::vector<StackEscapeRegion> regions_;
};

}  // namespace xdec::analysis
