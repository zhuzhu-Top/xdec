// Flag conditions as C.
//
// A flagdef that survived to Resolved is a condition the analyses could not
// fold into an integer comparison. Printing it exactly matters: the cheap
// conditions become plain C comparisons, the overflow-sensitive ones go to a
// helper that computes N and V the way the hardware does, and a def nobody has
// needed yet becomes a loud stub rather than a plausible-looking guess.
#pragma once

#include <string>

#include "c_context.h"

namespace xdec::emit {

class ExprPrinter;

/// `id` must be a FlagCond expression.
[[nodiscard]] std::string printFlagCond(CContext& context, ExprPrinter& expressions,
                                        il::ExprId id);

/// The NZCV truth table for a literal bundle, as "1" or "0".
[[nodiscard]] std::string flagFromNzcv(il::ConditionCode code, uint64_t nzcv);

}  // namespace xdec::emit
