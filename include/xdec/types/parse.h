// Reading C declarations into a TypeDatabase.
//
// This is a parser for the declaration subset of C, not for C. It exists
// because the alternative — linking libclang — buys a full C frontend, a
// 40MB dependency, and a build that only works where an LLVM install does, in
// order to read files that contain nothing but structs, enums, typedefs and
// prototypes. What it accepts is documented in docs/06-type-import.md and
// tested against the presets in types/presets/.
//
// What it deliberately does not do: preprocess. There is no #include
// following, no conditional compilation, and macros are only understood as
// integer constants (`#define FOO 4` can be an array length; a function-like
// macro cannot). A header that needs more than this should be run through
// `cc -E` first and imported as the .hdecl it becomes.
//
// The failure policy is per-declaration, not per-file. A declaration this
// parser cannot read is skipped with a warning naming the line, and the rest
// of the file still imports — because a real header always contains one
// attribute spelling nobody anticipated, and losing 400 good declarations to
// it would make the feature useless.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "xdec/support/result.h"
#include "xdec/types/database.h"

namespace xdec::types {

struct ParseWarning {
  unsigned line = 0;
  std::string message;
};

struct ParseReport {
  /// Declarations successfully imported (types plus prototypes).
  unsigned accepted = 0;
  /// Declarations skipped. Every one of these has a warning.
  unsigned skipped = 0;
  std::vector<ParseWarning> warnings;

  [[nodiscard]] std::string format(std::string_view origin) const;
};

/// Parses declarations from `text` into `database`. `origin` names the source
/// in warnings. Fails only for errors that make the whole file meaningless
/// (unterminated comment, runaway brace); everything else is a warning.
[[nodiscard]] Result<ParseReport> parseHeader(std::string_view text, TypeDatabase& database);

[[nodiscard]] Result<ParseReport> parseHeaderFile(const std::string& path,
                                                  TypeDatabase& database);

/// Resolves a preset name ("android-ndk") or a path to a readable file.
/// Presets live in `types/presets/<name>.hdecl` next to the executable's
/// source tree; a name with a directory separator or an extension is taken as
/// a path and returned unchanged.
[[nodiscard]] Result<std::string> resolveHeaderPath(std::string_view nameOrPath);

}  // namespace xdec::types
