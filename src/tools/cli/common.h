// Shared, dependency-free CLI plumbing: output, number parsing, and the help
// text. Every command file in cli/ builds on this without pulling in the
// binary/spec/analysis layers that session.h needs.
#pragma once

#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

#include "xdec/support/diag.h"

namespace xdec::cli {

void printLine(std::string_view text);

template <class... Args>
void print(std::format_string<Args...> fmt, Args&&... args) {
  printLine(std::format(fmt, std::forward<Args>(args)...));
}

/// Parses a decimal or 0x-prefixed hexadecimal integer.
bool parseNumber(std::string_view text, uint64_t& out);

int reportError(const xdec::Diag& diag);

/// Prints the full `xdec <command>` usage text and returns the exit code
/// every no-args / unknown-command path uses.
int usage();

}  // namespace xdec::cli
