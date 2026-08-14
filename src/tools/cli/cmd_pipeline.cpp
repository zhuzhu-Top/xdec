// observe, decompile: the two commands that run the pass pipeline. The
// heavyweight file of the set -- decompile alone exercises analysis, emit and
// the decompile driver in one function.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common.h"
#include "options.h"
#include "session.h"
#include "xdec/analysis/dispatch_region.h"
#include "xdec/analysis/emit_redundancy.h"
#include "xdec/analysis/expr_reuse.h"
#include "xdec/analysis/profile.h"
#include "xdec/decompile/driver.h"
#include "xdec/decompile/emit.h"
#include "xdec/il/printer.h"
#include "xdec/pass/manager.h"
#include "xdec/pass/observe.h"
#include "xdec/pass/registry.h"
#include "xdec/passes/builtin.h"
#include "xdec/plugin/loader.h"
#include "xdec/spec/lift.h"
#include "xdec/support/log.h"
#include "xdec/types/syscall_table.h"

namespace xdec::cli {

/// Times a CLI-only diagnostic (`--reuse-report`/`--emit-report`) under the
/// same "emit" category decompileToC's own internal stages log under (see
/// xdec/decompile/emit.h) -- one category for the whole analyse-and-emit
/// tail, whichever side of the library boundary a given stage now lives on.
template <class Fn>
auto timed(std::string_view stage, Fn&& fn) {
  const auto started = std::chrono::steady_clock::now();
  auto out = std::forward<Fn>(fn)();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  XDEC_LOG_DEBUG(xdec::decompile::emitLog(), "{:>16} {:>6}ms", stage, elapsed.count());
  return out;
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
        print("error: '{}' is not a maturity level", text);
        return 1;
      }
    } else if (option == "--out") {
      outDir = std::filesystem::path{std::string{value()}};
    } else if (option == "--rounds") {
      if (!parseRoundCap(value(), roundCap)) {
        print("error: '{}' is not a usable round cap", value());
        return 1;
      }
    } else if (option == "--plugin") {
      pluginPaths.emplace_back(value());
    } else {
      print("error: unknown observe option '{}'", option);
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
    print("plugin: {} ({} pass(es) so far)", pluginPath, registry.size());
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
  print("driver: {} round(s), {} extra entrie(s), {} block(s) total{}", result->report.rounds,
        result->report.extraEntries.size(), function.blockCount(),
        result->report.converged ? "" : " (round cap reached; coverage may be partial)");
  print("profile: {}", xdec::analysis::profile(function).format());
  print("observe dumps in '{}'", outDir.string());
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
      "sub sp, sp", "stp x29",      "stp x28",      "str d",   "stp d",
      "mov x17, x17", "mov x16, x16", "paciasp",      "pacibsp", "bti ",
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
/// structure, emit C. This is the deliverable every other command builds
/// towards.
int commandDecompile(std::string_view path, uint64_t address,
                     std::span<const std::string_view> options) {
  std::filesystem::path outPath;
  RoundCap roundCap;
  bool annotate = true;
  bool allowUnresolved = false;
  bool reuseReport = false;
  bool emitReport = false;
  bool dumpIl = false;
  std::vector<std::string> typeSources;
  // On by default, because a syscall's number means the same thing in every
  // AArch64 Linux binary and leaving it unnamed helps nobody. `--syscall-table
  // none` is there for the case the default is wrong -- a different kernel ABI
  // -- where a plausible name would be worse than a number.
  std::string syscallSource{xdec::types::SyscallTable::defaultName()};
  // What the preamble `#include`s for rotate/bswap/popcount/cc_* (see
  // xdec_helpers.h). `none` suppresses the include for a caller who wants
  // those names some other way -- inlined, or from a different path.
  std::string helpersHeader{"xdec_helpers.h"};
  // See COptions::indexedArgumentNames/securityHintsAsComments: both default
  // to the more readable spelling, with a flag to opt back out to the raw
  // form for a caller that wants every function to look the same regardless
  // of what a header happened to name.
  bool indexedArgumentNames = true;
  bool securityHintsAsComments = true;
  // How many bytes past the entry discovery may reach, when the caller wants
  // that bounded rather than left to the entry's symbol size (see
  // FunctionFence::enforce). Zero means "no such request", which is the
  // default and leaves the fence advisory as before.
  uint64_t maxSpan = 0;
  // How many unlifted targets one branch may contribute per round (see
  // DriverOptions::maxDiscoveryPerBranch). Zero means no limit.
  uint64_t discoveryCap = 0;
  for (std::size_t i = 0; i < options.size(); ++i) {
    const std::string_view option = options[i];
    const auto value = [&]() -> std::string_view {
      if (++i >= options.size()) {
        return {};
      }
      return options[i];
    };
    if (option == "-o" || option == "--out") {
      outPath = std::filesystem::path{std::string{value()}};
    } else if (option == "--rounds") {
      if (!parseRoundCap(value(), roundCap)) {
        print("error: '{}' is not a usable round cap", value());
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
        print("error: '--max-span' takes a byte count above zero, not '{}'", text);
        return 1;
      }
    } else if (option == "--discovery-cap") {
      const std::string_view text = value();
      if (!parseNumber(text, discoveryCap) || discoveryCap == 0) {
        print("error: '--discovery-cap' takes a target count above zero, not '{}'", text);
        return 1;
      }
    } else if (option == "--types") {
      typeSources.emplace_back(value());
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
        print("error: '--arg-naming' takes 'indexed' or 'reg', not '{}'", text);
        return 1;
      }
    } else if (option == "--security-hints") {
      const std::string_view text = value();
      if (text == "comment") {
        securityHintsAsComments = true;
      } else if (text == "keep") {
        securityHintsAsComments = false;
      } else {
        print("error: '--security-hints' takes 'comment' or 'keep', not '{}'", text);
        return 1;
      }
    } else {
      print("error: unknown decompile option '{}'", option);
      return 1;
    }
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

  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);
  // Session-wide facts (memory/types/syscalls/names) come from `session`;
  // only what varies per call is set here.
  xdec::decompile::DriverOptions driverOptions = session->driverOptions();
  // Vars, not Resolved: emission reads the recovered call arity off the IL, and
  // stopping a level short would leave it guessing (see passes/vars.h).
  driverOptions.target = xdec::il::Maturity::Vars;
  driverOptions.maxRounds = roundCap.value;
  driverOptions.extendWhileProving = !roundCap.pinned;
  driverOptions.sealUnresolvedBranches = allowUnresolved;
  driverOptions.maxDiscoveryPerBranch = static_cast<std::size_t>(discoveryCap);
  const xdec::binary::Symbol* entrySymbol = image.symbolAt(address);
  const bool sized = entrySymbol != nullptr && entrySymbol->size != 0;
  if (maxSpan != 0) {
    // An explicit bound outranks whatever the symbol table says, and is the
    // only thing that makes a fence binding: the caller is describing the work
    // it wants done, not the function's real extent.
    driverOptions.fence = {address, address + maxSpan, /*enforce=*/true};
  } else if (sized) {
    driverOptions.fence = {address, address + entrySymbol->size};
  } else if (const std::optional<uint64_t> next = nextSymbolAfter(image, address)) {
    // No sized symbol here, but the next one along is still a fact about the
    // image: whatever this function is, it stops before the next thing with a
    // name. Advisory like any other inferred fence -- it only feeds the
    // bleed-through diagnostics below -- but it is the difference between
    // those diagnostics having something to compare against and having
    // nothing, which is the state absd's LC_MAIN entry (which carries no
    // symbol of its own) was in.
    driverOptions.fence = {address, *next};
  }
  if (maxSpan == 0 && !sized && !looksLikePrologue(engine, image, address)) {
    // No symbol of its own, and the entry does not look like a function start
    // either: exactly the shape that let sub_627ac silently balloon into 1349
    // discovered addresses and 44k lines. Warn, rather than fail -- an unusual
    // but real entry (a hand-written trampoline, a prologue-less leaf) is
    // still worth decompiling, just not silently mistaken for one when it
    // might not be.
    print("warning: {:#x} has no symbol and its first instruction does not look like a "
          "function prologue; if this address is inside another function's body (a "
          "jump-table target, say) rather than a real entry, discovery has no function "
          "size to stay inside and this run may pull in unrelated code",
          address);
  }
  xdec::decompile::DecompileToCOptions toCOptions;
  toCOptions.driver = driverOptions;
  toCOptions.profile = &session->profile;
  toCOptions.emit.annotateBlocks = annotate;
  toCOptions.emit.symbols = session->symbols();
  toCOptions.emit.addresses = session->addresses();
  toCOptions.emit.imageReader = session->reader();
  toCOptions.emit.helpersHeader = helpersHeader;
  toCOptions.emit.indexedArgumentNames = indexedArgumentNames;
  toCOptions.emit.securityHintsAsComments = securityHintsAsComments;
  // `--emit-report` now asks decompileToC() itself for the scan (see
  // DecompileToCOptions::computeEmitRedundancy) instead of the CLI repeating
  // it over the result -- the same report, the same one function-sized scan.
  toCOptions.computeEmitRedundancy = emitReport;
  toCOptions.computeObfuscationProfile = emitReport;
  auto result = xdec::decompile::decompileToC(engine, reader, address, registry, toCOptions);
  if (!result) {
    return reportError(result.error());
  }
  const xdec::il::Function& function = *result->function;
  print("driver: {} round(s), {} extra entrie(s), {} block(s) total{}", result->driverReport.rounds,
        result->driverReport.extraEntries.size(), function.blockCount(),
        result->driverReport.converged ? "" : " (round cap reached; coverage may be partial)");
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
      print("  region[{}]: table=0x{:x} stride={} entryBits={} clamp={} sites={} sharedTail={}", i,
            region.tableBase, region.tableStride, region.tableEntryBits,
            region.clampBound ? std::format("0x{:x}/0x{:x}", *region.clampBound, *region.clampReplacement)
                              : std::string("none"),
            region.sites.size(), region.sharedTail.has_value());
      // docs/19-scatter-dispatch-target-shape.md: the region's own decision-
      // forest shape -- how many independent roots a reader meets walking
      // the function, how deep the longest chained-2-way arm goes, and how
      // many sites are only reached that way rather than from the rest of
      // the function's own ordinary control flow.
      const auto nest = xdec::analysis::buildDispatchNestGraph(function, region);
      print("    nest: roots={} depth={} nested={}", nest.roots.size(), nest.maxDepth,
            nest.nestedSiteCount);
    }
  }

  print("emit: {} argument(s), {} local(s), {} temp(s), {} labeled block(s)",
        result->variables.arguments().size(), result->variables.locals().size(),
        result->variables.temps().size(), result->structured.labeled.size());
  if (outPath.empty()) {
    print("{}", result->cSource);
  } else {
    std::ofstream stream(outPath, std::ios::binary | std::ios::trunc);
    if (!stream) {
      print("error: cannot open '{}' for writing", outPath.string());
      return 1;
    }
    stream << result->cSource;
    print("wrote '{}' ({} bytes)", outPath.string(), result->cSource.size());
  }
  return 0;
}

}  // namespace xdec::cli
