// Lowering a checked spec to bytecode.
#pragma once

#include <filesystem>
#include <memory>

#include "xdec/spec/check.h"
#include "xdec/spec/program.h"
#include "xdec/support/result.h"

namespace xdec::spec {

/// Lowers a module that has already passed checking. Anything the checker would
/// have rejected is an internal error here rather than a diagnostic: the
/// compiler's job is translation, not validation, and mixing the two is how a
/// half-checked spec reaches the engine.
[[nodiscard]] Result<std::unique_ptr<SpecProgram>> compile(const Module& module,
                                                           const CheckedModule& checked);

/// Parse, check and compile in one step.
[[nodiscard]] Result<std::unique_ptr<SpecProgram>> compileSource(std::string_view text,
                                                                 std::string_view sourceName);

/// The same, for a spec on disk, following its includes.
[[nodiscard]] Result<std::unique_ptr<SpecProgram>> compileFile(const std::filesystem::path& path);

/// Writes a program to the blob format. The blob is what a release build ships:
/// loading it skips parsing and checking entirely.
[[nodiscard]] std::vector<std::byte> serialize(const SpecProgram& program);

[[nodiscard]] Result<std::unique_ptr<SpecProgram>> deserialize(std::span<const std::byte> bytes);

}  // namespace xdec::spec
