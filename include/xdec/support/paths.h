// Portable runtime lookup for shipped data files (specs, type presets,
// syscall tables) and bundled plugins relative to the running executable.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace xdec {

/// Returns the directory containing the currently running executable, or an
/// empty path if the platform query fails.
[[nodiscard]] std::filesystem::path executableDirectory();

/// Resolves a shipped data file so portable/installed binaries work outside
/// the build tree while in-tree builds continue to find source-tree data.
///
/// Search order:
///   1. <exeDir>/<relativeFromRoot>
///   2. <exeDir>/../<relativeFromRoot>
///   3. <exeDir>/../share/xdec/<relativeFromRoot>
///   4. <compiledDir>/<relativeFromCompiledDir>  (returned even if missing so
///      caller diagnostics still report a concrete expected path)
[[nodiscard]] std::filesystem::path findShippedDataFile(
    const std::filesystem::path& relativeFromRoot, std::string_view compiledDir,
    const std::filesystem::path& relativeFromCompiledDir);

/// Locates a bundled plugin shared library by stem (e.g. "objc_libcxx") next
/// to the executable (or in `../lib`), matching platform naming with or
/// without a `lib` prefix. Returns empty string if not present.
[[nodiscard]] std::string defaultPluginPath(std::string_view stem);

}  // namespace xdec
