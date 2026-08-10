// Which expressions an op hands to a C emitter (or an analysis over the same
// shape) as one of "the values this statement needs printed."
//
// Factored out of emit/c_stmt.cpp so that xdec::analysis::expr_reuse can walk
// exactly the same root set a real StmtPrinter would use, without the two
// drifting apart as new op shapes are added.
#pragma once

#include <set>
#include <vector>

#include "xdec/il/function.h"

namespace xdec::il {

/// Every expression `op` contributes as a root for CSE/reuse purposes:
/// an address a Load reads through, the operands a Store/Call/Intrinsic
/// hands to memory or a callee, or the one value a WriteReg/Return carries
/// out of the block. Mirrors exactly what StmtPrinter::printOp ends up
/// asking the expression printer for, so a block's ops can be counted (or
/// scanned for reuse) before any of them is printed.
void addExprRoots(const Function& function, const Op& op, std::vector<ExprId>& roots);

/// Every `Value` leaf under `root`, iteratively: expression trees here are
/// DAGs deep enough that recursion is a stack-overflow risk on obfuscated
/// input. Shared by the emitter's phi-copy ordering and anything else that
/// needs "which SSA values does this expression actually read."
void collectValueLeaves(const Function& function, ExprId root, std::set<uint32_t>& out);

}  // namespace xdec::il
