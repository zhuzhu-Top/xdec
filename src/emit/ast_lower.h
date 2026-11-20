// Lowers `StructuredFunction` + `il::Function` + Out-of-SSA Phi edge plans
// into a target-language-agnostic `HlFunction` AST (`include/xdec/emit/ast.h`).
#pragma once

#include "c_context.h"
#include "c_expr.h"
#include "xdec/emit/ast.h"

namespace xdec::emit {

[[nodiscard]] HlFunction lowerToHlAst(CContext& context, ExprPrinter& expressions);

}  // namespace xdec::emit
