// Constant evaluation of pure expressions.
//
// Two consumers share this code, and sharing is the point:
//
//   - the interpreter materialises flag bundles with these formulas on every
//     FlagDef, so they are continuously validated against Unicorn;
//   - the constant folder evaluates whole expression trees with the same
//     formulas, so a fold can never introduce a semantics the oracle would
//     not also produce. A fold that disagrees with the interpreter is a bug
//     in one place, not two implementations drifting.
//
// `tryEvalConst` covers integer widths up to 64. Wider expressions (the
// 128-bit forms the AArch64 lifter can build) are left unevaluated rather
// than truncated: a fold that does not happen is a missed optimisation, a
// fold that happens wrong is a wrong binary.
#pragma once

#include <cstdint>
#include <span>

#include "xdec/il/expr.h"
#include "xdec/il/function.h"
#include "xdec/il/interp.h"

namespace xdec::il {

/// The NZCV bundle a flag-defining op produces over concrete operands, in
/// bits 3..0. The formulas are the ones the Unicorn differential checks.
[[nodiscard]] uint8_t evalFlagDef(FlagOp op, unsigned width,
                                  std::span<const ConcreteValue> operands) noexcept;

/// Whether an architecture-neutral condition holds over a materialised NZCV.
[[nodiscard]] bool evalCondition(ConditionCode code, uint8_t nzcv) noexcept;

/// Whole-expression constant evaluation. Succeeds exactly when every leaf is
/// a Const and every op along the way is in the covered set (integer ops up
/// to 64 bits, casts, bit counts, select, and the flag triple); the output is
/// masked to the expression's width. Division and remainder by zero produce
/// zero, matching the interpreter and AArch64 hardware. Depth-bounded against
/// pathological trees.
[[nodiscard]] bool tryEvalConst(const Function& function, ExprId id,
                                ConcreteValue& out) noexcept;

}  // namespace xdec::il
