#include "c_stmt.h"

#include "ast_lower.h"
#include "backend_c.h"

namespace xdec::emit {

std::string StmtPrinter::run() {
  const HlFunction hl = lowerToHlAst(ctx_, expressions_);
  return hl.body ? renderCBody(*hl.body, /*baseIndent=*/1) : std::string{};
}

}  // namespace xdec::emit
