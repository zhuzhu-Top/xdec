#include "dispatch.h"

#include <cstdint>
#include <string_view>

#include "common.h"

namespace xdec::cli {

// Forward declarations for the command handlers, defined across the
// cmd_*.cpp files (grouped by domain).
int commandInfo(std::string_view path);
int commandImages(std::string_view path, uint64_t limit);
int commandCacheLocate(std::string_view path, uint64_t address);
int commandSections(std::string_view path);
int commandSymbols(std::string_view path, std::span<const std::string_view> options);
int commandRelocs(std::string_view path, uint64_t limit);
int commandRead(std::string_view path, uint64_t address, uint64_t size);
int commandStrings(std::string_view path, std::span<const std::string_view> options);
int commandXrefs(std::string_view path, std::string_view query,
                 std::span<const std::string_view> options);
int commandFunctions(std::string_view path, std::span<const std::string_view> options);
int commandObjc(std::string_view path, std::span<const std::string_view> options);
int commandData(std::string_view path, uint64_t address, uint64_t count,
                std::span<const std::string_view> options);
int commandCallers(std::string_view path, std::string_view query,
                   std::span<const std::string_view> options);
int commandCallees(std::string_view path, std::string_view query,
                   std::span<const std::string_view> options);
int commandServe(std::string_view path, std::span<const std::string_view> options);
int commandMemDump(std::string_view path, std::string_view outPath);
int commandSpec(std::string_view path, std::string_view output);
int commandDisasm(std::string_view path, uint64_t address, uint64_t count);
int commandLift(std::string_view path, uint64_t address, uint64_t count);
int commandObserve(std::string_view path, uint64_t address,
                    std::span<const std::string_view> options);
int commandDecompile(std::string_view path, std::span<const std::string_view> args);
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
  const auto hasHelpFlag = [&args]() {
    for (std::size_t i = 1; i < args.size(); ++i) {
      if (args[i] == "-h" || args[i] == "--help") {
        return true;
      }
    }
    return false;
  };
  if (hasHelpFlag()) {
    if (command == "strings") {
      printLine("usage: xdec strings <binary> [--grep <pat>] [--min-len <n>] [--section <sec>] [--limit <n>] [--xrefs] [--json]");
      return 0;
    }
    if (command == "xrefs") {
      printLine("usage: xdec xrefs <binary> <addr|symbol|sel:<name>|str:<text>> [--limit <n>] [--no-stubs] [--json]");
      return 0;
    }
    if (command == "functions") {
      printLine("usage: xdec functions <binary> [--grep <pat>] [--limit <n>] [--json]");
      return 0;
    }
    if (command == "objc") {
      printLine("usage: xdec objc <binary> [--class <pat>] [--sel <pat>] [--limit <n>] [--json]");
      return 0;
    }
    if (command == "data") {
      printLine("usage: xdec data <binary> <address> [count] [--json]");
      return 0;
    }
    if (command == "symbols") {
      printLine("usage: xdec symbols <binary> [count] [--grep <pat>] [--kind func|import|export|data] [--limit <n>]");
      return 0;
    }
    if (command == "decompile") {
      printLine("usage: xdec decompile <binary> <addr|-s sym|--match glob> [options]");
      printLine("  -o <file.c>          output single function to file");
      printLine("  --out-dir <dir>      output batch/multi-target functions to directory");
      printLine("  -j <threads>         parallel worker count for batch mode");
      printLine("  --plugin <path>      load plugin (repeatable)");
      printLine("  --types <hdr|preset> import C declarations");
      printLine("  --allow-unresolved   seal unresolved indirect branches instead of failing");
      printLine("  --lang <c|objc>      output language dialect (default: c)");
      printLine("  --verbose-notes      keep verbose ABI/GOT prose comments");
      printLine("  --no-objc-syntax     keep arg1/arg2 instead of self/_cmd in ObjC methods");
      return 0;
    }
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
    printError("'{}' needs a binary path", command);
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
  if (command == "images") {
    uint64_t limit = 200;
    if (args.size() > 2 && !parseNumber(args[2], limit)) {
      printError("'{}' is not a number", args[2]);
      return 1;
    }
    return commandImages(args[1], limit);
  }
  if (command == "cache-locate") {
    if (!requireArgs(2)) {
      printError("cache-locate needs a cache path and an address");
      return usage();
    }
    uint64_t address = 0;
    if (!parseNumber(args[2], address)) {
      printError("'{}' is not a number", args[2]);
      return 1;
    }
    return commandCacheLocate(args[1], address);
  }
  if (command == "coverage") {
    uint64_t limit = 20;
    if (args.size() > 2 && !parseNumber(args[2], limit)) {
      printError("'{0}' is not a number", args[2]);
      return 1;
    }
    return commandCoverage(args[1], limit);
  }
  if (command == "symbols") {
    return commandSymbols(args[1], std::span<const std::string_view>{args}.subspan(2));
  }
  if (command == "relocs") {
    uint64_t limit = 40;
    if (args.size() > 2 && !parseNumber(args[2], limit)) {
      printError("'{}' is not a number", args[2]);
      return 1;
    }
    return commandRelocs(args[1], limit);
  }
  if (command == "strings") {
    return commandStrings(args[1], std::span<const std::string_view>{args}.subspan(2));
  }
  if (command == "functions") {
    return commandFunctions(args[1], std::span<const std::string_view>{args}.subspan(2));
  }
  if (command == "objc") {
    return commandObjc(args[1], std::span<const std::string_view>{args}.subspan(2));
  }
  if (command == "xrefs") {
    if (!requireArgs(2)) {
      printError("xrefs needs a binary and a target (address, symbol, sel:..., or str:...)");
      return usage();
    }
    return commandXrefs(args[1], args[2], std::span<const std::string_view>{args}.subspan(3));
  }
  if (command == "callers") {
    if (!requireArgs(2)) {
      printError("callers needs a binary and a target (address or symbol)");
      return usage();
    }
    return commandCallers(args[1], args[2], std::span<const std::string_view>{args}.subspan(3));
  }
  if (command == "callees") {
    if (!requireArgs(2)) {
      printError("callees needs a binary and a function (address or symbol)");
      return usage();
    }
    return commandCallees(args[1], args[2], std::span<const std::string_view>{args}.subspan(3));
  }
  if (command == "serve") {
    return commandServe(args[1], std::span<const std::string_view>{args}.subspan(2));
  }
  if (command == "data") {
    if (!requireArgs(2)) {
      printError("data needs a binary and an address");
      return usage();
    }
    uint64_t address = 0;
    if (!parseNumber(args[2], address)) {
      printError("'{}' is not a number", args[2]);
      return 1;
    }
    uint64_t count = 8;
    std::size_t optStart = 3;
    if (args.size() > 3 && !args[3].starts_with("-")) {
      if (!parseNumber(args[3], count)) {
        printError("'{}' is not a number", args[3]);
        return 1;
      }
      optStart = 4;
    }
    return commandData(args[1], address, count,
                       std::span<const std::string_view>{args}.subspan(optStart));
  }
  if (command == "exec") {
    if (!requireArgs(2)) {
      printError("exec needs a binary and a workload file");
      return usage();
    }
    return commandExec(args[1], args[2]);
  }
  if (command == "memdump") {
    if (!requireArgs(2)) {
      printError("memdump needs a binary and an output path");
      return usage();
    }
    return commandMemDump(args[1], args[2]);
  }
  if (command == "observe") {
    if (!requireArgs(2)) {
      printError("observe needs a binary and a function address");
      return usage();
    }
    uint64_t address = 0;
    if (!parseNumber(args[2], address)) {
      printError("'{}' is not a number", args[2]);
      return 1;
    }
    return commandObserve(args[1], address,
                           std::span<const std::string_view>{args}.subspan(3));
  }
  if (command == "decompile") {
    if (!requireArgs(2)) {
      printError("decompile needs a binary and a function address (or -s / --match)");
      return usage();
    }
    return commandDecompile(args[1], std::span<const std::string_view>{args}.subspan(2));
  }
  if (command == "read" || command == "disasm" || command == "lift") {
    if (!requireArgs(3)) {
      printError("{} needs an address and a count", command);
      return usage();
    }
    uint64_t address = 0;
    uint64_t size = 0;
    if (!parseNumber(args[2], address) || !parseNumber(args[3], size)) {
      printError("address and size must be numbers");
      return 1;
    }
    if (command == "read") {
      return commandRead(args[1], address, size);
    }
    return command == "disasm" ? commandDisasm(args[1], address, size)
                                : commandLift(args[1], address, size);
  }

  printError("unknown command '{}'", command);
  return usage();
}

}  // namespace xdec::cli
