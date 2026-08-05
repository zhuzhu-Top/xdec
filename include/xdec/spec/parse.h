// Front end for the instruction semantics DSL.
#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

#include "xdec/spec/ast.h"
#include "xdec/support/result.h"

namespace xdec::spec {

/// Parses one spec file. Reports the first syntax error with its location;
/// nothing downstream can be trusted once the shape of the file is unclear.
///
/// Any `include` the file names is recorded but not read: a caller holding only
/// text has nowhere to read it from. Use `parseSpecFile` for a spec on disk.
[[nodiscard]] Result<std::unique_ptr<Module>> parseModule(std::string_view text,
                                                          std::string_view sourceName);

/// Parses declarations without an `arch` block, which is what an included file
/// holds. Exposed for tests; `parseSpecFile` is what reads a real spec.
[[nodiscard]] Result<std::unique_ptr<Module>> parseFragment(std::string_view text,
                                                            std::string_view sourceName);

/// Parses a spec and everything it includes, depth first, with include paths
/// resolved against the directory of the file naming them. The result is one
/// module: which file a rule came from survives only in its diagnostics, because
/// nothing downstream has any business caring.
[[nodiscard]] Result<std::unique_ptr<Module>> parseSpecFile(const std::filesystem::path& path);

/// Parses a standalone expression, for tests and for the asm template parser.
[[nodiscard]] Result<ExprPtr> parseExpression(std::string_view text,
                                              std::string_view sourceName);

}  // namespace xdec::spec
