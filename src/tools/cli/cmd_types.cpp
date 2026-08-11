// types parse: import C declarations and report, or cache, what they declared.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common.h"
#include "session.h"

namespace xdec::cli {

/// `xdec types parse` — read headers and report, or cache, what they declared.
///
/// This exists because the failure mode of an import is silence: a header that
/// half-parsed produces a decompilation that is subtly less typed than
/// expected, with nothing pointing at the header. Running the parser on its own
/// makes what was imported, and what was skipped, the whole output.
int commandTypes(std::span<const std::string_view> args) {
  if (args.empty() || args[0] != "parse") {
    printLine("usage: xdec types parse <header|preset>... [-o <out.json>] [--definitions]");
    return 2;
  }

  std::vector<std::string> sources;
  std::filesystem::path outPath;
  bool definitions = false;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string_view option = args[i];
    if (option == "-o" || option == "--out") {
      if (++i >= args.size()) {
        printLine("error: -o needs a path");
        return 1;
      }
      outPath = std::filesystem::path{std::string{args[i]}};
    } else if (option == "--definitions") {
      definitions = true;
    } else if (option.starts_with("-")) {
      print("error: unknown types option '{}'", option);
      return 1;
    } else {
      sources.emplace_back(option);
    }
  }
  if (sources.empty()) {
    printLine("error: types parse needs at least one header or preset name");
    return 1;
  }

  auto database = loadTypes(sources, /*verbose=*/true);
  if (!database) {
    return reportError(database.error());
  }
  print("imported {} type(s), {} declaration(s)", database->typeCount(),
        database->declarations().size());

  if (definitions) {
    print("{}", database->formatDefinitions());
  }
  if (!outPath.empty()) {
    const std::string text = database->toJson().dump();
    std::ofstream stream(outPath, std::ios::binary | std::ios::trunc);
    if (!stream) {
      print("error: cannot open '{}' for writing", outPath.string());
      return 1;
    }
    stream << text;
    print("wrote '{}' ({} bytes)", outPath.string(), text.size());
  }
  return 0;
}

}  // namespace xdec::cli
