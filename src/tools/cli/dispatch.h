// Command dispatch: maps `argv[1:]` to the command it names.
#pragma once

#include <span>
#include <string_view>

namespace xdec::cli {

/// `args` is argv with the program name stripped, so `args[0]` (if any) is the
/// command name. Returns the process exit code.
int dispatch(std::span<const std::string_view> args);

}  // namespace xdec::cli
