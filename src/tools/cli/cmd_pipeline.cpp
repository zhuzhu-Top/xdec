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
#include "xdec/analysis/dominators.h"
#include "xdec/analysis/emit_redundancy.h"
#include "xdec/analysis/expr_reuse.h"
#include "xdec/analysis/loops.h"
#include "xdec/analysis/profile.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/analysis/typed_variables.h"
#include "xdec/analysis/variables.h"
#include "xdec/binary/target_profile.h"
#include "xdec/decompile/driver.h"
#include "xdec/emit/c_printer.h"
#include "xdec/emit/structure.h"
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

XDEC_DEFINE_LOG_CATEGORY(emitLog, "emit")

/// Times one stage of the analyse-and-emit tail, which the pass manager does not
/// cover: everything from the stack frame to the printed text runs outside the
/// pipeline, and on a large flattened function that is where the wall clock goes.
template <class Fn>
auto timed(std::string_view stage, Fn&& fn) {
  const auto started = std::chrono::steady_clock::now();
  auto out = std::forward<Fn>(fn)();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  XDEC_LOG_DEBUG(emitLog(), "{:>16} {:>6}ms", stage, elapsed.count());
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

  auto session = ToolSession::openBinary(path);
  if (!session) {
    return reportError(session.error());
  }
  const BinaryImage& image = *session->image;
  const xdec::spec::SpecEngine& engine = *session->engine;
  const xdec::spec::ByteReader reader =
      [&image](uint64_t va, std::span<std::byte> out) { return image.read(va, out); };

  // What the platform implies, applied wherever the user did not already say
  // something more specific: `--types`/`--syscall-table` are still there for
  // when the inference is wrong, but the common case -- an AArch64 .so, this
  // project's only supported target today -- no longer needs either flag
  // (see xdec/binary/target_profile.h).
  const xdec::binary::TargetProfile profile = xdec::binary::inferTargetProfile(image);
  if (typeSources.empty()) {
    typeSources = profile.typePresets;
  }
  if (syscallSource == xdec::types::SyscallTable::defaultName() && !profile.syscallTable.empty()) {
    syscallSource = profile.syscallTable;
  }

  xdec::types::TypeDatabase types;
  if (!typeSources.empty()) {
    auto loaded = loadTypes(typeSources, /*verbose=*/false);
    if (!loaded) {
      return reportError(loaded.error());
    }
    types = std::move(*loaded);
    print("types: {} type(s), {} declaration(s) from {} header(s)", types.typeCount(),
          types.declarations().size(), typeSources.size());
  }

  xdec::types::SyscallTable syscalls;
  if (!syscallSource.empty()) {
    const auto load = [&]() -> xdec::Result<xdec::types::SyscallTable> {
      XDEC_TRY(const std::string resolved,
               xdec::types::SyscallTable::resolvePath(syscallSource));
      return xdec::types::SyscallTable::loadFile(resolved);
    };
    auto loaded = load();
    if (!loaded) {
      // A table the user named must load: they said which ABI this is, and
      // guessing past that would name syscalls from the wrong kernel. The
      // default is different -- it is the build's own data file, and a broken
      // installation should not stop a decompilation that never needed it.
      if (syscallSource != xdec::types::SyscallTable::defaultName()) {
        return reportError(loaded.error());
      }
      print("note: no syscall table ({}); svc will print as __xdec_syscall",
            loaded.error().format());
    } else {
      syscalls = std::move(*loaded);
    }
  }
  // Both loaded independently (the syscall table has no reason to depend on
  // whether a header was given, see SyscallTable::resolveTypes), so the link
  // between them is made here, once, rather than by either constructor.
  if (!typeSources.empty() && !syscalls.empty()) {
    syscalls.resolveTypes(types);
  }

  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);
  xdec::decompile::DriverOptions driverOptions;
  // Vars, not Resolved: emission reads the recovered call arity off the IL, and
  // stopping a level short would leave it guessing (see passes/vars.h).
  driverOptions.target = xdec::il::Maturity::Vars;
  driverOptions.maxRounds = roundCap.value;
  driverOptions.extendWhileProving = !roundCap.pinned;
  driverOptions.sealUnresolvedBranches = allowUnresolved;
  driverOptions.memory = memoryFactsOf(image);
  driverOptions.types = typeSources.empty() ? nullptr : &types;
  driverOptions.syscalls = syscalls.empty() ? nullptr : &syscalls;
  driverOptions.names = nameResolverOf(image, reader, driverOptions.memory, profile);
  if (const xdec::binary::Symbol* symbol = image.symbolAt(address);
      symbol != nullptr && symbol->size != 0) {
    driverOptions.fence = {address, address + symbol->size};
  } else if (!looksLikePrologue(engine, image, address)) {
    // No symbol to fence discovery with, and the entry itself does not look
    // like a function start either: exactly the shape that let sub_627ac
    // silently balloon into 1349 discovered addresses and 44k lines. Warn,
    // rather than fail -- an unusual but real entry (a hand-written
    // trampoline, a prologue-less leaf) is still worth decompiling, just not
    // silently mistaken for one when it might not be.
    print("warning: {:#x} has no symbol and its first instruction does not look like a "
          "function prologue; if this address is inside another function's body (a "
          "jump-table target, say) rather than a real entry, discovery has no function "
          "size to stay inside and this run may pull in unrelated code",
          address);
  }
  auto result = xdec::decompile::decompile(engine, reader, address, registry, driverOptions);
  if (!result) {
    return reportError(result.error());
  }
  const xdec::il::Function& function = *result->function;
  print("driver: {} round(s), {} extra entrie(s), {} block(s) total{}", result->report.rounds,
        result->report.extraEntries.size(), function.blockCount(),
        result->report.converged ? "" : " (round cap reached; coverage may be partial)");
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

  const xdec::analysis::StackFrame frame =
      timed("stack-frame", [&] { return xdec::analysis::StackFrame::compute(function); });
  xdec::analysis::VariableTable variables = timed(
      "vars", [&] { return xdec::analysis::VariableTable::recover(function, frame); });
  if (emitReport) {
    const xdec::analysis::EmitRedundancyReport emitRedundancy = timed(
        "emit-report", [&] { return xdec::analysis::analyzeEmitRedundancy(function, frame, variables); });
    print("emit-report: {}", emitRedundancy.format());
  }
  // Outlives this block -- COptions::typedVariables below borrows it, so it
  // has to still be alive at emission, not just while applyImportedTypes
  // runs. Default-constructed (every lookup unset) when neither a header nor
  // a syscall table was given, the same "no evidence" shape TypedVariables
  // itself returns for that case.
  xdec::analysis::TypedVariables typedVariables;
  // Built unconditionally: CContext's own binder and calleeName (below) reuse
  // this exact resolver, so a callee named through a PLT stub's import (see
  // nameResolverOf) resolves the same way whether or not a header ended up
  // loaded.
  const xdec::types::NameAt namesForTypes = [&](uint64_t va) {
    const xdec::pass::SymbolName symbol = driverOptions.names(va);
    return xdec::types::BoundName{symbol.name, symbol.isFunction};
  };
  if (driverOptions.types != nullptr || driverOptions.syscalls != nullptr) {
    typedVariables = timed("typed-variables", [&] {
      return xdec::analysis::TypedVariables::recover(function, frame, driverOptions.types,
                                                      driverOptions.syscalls, namesForTypes, {},
                                                      driverOptions.memory, &profile);
    });
    if (driverOptions.types != nullptr) {
      const xdec::types::TypeBinder binder(*driverOptions.types, namesForTypes);
      variables.applyImportedTypes(typedVariables, binder);
    }
  }
  const xdec::analysis::Dominators dominators =
      timed("dominators", [&] { return xdec::analysis::Dominators::compute(function); });
  const xdec::analysis::PostDominators postDominators = timed(
      "post-dominators", [&] { return xdec::analysis::PostDominators::compute(function); });
  const std::vector<xdec::analysis::NaturalLoop> loops =
      timed("loops", [&] { return xdec::analysis::naturalLoops(function, dominators); });
  const xdec::emit::StructuredFunction structured = timed("structure", [&] {
    return xdec::emit::structureFunction(function, dominators, postDominators, loops);
  });
  xdec::emit::COptions cOptions;
  cOptions.annotateBlocks = annotate;
  cOptions.symbols = symbolResolverOf(image);
  cOptions.addresses = addressDescriberOf(image);
  cOptions.types = driverOptions.types;
  cOptions.syscalls = driverOptions.syscalls;
  cOptions.typedVariables = &typedVariables;
  cOptions.names = namesForTypes;
  cOptions.memory = driverOptions.memory;
  cOptions.helpersHeader = helpersHeader;
  cOptions.indexedArgumentNames = indexedArgumentNames;
  cOptions.securityHintsAsComments = securityHintsAsComments;
  const std::string text = timed("print", [&] {
    return xdec::emit::printFunction(function, variables, frame, structured, cOptions);
  });

  print("emit: {} argument(s), {} local(s), {} temp(s), {} labeled block(s)",
        variables.arguments().size(), variables.locals().size(),
        variables.temps().size(), structured.labeled.size());
  if (outPath.empty()) {
    print("{}", text);
  } else {
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
