// observe, decompile: the two commands that run the pass pipeline. The
// heavyweight file of the set -- decompile alone exercises analysis, emit and
// the decompile driver in one function.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "common.h"
#include "options.h"
#include "run_context.h"
#include "session.h"
#include "xdec/analysis/dispatch_region.h"
#include "xdec/analysis/emit_redundancy.h"
#include "xdec/analysis/execution_coverage.h"
#include "xdec/analysis/expr_reuse.h"
#include "xdec/analysis/path_explorer.h"
#include "xdec/analysis/plt_stub.h"
#include "xdec/analysis/profile.h"
#include "xdec/binary/macho_metadata.h"
#include "xdec/decompile/driver.h"
#include "xdec/decompile/emit.h"
#include "xdec/il/printer.h"
#include "xdec/pass/observe.h"
#include "xdec/pass/registry.h"
#include "xdec/passes/builtin.h"
#include "xdec/plugin/loader.h"
#include "xdec/obs/span.h"
#include "xdec/spec/lift.h"
#include "xdec/storage/tracedb.h"
#include "xdec/support/json.h"
#include "xdec/types/syscall_table.h"

namespace xdec::cli {

namespace {

[[nodiscard]] bool globMatchCI(std::string_view pattern, std::string_view text) noexcept {
  std::size_t p = 0;
  std::size_t t = 0;
  std::size_t starP = std::string_view::npos;
  std::size_t starT = 0;
  while (t < text.size()) {
    if (p < pattern.size() &&
        (pattern[p] == '?' ||
         std::tolower(static_cast<unsigned char>(pattern[p])) ==
             std::tolower(static_cast<unsigned char>(text[t])))) {
      ++p;
      ++t;
    } else if (p < pattern.size() && pattern[p] == '*') {
      starP = p++;
      starT = t;
    } else if (starP != std::string_view::npos) {
      p = starP + 1;
      t = ++starT;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') {
    ++p;
  }
  return p == pattern.size();
}

[[nodiscard]] bool matchesQueryCI(std::string_view hay, std::string_view needle) noexcept {
  if (needle.empty()) {
    return true;
  }
  if (needle.find('*') != std::string_view::npos ||
      needle.find('?') != std::string_view::npos) {
    return globMatchCI(needle, hay);
  }
  return hay.size() >= needle.size() &&
         std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                     [](char a, char b) {
                       return std::tolower(static_cast<unsigned char>(a)) ==
                              std::tolower(static_cast<unsigned char>(b));
                     }) != hay.end();
}

[[nodiscard]] std::string defaultObjcPluginPath() {
  std::filesystem::path exeDir;
#ifdef _WIN32
  std::wstring buf(1024, L'\0');
  const DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
  if (len > 0 && len < buf.size()) {
    buf.resize(len);
    exeDir = std::filesystem::path{buf}.parent_path();
  }
#endif
  if (exeDir.empty()) {
    return {};
  }
#ifdef _WIN32
  const std::filesystem::path cand = exeDir / "objc_libcxx.dll";
#elif defined(__APPLE__)
  const std::filesystem::path cand = exeDir / "libobjc_libcxx.dylib";
#else
  const std::filesystem::path cand = exeDir / "libobjc_libcxx.so";
#endif
  std::error_code ec;
  if (std::filesystem::is_regular_file(cand, ec) && !ec) {
    return cand.string();
  }
  return {};
}

}  // namespace

/// Times a CLI-only diagnostic (`--reuse-report`/`--emit-report`) as an
/// "emit" span, like decompileToC's own stages (see xdec/decompile/emit.h).
template <class Fn>
auto timed(std::string_view stage, Fn&& fn) {
  const xdec::obs::Span span{"emit", std::string{stage}};
  return std::forward<Fn>(fn)();
}

/// The driver's half of the run report, shared by observe and decompile.
[[nodiscard]] xdec::json::Value driverSection(const xdec::decompile::DriverReport& report,
                                              const xdec::il::Function& function) {
  return xdec::json::Value::object({
      {"stop", std::string{xdec::decompile::toString(report.stop)}},
      {"wind_down_reason", report.windDownReason},
      {"converged", report.converged},
      {"rounds", static_cast<uint64_t>(report.rounds)},
      {"settles", static_cast<uint64_t>(report.settles)},
      {"extra_entries", static_cast<uint64_t>(report.extraEntries.size())},
      {"blocks", static_cast<uint64_t>(function.blockCount())},
      {"ops", static_cast<uint64_t>(function.opCount())},
      {"exprs", static_cast<uint64_t>(function.exprCount())},
      {"unreachable_blocks", static_cast<uint64_t>(report.unreachableBlocks)},
      {"trace_covered", report.traceCovered},
      {"trace_instruction_addresses",
       static_cast<uint64_t>(report.traceInstructionCount)},
  });
}

/// "partial" for every way the driver can stop short of a fixpoint.
void recordDriverOutcome(const xdec::decompile::DriverReport& report) {
  RunContext* run = RunContext::current();
  if (run == nullptr) {
    return;
  }
  using xdec::decompile::DriverStop;
  if (report.traceCovered || report.stop == DriverStop::RoundCap ||
      report.stop == DriverStop::WindDown ||
      report.stop == DriverStop::StuckResolution) {
    run->setOutcome("partial");
  }
}

[[nodiscard]] std::string driverSummary(const xdec::decompile::DriverReport& report,
                                        std::size_t blocks) {
  using xdec::decompile::DriverStop;
  std::string why;
  switch (report.stop) {
    case DriverStop::RoundCap:
      why = " (round cap reached; coverage may be partial)";
      break;
    case DriverStop::WindDown:
      why = std::format(" (wound down on {}; coverage may be partial)", report.windDownReason);
      break;
    case DriverStop::StuckResolution:
      why = " (a resolved target never became a block; coverage may be partial)";
      break;
    case DriverStop::Converged:
    case DriverStop::NoDiscovery:
      break;
  }
  return std::format("driver: {} round(s), {} settle(s), {} extra entrie(s), {} block(s) total{}",
                     report.rounds, report.settles, report.extraEntries.size(), blocks, why);
}

/// Lifts a whole function by recursive descent and runs the pass pipeline
/// under the dump observer, so every pass's effect lands in a file pair
/// (NN-pass.il / .map) plus an index, instead of in a scrollback buffer.
///
/// With no real optimisation passes built in yet, the pipeline is whatever
/// --plugin brings; the command is the harness P7's passes plug into.
int commandObserve(std::string_view path, uint64_t address, std::span<const std::string_view> options) {
  xdec::il::Maturity target = xdec::il::Maturity::Lifted;
  std::filesystem::path outDir =
      std::filesystem::path{std::format("observe-{:x}", address)};
  std::vector<std::string> pluginPaths;
  RoundCap roundCap;

  for (std::size_t i = 0; i < options.size(); ++i) {
    const std::string_view option = options[i];
    const auto value = [&]() -> std::string_view {
      if (++i >= options.size()) {
        return {};
      }
      return options[i];
    };
    if (option == "--to") {
      const std::string_view text = value();
      if (!xdec::il::parseMaturity(text, target)) {
        printError("'{}' is not a maturity level", text);
        return 1;
      }
    } else if (option == "--out") {
      outDir = std::filesystem::path{std::string{value()}};
    } else if (option == "--rounds") {
      if (!parseRoundCap(value(), roundCap)) {
        printError("'{}' is not a usable round cap", value());
        return 1;
      }
    } else if (option == "--plugin") {
      pluginPaths.emplace_back(value());
    } else {
      printError("unknown observe option '{}'", option);
      return 1;
    }
  }

  auto session = ToolSession::openBinary(path);
  if (!session) {
    return reportError(session.error());
  }
  const BinaryImage& image = *session->image;
  const xdec::spec::SpecEngine& engine = *session->engine;

  const xdec::spec::ByteReader reader =
      [&image](uint64_t va, std::span<std::byte> out) { return image.read(va, out); };

  // Declaration order is load order reversed: the Plugin must outlive the
  // Registry that owns its passes, so it unloads only after they are destroyed
  // (see plugin/abi.h). Plugins are therefore declared first.
  std::vector<xdec::plugin::Plugin> plugins;
  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);
  for (const std::string& pluginPath : pluginPaths) {
    auto plugin = xdec::plugin::Plugin::load(pluginPath, registry);
    if (!plugin) {
      return reportError(plugin.error());
    }
    note("plugin: {} ({} pass(es) so far)", pluginPath, registry.size());
    plugins.push_back(std::move(*plugin));
  }

  xdec::pass::DumpObserver observer(outDir);
  xdec::decompile::DriverOptions driverOptions;
  driverOptions.target = target;
  driverOptions.observer = &observer;
  driverOptions.maxRounds = roundCap.value;
  driverOptions.extendWhileProving = !roundCap.pinned;
  driverOptions.memory = memoryFactsOf(image);
  if (const xdec::binary::Symbol* symbol = image.symbolAt(address);
      symbol != nullptr && symbol->size != 0) {
    driverOptions.fence = {address, address + symbol->size};
  }
  auto result = xdec::decompile::decompile(engine, reader, address, registry, driverOptions);
  if (!result) {
    return reportError(result.error());
  }
  if (observer.failure()) {
    return reportError(*observer.failure());
  }
  const xdec::il::Function& function = *result->function;
  note("{}", driverSummary(result->report, function.blockCount()));
  recordDriverOutcome(result->report);
  if (RunContext* run = RunContext::current(); run != nullptr) {
    run->setSection("observe",
                    xdec::json::Value::object({{"address", std::format("{:#x}", address)},
                                               {"input", std::string{path}},
                                               {"out_dir", outDir.string()},
                                               {"driver", driverSection(result->report, function)}}));
  }
  note("profile: {}", xdec::analysis::profile(function).format());
  note("observe dumps in '{}'", outDir.string());
  return 0;
}

/// Whether an address's first instruction disassembles like the start of an
/// AArch64 function: a stack frame going up (`sub sp`, a `stp`/`str` saving
/// callee-saved registers) or one of the landing-pad no-ops (BTI/PAC, or this
/// project's own obfuscator's `mov x17, x17` / `mov x16, x16`) that precede
/// one. Best-effort and deliberately permissive -- it exists to catch the
/// class of mistake decompiling `sub_627ac` in bc_lib made (an address pulled
/// from the middle of another function's jump table, decompiled as if it were
/// its own entry, which then discovers that whole table as if it were free
/// code -- see samples/manifest.json's sample_afRDLog comment for the real
/// entry), not to reject every real prologue this short list does not cover.
[[nodiscard]] bool looksLikePrologue(const xdec::spec::SpecEngine& engine,
                                     const BinaryImage& image, uint64_t address) {
  const unsigned width = engine.program().insnWidth / 8;
  std::vector<std::byte> buffer(width);
  if (!image.read(address, buffer)) {
    return true;  // unreadable is a different problem, not this check's to report
  }
  const auto insn = engine.decode(buffer, address);
  if (!insn.valid) {
    return true;
  }
  const std::string text = engine.disassemble(insn);
  static constexpr std::string_view kPrologueShapes[] = {
      "sub sp, sp", "[sp, #-",      "stp x19", "stp x20", "stp x21",
      "stp x22",    "stp x23",      "stp x24", "stp x25", "stp x26",
      "stp x27",    "stp x28",      "stp x29", "stp d",   "str d",
      "stp q",      "str x30",      "mov x17, x17", "mov x16, x16",
      "paciasp",    "pacibsp",      "bti ",
  };
  return std::ranges::any_of(
      kPrologueShapes, [&](std::string_view shape) { return text.find(shape) != std::string::npos; });
}

/// The lowest symbol address strictly above `address`, when the image has
/// one. Whatever the function at `address` is, it stops before the next thing
/// with a name -- which is a weaker statement than a symbol's recorded size,
/// and the only one available for an entry that has no symbol of its own.
[[nodiscard]] std::optional<uint64_t> nextSymbolAfter(const BinaryImage& image,
                                                      uint64_t address) {
  std::optional<uint64_t> best;
  for (const xdec::binary::Symbol& symbol : image.symbols()) {
    if (!symbol.defined || symbol.va <= address) {
      continue;
    }
    if (!best.has_value() || symbol.va < *best) {
      best = symbol.va;
    }
  }
  return best;
}

/// The full pipeline: discover and lift, resolve, recover variables,
/// structure, emit C. Supports single address, multiple addresses, `-s <sym>`,
/// and `--match <pattern> [--out-dir <dir>] [-j <threads>]` batch mode.
int commandDecompile(std::string_view path, std::span<const std::string_view> options) {
  std::filesystem::path outPath;
  std::filesystem::path outDir;
  std::filesystem::path enginePath;
  RoundCap roundCap;
  bool annotate = true;
  bool allowUnresolved = false;
  bool reuseReport = false;
  bool emitReport = false;
  bool dumpIl = false;
  std::vector<std::string> typeSources;
  std::vector<std::string> pluginPaths;
  std::vector<uint64_t> targetAddresses;
  std::vector<std::string> symbolQueries;
  std::string matchPattern;
  unsigned batchThreads = 0;
  std::string syscallSource{xdec::types::SyscallTable::defaultName()};
  std::string helpersHeader{"xdec_helpers.h"};
  bool indexedArgumentNames = true;
  bool securityHintsAsComments = true;
  bool verboseNotes = false;
  bool objcSyntax = true;
  bool defaultPlugins = true;
  xdec::emit::EmitLanguage language = xdec::emit::EmitLanguage::C;
  uint64_t maxSpan = 0;
  uint64_t discoveryCap = 0;
  xdec::analysis::PathExploreOptions pathExplore;
  for (std::size_t i = 0; i < options.size(); ++i) {
    const std::string_view option = options[i];
    const auto value = [&]() -> std::string_view {
      if (++i >= options.size()) {
        return {};
      }
      return options[i];
    };
    if (!option.starts_with("-")) {
      uint64_t parsedAddr = 0;
      if (!parseNumber(option, parsedAddr)) {
        printError("'{}' is not a valid address or option", option);
        return 1;
      }
      targetAddresses.push_back(parsedAddr);
    } else if (option == "-s" || option == "--symbol") {
      symbolQueries.emplace_back(value());
    } else if (option == "--match") {
      matchPattern = std::string{value()};
    } else if (option == "--out-dir") {
      outDir = std::filesystem::path{std::string{value()}};
    } else if (option == "-j" || option == "--threads") {
      uint64_t n = 0;
      if (!parseNumber(value(), n) || n == 0 || n > 64) {
        printError("'-j' requires a thread count in 1..64");
        return 1;
      }
      batchThreads = static_cast<unsigned>(n);
    } else if (option == "-o" || option == "--out") {
      outPath = std::filesystem::path{std::string{value()}};
    } else if (option == "--engine") {
      enginePath = std::filesystem::path{std::string{value()}};
    } else if (option == "--rounds") {
      if (!parseRoundCap(value(), roundCap)) {
        printError("'{}' is not a usable round cap", value());
        return 1;
      }
    } else if (option == "--no-annotate") {
      annotate = false;
    } else if (option == "--allow-unresolved") {
      allowUnresolved = true;
    } else if (option == "--reuse-report") {
      reuseReport = true;
    } else if (option == "--emit-report") {
      emitReport = true;
    } else if (option == "--dump-il") {
      dumpIl = true;
    } else if (option == "--max-span") {
      const std::string_view text = value();
      if (!parseNumber(text, maxSpan) || maxSpan == 0) {
        printError("'--max-span' takes a byte count above zero, not '{}'", text);
        return 1;
      }
    } else if (option == "--discovery-cap") {
      const std::string_view text = value();
      if (!parseNumber(text, discoveryCap) || discoveryCap == 0) {
        printError("'--discovery-cap' takes a target count above zero, not '{}'", text);
        return 1;
      }
    } else if (option == "--no-path-explore") {
      pathExplore.enabled = false;
    } else if (option == "--path-explore-max-paths") {
      const std::string_view text = value();
      uint64_t maxPaths = 0;
      if (!parseNumber(text, maxPaths) || maxPaths == 0) {
        printError("'--path-explore-max-paths' takes a count above zero, not '{}'", text);
        return 1;
      }
      pathExplore.maxPaths = static_cast<unsigned>(maxPaths);
    } else if (option == "--path-explore-canary-fail") {
      pathExplore.exploreCanaryFailPaths = true;
    } else if (option == "--types") {
      typeSources.emplace_back(value());
    } else if (option == "--plugin") {
      pluginPaths.emplace_back(value());
    } else if (option == "--syscall-table") {
      const std::string_view text = value();
      syscallSource = text == "none" ? std::string{} : std::string{text};
    } else if (option == "--helpers-header") {
      const std::string_view text = value();
      helpersHeader = text == "none" ? std::string{} : std::string{text};
    } else if (option == "--arg-naming") {
      const std::string_view text = value();
      if (text == "indexed") {
        indexedArgumentNames = true;
      } else if (text == "reg") {
        indexedArgumentNames = false;
      } else {
        printError("'--arg-naming' takes 'indexed' or 'reg', not '{}'", text);
        return 1;
      }
    } else if (option == "--security-hints") {
      const std::string_view text = value();
      if (text == "comment") {
        securityHintsAsComments = true;
      } else if (text == "keep") {
        securityHintsAsComments = false;
      } else {
        printError("'--security-hints' takes 'comment' or 'keep', not '{}'", text);
        return 1;
      }
    } else if (option == "--verbose-notes") {
      verboseNotes = true;
    } else if (option == "--no-objc-syntax") {
      objcSyntax = false;
    } else if (option == "--no-default-plugins") {
      defaultPlugins = false;
    } else if (option == "--lang") {
      const std::string_view text = value();
      if (text == "c") {
        language = xdec::emit::EmitLanguage::C;
      } else if (text == "objc" || text == "oc") {
        language = xdec::emit::EmitLanguage::Objc;
      } else {
        printError("'--lang' takes 'c' or 'objc', not '{}'", text);
        return 1;
      }
    } else {
      printError("unknown decompile option '{}'", option);
      return 1;
    }
  }

  if (targetAddresses.empty() && symbolQueries.empty() && matchPattern.empty()) {
    printError("decompile needs at least one address, -s <symbol>, or --match <pattern>");
    return 1;
  }

  SessionLoadOptions loadOptions;
  loadOptions.typeSources = typeSources;
  loadOptions.syscallSource = syscallSource;
  auto session = SessionContext::open(path, loadOptions);
  if (!session) {
    return reportError(session.error());
  }
  const BinaryImage& image = *session->image;
  const xdec::spec::SpecEngine& engine = *session->engine;
  const xdec::spec::ByteReader reader = session->reader();
  const auto* machoMeta =
      dynamic_cast<const xdec::binary::MachOMetadata*>(image.formatMetadata());

  const auto containsCI = [](std::string_view hay, std::string_view needle) {
    return matchesQueryCI(hay, needle);
  };
  if (machoMeta != nullptr) {
    XDEC_SPAN(idxSpan, "index", "objc");
    (void)machoMeta->objcMethodAt(image, targetAddresses.empty() ? 0 : targetAddresses[0]);
  }

  std::unordered_map<uint64_t, std::string> resolvedNames;
  for (const std::string& q : symbolQueries) {
    std::vector<std::pair<uint64_t, std::string>> exact;
    std::vector<std::pair<uint64_t, std::string>> partial;
    for (const auto& sym : image.symbols()) {
      if (!sym.defined || sym.kind != xdec::binary::SymbolKind::Function) {
        continue;
      }
      if (sym.name == q) {
        exact.emplace_back(sym.va, sym.name);
      } else if (containsCI(sym.name, q)) {
        partial.emplace_back(sym.va, sym.name);
      }
    }
    if (machoMeta != nullptr) {
      for (const auto& [impVa, info] : machoMeta->allObjcMethods(image)) {
        if (info.displayName == q || info.cName == q) {
          exact.emplace_back(impVa, info.displayName);
        } else if (containsCI(info.displayName, q) || containsCI(info.cName, q)) {
          partial.emplace_back(impVa, info.displayName);
        }
      }
    }
    if (exact.size() == 1) {
      targetAddresses.push_back(exact[0].first);
      resolvedNames[exact[0].first] = exact[0].second;
    } else if (!exact.empty()) {
      for (const auto& e : exact) {
        targetAddresses.push_back(e.first);
        resolvedNames[e.first] = e.second;
      }
    } else if (partial.size() == 1) {
      targetAddresses.push_back(partial[0].first);
      resolvedNames[partial[0].first] = partial[0].second;
    } else if (partial.empty()) {
      printError("no function matching symbol '{}'", q);
      return 1;
    } else {
      std::sort(partial.begin(), partial.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
      printError("symbol '{}' is ambiguous ({} matches); specify an address or use --match:",
                 q, partial.size());
      for (std::size_t idx = 0; idx < std::min<std::size_t>(partial.size(), 15); ++idx) {
        printError("  0x{:x}  {}", partial[idx].first, partial[idx].second);
      }
      return 1;
    }
  }

  if (!matchPattern.empty()) {
    std::set<uint64_t> matchedSet;
    for (const auto& sym : image.symbols()) {
      if (sym.defined && sym.kind == xdec::binary::SymbolKind::Function &&
          containsCI(sym.name, matchPattern)) {
        if (matchedSet.insert(sym.va).second) {
          targetAddresses.push_back(sym.va);
          resolvedNames[sym.va] = sym.name;
        }
      }
    }
    if (machoMeta != nullptr) {
      for (const auto& [impVa, info] : machoMeta->allObjcMethods(image)) {
        if (containsCI(info.displayName, matchPattern) ||
            containsCI(info.cName, matchPattern)) {
          if (matchedSet.insert(impVa).second) {
            targetAddresses.push_back(impVa);
            resolvedNames[impVa] = info.displayName;
          }
        }
      }
    }
    if (targetAddresses.empty()) {
      printError("no functions match '{}'", matchPattern);
      return 1;
    }
    note("matched {} function(s) for '{}'", targetAddresses.size(), matchPattern);
  }

  // Plugins must outlive `registry` and `toCOptions.structure.astTransforms`
  // (see plugin/abi.h).
  if (defaultPlugins && image.format() == xdec::binary::BinaryFormat::MachO) {
    const bool already = std::any_of(pluginPaths.begin(), pluginPaths.end(), [](const std::string& p) {
      return p.find("objc_libcxx") != std::string::npos;
    });
    if (!already) {
      if (const std::string p = defaultObjcPluginPath(); !p.empty()) {
        pluginPaths.push_back(p);
      }
    }
  }
  std::vector<xdec::plugin::Plugin> plugins;
  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);
  std::vector<xdec::emit::AstTransformFn> pluginAstTransforms;
  std::vector<xdec::emit::HlAstTransformFn> pluginHlTransforms;
  for (const std::string& pluginPath : pluginPaths) {
    auto plugin = xdec::plugin::Plugin::load(pluginPath, registry, &pluginAstTransforms,
                                             &pluginHlTransforms);
    if (!plugin) {
      return reportError(plugin.error());
    }
    note("plugin: {} ({} pass(es), {} ast transform(s))", pluginPath,
         plugin->registeredPasses(), plugin->registeredAstTransforms());
    plugins.push_back(std::move(*plugin));
  }

  const auto configureForAddress = [&](uint64_t address,
                                       xdec::decompile::DecompileToCOptions& toCOptions,
                                       std::optional<xdec::analysis::ExecutionCoverage>& cov) {
    xdec::decompile::DriverOptions driverOptions = session->driverOptions();
    driverOptions.target = xdec::il::Maturity::Vars;
    driverOptions.maxRounds = roundCap.value;
    driverOptions.extendWhileProving = !roundCap.pinned;
    driverOptions.sealUnresolvedBranches = allowUnresolved;
    driverOptions.maxDiscoveryPerBranch = static_cast<std::size_t>(discoveryCap);
    driverOptions.pathExplore = pathExplore;
    if (!enginePath.empty()) {
      auto database = xdec::storage::Tracedb::open(enginePath);
      if (!database) {
        return database.error();
      }
      xdec::analysis::CoverageSelection selection;
      selection.functionTarget = address;
      selection.imageBase = image.memory().lowestAddress();
      auto loaded = xdec::analysis::ExecutionCoverage::load(*database, selection);
      if (!loaded) {
        return loaded.error();
      }
      if (loaded->invocations().empty()) {
        return xdec::Diag{xdec::DiagCode::BadFormat,
                          std::format("tracedb contains no invocation of {:#x}", address)};
      }
      cov.emplace(std::move(*loaded));
      driverOptions.executionCoverage = &*cov;
      driverOptions.pathExplore.enabled = false;
    }
    const xdec::binary::Symbol* entrySymbol = image.symbolAt(address);
    const std::optional<std::pair<uint64_t, uint64_t>> unwindBounds =
        machoMeta != nullptr ? machoMeta->functionBoundsAt(image, address) : std::nullopt;
    const bool knownUnwindEntry = unwindBounds.has_value() && unwindBounds->first == address;
    const bool sized = entrySymbol != nullptr && entrySymbol->size != 0;
    if (enginePath.empty() && maxSpan != 0) {
      driverOptions.fence = {address, address + maxSpan, /*enforce=*/true};
    } else if (enginePath.empty() && sized) {
      driverOptions.fence = {address, address + entrySymbol->size};
    } else if (enginePath.empty()) {
      std::optional<uint64_t> upper = nextSymbolAfter(image, address);
      if (knownUnwindEntry) {
        upper = upper.has_value() ? std::min(*upper, unwindBounds->second)
                                  : unwindBounds->second;
      } else if (machoMeta != nullptr) {
        if (const std::optional<uint64_t> nextUnwind =
                machoMeta->nextUnwindFunctionAfter(image, address)) {
          upper = upper.has_value() ? std::min(*upper, *nextUnwind) : *nextUnwind;
        }
      }
      if (upper.has_value()) {
        driverOptions.fence = {address, *upper};
      }
    }
    if (enginePath.empty() && maxSpan == 0 && entrySymbol == nullptr && !knownUnwindEntry &&
        !looksLikePrologue(engine, image, address)) {
      warn("{:#x} has no symbol and its first instruction does not look like a "
           "function prologue; if this address is inside another function's body (a "
           "jump-table target, say) rather than a real entry, discovery has no function "
           "size to stay inside and this run may pull in unrelated code",
           address);
    }
    if (enginePath.empty()) {
      driverOptions.isTailCall = [&image, &engine, machoMeta, &reader,
                                  entryAddr = address, unwindBounds, knownUnwindEntry](
                                     uint64_t /*branchVa*/, uint64_t target) -> bool {
        if (target == entryAddr) {
          return false;
        }
        if (xdec::analysis::pltGotSlot(reader, target).has_value()) {
          return true;
        }
        uint64_t dest = target;
        for (unsigned hop = 0; hop < 4; ++hop) {
          std::array<std::byte, 4> bytes{};
          if (!reader(dest, std::span<std::byte>{bytes})) {
            break;
          }
          const uint32_t w =
              static_cast<uint32_t>(static_cast<uint8_t>(bytes[0])) |
              (static_cast<uint32_t>(static_cast<uint8_t>(bytes[1])) << 8) |
              (static_cast<uint32_t>(static_cast<uint8_t>(bytes[2])) << 16) |
              (static_cast<uint32_t>(static_cast<uint8_t>(bytes[3])) << 24);
          if ((w & 0xfc000000u) != 0x14000000u) {
            break;
          }
          const uint32_t imm26 = w & 0x03ffffffu;
          const int32_t sext = static_cast<int32_t>(imm26 << 6) >> 6;
          dest = dest + static_cast<uint64_t>(static_cast<int64_t>(sext) << 2);
        }
        if (dest == entryAddr) {
          return false;
        }
        if (const auto* sym = image.symbolAt(dest);
            sym != nullptr && sym->kind == xdec::binary::SymbolKind::Function) {
          return true;
        }
        if (machoMeta != nullptr) {
          if (machoMeta->objcMethodAt(image, dest).has_value()) {
            return true;
          }
          if (const auto b = machoMeta->functionBoundsAt(image, dest);
              b.has_value() && b->first == dest) {
            return true;
          }
        }
        if (knownUnwindEntry &&
            (dest < unwindBounds->first || dest >= unwindBounds->second) &&
            looksLikePrologue(engine, image, dest)) {
          return true;
        }
        return false;
      };
    }
    toCOptions.driver = driverOptions;
    toCOptions.profile = &session->profile;
    toCOptions.structure.astTransforms = pluginAstTransforms;
    toCOptions.emit.annotateBlocks = annotate;
    toCOptions.emit.symbols = session->symbols();
    toCOptions.emit.addresses = session->addresses();
    toCOptions.emit.imageReader = session->reader();
    toCOptions.emit.entryRegs =
        session->entryRegFacts().empty() ? nullptr : &session->entryRegFacts();
    toCOptions.emit.helpersHeader = helpersHeader;
    toCOptions.emit.indexedArgumentNames = indexedArgumentNames;
    toCOptions.emit.securityHintsAsComments = securityHintsAsComments;
    toCOptions.emit.verboseNotes = verboseNotes;
    toCOptions.emit.objcSyntax = objcSyntax;
    toCOptions.emit.language = language;
    toCOptions.emit.hlTransforms = pluginHlTransforms;
    toCOptions.computeEmitRedundancy = emitReport;
    toCOptions.computeObfuscationProfile = emitReport;
    return xdec::Diag{xdec::DiagCode::Ok, ""};
  };

  if (targetAddresses.size() == 1 && outDir.empty()) {
    const uint64_t address = targetAddresses[0];
    std::optional<xdec::analysis::ExecutionCoverage> executionCoverage;
    xdec::decompile::DecompileToCOptions toCOptions;
    const xdec::Diag cfgDiag = configureForAddress(address, toCOptions, executionCoverage);
    if (cfgDiag.code() != xdec::DiagCode::Ok) {
      return reportError(cfgDiag);
    }
    auto result = xdec::decompile::decompileToC(engine, reader, address, registry, toCOptions);
    if (!result) {
      return reportError(result.error());
    }
    const xdec::il::Function& function = *result->function;
    note("{}", driverSummary(result->driverReport, function.blockCount()));
    recordDriverOutcome(result->driverReport);
    if (reuseReport) {
      const xdec::analysis::ExpressionReuseReport reuse =
          timed("reuse-report", [&] { return xdec::analysis::analyzeExpressionReuse(function); });
      print("reuse: {} exact duplicate(s), {} structural duplicate(s)",
            reuse.count(xdec::analysis::ReuseKind::ExactDuplicate),
            reuse.count(xdec::analysis::ReuseKind::StructuralDuplicate));
      if (!reuse.findings.empty()) {
        print("{}", reuse.format(function));
      }
    }
    if (dumpIl) {
      print("{}", xdec::il::print(function));
    }
    if (emitReport && result->report.emitRedundancy) {
      print("emit-report: {}", result->report.emitRedundancy->format());
    }
    if (emitReport && result->report.obfuscationProfile) {
      print("profile: {}", result->report.obfuscationProfile->format());
    }
    if (emitReport) {
      const auto regions = xdec::analysis::findDispatchRegions(function);
      std::size_t totalSites = 0;
      for (const auto& region : regions) {
        totalSites += region.sites.size();
      }
      print("dispatch-regions: {} region(s), {} site(s) total", regions.size(), totalSites);
      for (std::size_t i = 0; i < regions.size(); ++i) {
        const auto& region = regions[i];
        print("  region[{}]: table=0x{:x} stride={} entryBits={} clamp={} sites={} sharedTail={}",
              i, region.tableBase, region.tableStride, region.tableEntryBits,
              region.clampBound ? std::format("0x{:x}/0x{:x}", *region.clampBound,
                                              *region.clampReplacement)
                                : std::string("none"),
              region.sites.size(), region.sharedTail.has_value());
        const auto nest = xdec::analysis::buildDispatchNestGraph(function, region);
        print("    nest: roots={} depth={} nested={}", nest.roots.size(), nest.maxDepth,
              nest.nestedSiteCount);
      }
    }
    note("emit: {} argument(s), {} local(s), {} temp(s), {} labeled block(s)",
         result->variables.arguments().size(), result->variables.locals().size(),
         result->variables.temps().size(), result->structured.labeled.size());
    if (RunContext* run = RunContext::current(); run != nullptr) {
      std::vector<xdec::json::Value> stages;
      for (const auto& stage : result->report.stageTimings) {
        stages.push_back(xdec::json::Value::object(
            {{"stage", std::string{stage.stage}},
             {"ms", static_cast<int64_t>(stage.elapsed.count())}}));
      }
      std::error_code sizeError;
      const auto inputSize = std::filesystem::file_size(std::filesystem::path{path}, sizeError);
      run->setSection(
          "decompile",
          xdec::json::Value::object({
              {"address", std::format("{:#x}", address)},
              {"input", std::string{path}},
              {"input_bytes", static_cast<uint64_t>(sizeError ? 0 : inputSize)},
              {"output", outPath.string()},
              {"output_bytes", static_cast<uint64_t>(result->cSource.size())},
              {"driver", driverSection(result->driverReport, function)},
              {"emit", xdec::json::Value::object({
                           {"arguments", static_cast<uint64_t>(result->variables.arguments().size())},
                           {"locals", static_cast<uint64_t>(result->variables.locals().size())},
                           {"temps", static_cast<uint64_t>(result->variables.temps().size())},
                           {"labeled_blocks",
                            static_cast<uint64_t>(result->structured.labeled.size())},
                           {"stages", xdec::json::Value::array(std::move(stages))},
                       })},
          }));
    }
    if (outPath.empty()) {
      print("{}", result->cSource);
    } else {
      std::ofstream stream(outPath, std::ios::binary | std::ios::trunc);
      if (!stream) {
        printError("cannot open '{}' for writing", outPath.string());
        return 1;
      }
      stream << result->cSource;
      note("wrote '{}' ({} bytes)", outPath.string(), result->cSource.size());
    }
    return 0;
  }

  // Multi-address / batch parallel decompile mode.
  if (!outDir.empty()) {
    std::error_code mkErr;
    std::filesystem::create_directories(outDir, mkErr);
    if (mkErr) {
      printError("cannot create output directory '{}': {}", outDir.string(), mkErr.message());
      return 1;
    }
  }
  const auto sanitizeFilename = [](std::string_view name) {
    std::string out;
    out.reserve(std::min<std::size_t>(name.size(), 64));
    for (std::size_t i = 0; i < name.size() && out.size() < 64; ++i) {
      const char ch = name[i];
      const auto uc = static_cast<unsigned char>(ch);
      out.push_back((std::isalnum(uc) != 0 || ch == '_' || ch == '-') ? ch : '_');
    }
    return out.empty() ? std::string{"fn"} : out;
  };

  unsigned workers = batchThreads;
  if (workers == 0) {
    const unsigned hw = std::thread::hardware_concurrency();
    workers = std::min<unsigned>(
        hw == 0 ? 4u : std::min(hw, 8u),
        static_cast<unsigned>(std::max<std::size_t>(1, targetAddresses.size())));
  }
  workers = std::max(1u, workers);

  std::atomic<std::size_t> nextIndex{0};
  std::atomic<std::size_t> okCount{0};
  std::atomic<std::size_t> failCount{0};
  std::mutex ioMutex;

  const auto runBatchWorker = [&]() {
    for (;;) {
      const std::size_t idx = nextIndex.fetch_add(1, std::memory_order_relaxed);
      if (idx >= targetAddresses.size()) {
        break;
      }
      const uint64_t address = targetAddresses[idx];
      std::optional<xdec::analysis::ExecutionCoverage> cov;
      xdec::decompile::DecompileToCOptions toCOptions;
      const xdec::Diag cfgDiag = configureForAddress(address, toCOptions, cov);
      if (cfgDiag.code() != xdec::DiagCode::Ok) {
        std::lock_guard<std::mutex> lock(ioMutex);
        printError("[{:#x}] {}", address, cfgDiag.format());
        failCount.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      auto result = xdec::decompile::decompileToC(engine, reader, address, registry, toCOptions);
      if (!result) {
        std::lock_guard<std::mutex> lock(ioMutex);
        printError("[{:#x}] {}", address, result.error().format());
        failCount.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      okCount.fetch_add(1, std::memory_order_relaxed);
      if (!outDir.empty()) {
        std::string label;
        if (const auto it = resolvedNames.find(address); it != resolvedNames.end()) {
          label = it->second;
        } else if (const auto* sym = image.symbolAt(address)) {
          label = sym->name;
        } else if (machoMeta != nullptr) {
          if (const auto m = machoMeta->objcMethodAt(image, address)) {
            label = m->displayName;
          }
        }
        const std::string fname =
            label.empty() ? std::format("sub_{:x}.c", address)
                          : std::format("0x{:x}_{}.c", address, sanitizeFilename(label));
        const std::filesystem::path fileOut = outDir / fname;
        std::ofstream stream(fileOut, std::ios::binary | std::ios::trunc);
        std::lock_guard<std::mutex> lock(ioMutex);
        if (!stream) {
          printError("cannot write '{}'", fileOut.string());
          failCount.fetch_add(1, std::memory_order_relaxed);
        } else {
          stream << result->cSource;
        }
      } else {
        std::lock_guard<std::mutex> lock(ioMutex);
        print("// === 0x{:x} ===", address);
        print("{}", result->cSource);
      }
    }
  };

  if (workers == 1) {
    runBatchWorker();
  } else {
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (unsigned t = 0; t < workers; ++t) {
      pool.emplace_back(runBatchWorker);
    }
    for (auto& th : pool) {
      th.join();
    }
  }
  note("decompile-batch: {} succeeded, {} failed (of {})", okCount.load(), failCount.load(),
       targetAddresses.size());
  return failCount.load() == 0 ? 0 : 1;
}

}  // namespace xdec::cli
