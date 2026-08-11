#include "dispatch.h"

#include <cstdint>
#include <string_view>

#include "common.h"

namespace xdec::cli {

// Forward declarations for the command handlers, defined across the
// cmd_*.cpp files (grouped by domain).
int commandInfo(std::string_view path);
int commandSections(std::string_view path);
int commandSymbols(std::string_view path, uint64_t limit);
int commandRelocs(std::string_view path, uint64_t limit);
int commandRead(std::string_view path, uint64_t address, uint64_t size);
int commandMemDump(std::string_view path, std::string_view outPath);
int commandSpec(std::string_view path, std::string_view output);
int commandDisasm(std::string_view path, uint64_t address, uint64_t count);
int commandLift(std::string_view path, uint64_t address, uint64_t count);
int commandObserve(std::string_view path, uint64_t address,
                    std::span<const std::string_view> options);
int commandDecompile(std::string_view path, uint64_t address,
                      std::span<const std::string_view> options);
int commandTypes(std::span<const std::string_view> args);
int commandCoverage(std::string_view path, uint64_t limit);
int commandLogCategories();
int commandDecode();
int commandExec(std::string_view path, std::string_view workloadPath);

int dispatch(std::span<const std::string_view> args) {
  if (args.empty()) {
    return usage();
  }

  const std::string_view command = args[0];
  const auto requireArgs = [&args](std::size_t count) { return args.size() > count; };

  if (command == "log-categories") {
    return commandLogCategories();
  }
  if (command == "-h" || command == "--help" || command == "help") {
    usage();
    return 0;
  }
  if (command == "decode") {
    return commandDecode();
  }
  if (command == "types") {
    return commandTypes(std::span<const std::string_view>{args}.subspan(1));
  }
  if (!requireArgs(1)) {
    print("error: '{}' needs a binary path", command);
    return usage();
  }

  if (command == "spec") {
    return commandSpec(args[1], args.size() > 2 ? args[2] : std::string_view{});
  }
  if (command == "info") {
    return commandInfo(args[1]);
  }
  if (command == "sections") {
    return commandSections(args[1]);
  }
  if (command == "coverage") {
    uint64_t limit = 20;
    if (args.size() > 2 && !parseNumber(args[2], limit)) {
      print("error: '{0}' is not a number", args[2]);
      return 1;
    }
    return commandCoverage(args[1], limit);
  }
  if (command == "symbols" || command == "relocs") {
    uint64_t limit = 40;
    if (args.size() > 2 && !parseNumber(args[2], limit)) {
      print("error: '{}' is not a number", args[2]);
      return 1;
    }
    return command == "symbols" ? commandSymbols(args[1], limit) : commandRelocs(args[1], limit);
  }
  if (command == "exec") {
    if (!requireArgs(2)) {
      printLine("error: exec needs a binary and a workload file");
      return usage();
    }
    return commandExec(args[1], args[2]);
  }
  if (command == "memdump") {
    if (!requireArgs(2)) {
      printLine("error: memdump needs a binary and an output path");
      return usage();
    }
    return commandMemDump(args[1], args[2]);
  }
  if (command == "observe") {
    if (!requireArgs(2)) {
      printLine("error: observe needs a binary and a function address");
      return usage();
    }
    uint64_t address = 0;
    if (!parseNumber(args[2], address)) {
      print("error: '{}' is not a number", args[2]);
      return 1;
    }
    return commandObserve(args[1], address,
                           std::span<const std::string_view>{args}.subspan(3));
  }
  if (command == "decompile") {
    if (!requireArgs(2)) {
      printLine("error: decompile needs a binary and a function address");
      return usage();
    }
    uint64_t address = 0;
    if (!parseNumber(args[2], address)) {
      print("error: '{}' is not a number", args[2]);
      return 1;
    }
    return commandDecompile(args[1], address,
                             std::span<const std::string_view>{args}.subspan(3));
  }
  if (command == "read" || command == "disasm" || command == "lift") {
    if (!requireArgs(3)) {
      print("error: {} needs an address and a count", command);
      return usage();
    }
    uint64_t address = 0;
    uint64_t size = 0;
    if (!parseNumber(args[2], address) || !parseNumber(args[3], size)) {
      printLine("error: address and size must be numbers");
      return 1;
    }
    if (command == "read") {
      return commandRead(args[1], address, size);
    }
    return command == "disasm" ? commandDisasm(args[1], address, size)
                                : commandLift(args[1], address, size);
  }

  print("error: unknown command '{}'", command);
  return usage();
}

}  // namespace xdec::cli
