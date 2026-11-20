// Renders a lowered `HlFunction` / `HlStmt` AST into Objective-C syntax.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xdec/emit/ast.h"

namespace xdec::emit {

[[nodiscard]] std::string renderObjcBody(const HlStmt& root, unsigned baseIndent = 1);

/// Formats a Mach-O Objective-C method display name (`-[Class sel:]` /
/// `+[Class sel:]`), optional `@encode` string, and recovered parameter
/// `(type, name)` pairs into an Objective-C method signature line.
[[nodiscard]] std::string formatObjcMethodSignature(
    std::string_view displayName, std::string_view typeEncoding, std::string_view returnType,
    const std::vector<std::pair<std::string, std::string>>& params);

}  // namespace xdec::emit
