// Renders a lowered `HlFunction` / `HlStmt` AST into C text.
#pragma once

#include <string>

#include "xdec/emit/ast.h"

namespace xdec::emit {

[[nodiscard]] std::string renderCBody(const HlStmt& root, unsigned baseIndent = 1);

}  // namespace xdec::emit
