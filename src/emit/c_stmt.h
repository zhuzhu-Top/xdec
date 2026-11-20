// Thin facade bridging `CPrinter` (`c_printer.cpp`) to the split emit stages:
//   - dead_ops.{h,cpp}     : pre-emission vestigial IL op detection
//   - phi_lowering.{h,cpp} : Out-of-SSA phi edge plans & `__prev` snapshots
//   - ast_lower.{h,cpp}    : StructuredFunction + IL -> language-agnostic HlFunction
//   - backend_c.{h,cpp}    : HlFunction -> formatted C body
#pragma once

#include <string>

#include "c_context.h"
#include "c_expr.h"
#include "dead_ops.h"

namespace xdec::emit {

class StmtPrinter {
 public:
  StmtPrinter(CContext& context, ExprPrinter& expressions)
      : ctx_(context), expressions_(expressions) {}

  /// Lowers to `HlFunction` AST and renders the function body in C.
  [[nodiscard]] std::string run();

 private:
  CContext& ctx_;
  ExprPrinter& expressions_;
};

}  // namespace xdec::emit
