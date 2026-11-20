#include "xdec/decompile/from_il.h"

#include <chrono>
#include <utility>

#include "xdec/analysis/image_literals.h"
#include "xdec/il/verify.h"
#include "xdec/obs/context.h"
#include "xdec/obs/span.h"
#include "xdec/support/stop.h"

namespace xdec::decompile {

Result<DecompileToCResult> decompileFromIL(std::unique_ptr<il::Function> function,
                                            pass::Registry& registry,
                                            const DecompileFromILOptions& options) {
  if (!function) {
    return err(DiagCode::Internal, "decompileFromIL: null il::Function");
  }
  return catchStop([&]() -> Result<DecompileToCResult> {
    const uint64_t entryVa =
        function->entryBlock().valid() ? function->block(function->entryBlock()).va : 0;
    XDEC_LOG_CONTEXT("entry", obs::hex(entryVa));

    const il::VerifyReport inputReport = il::verify(*function, function->maturity());
    if (!inputReport.ok()) {
      return err(DiagCode::VerifyFailure,
                 "decompileFromIL input IL failed verification at {}:\n{}",
                 il::toString(function->maturity()), inputReport.format());
    }

    ByteReader reader;
    MemoryFacts facts = options.memoryFacts;
    if (options.layeredMemory != nullptr) {
      reader = options.layeredMemory->asByteReader();
      facts = options.layeredMemory->asMemoryFacts();
    } else if (options.memory.has_value()) {
      reader = *options.memory;
    }

    pass::Manager manager(options.observer);
    if (reader) {
      manager.setImage(reader);
    } else {
      manager.setImage([](uint64_t va, std::span<std::byte>) -> Result<void> {
        return err(DiagCode::UnmappedAddress, "no image bytes mapped at {:#x}", va);
      });
    }
    manager.setMemoryFacts(facts);
    manager.setSealUnresolvedBranches(options.sealUnresolvedBranches);
    manager.setTypeDatabase(options.types);
    manager.setSyscallTable(options.syscalls);
    if (options.names) {
      manager.setNames(options.names);
    }
    if (options.abi.has_value()) {
      manager.setAbi(&*options.abi);
    }
    if (options.regions != nullptr) {
      manager.setRegions(options.regions);
    }

    DriverReport driverReport;
    driverReport.stop = DriverStop::NoDiscovery;
    driverReport.converged = true;
    if (function->maturity() < il::Maturity::Vars) {
      XDEC_TRY(std::vector<pass::RunStats> stats,
               manager.runTo(*function, registry, il::Maturity::Vars));
      (void)stats;
    }

    DecompileToCOptions emitOpts;
    emitOpts.driver.types = options.types;
    emitOpts.driver.syscalls = options.syscalls;
    emitOpts.driver.names = options.names;
    emitOpts.driver.memory = facts;
    emitOpts.emit = options.emit;
    if (emitOpts.emit.name.empty() && !function->name().empty()) {
      emitOpts.emit.name = function->name();
    }
    if (!emitOpts.emit.imageReader && reader) {
      emitOpts.emit.imageReader = reader;
    }
    if (!emitOpts.emit.addresses && options.layeredMemory != nullptr) {
      const auto* lm = options.layeredMemory;
      emitOpts.emit.addresses = [lm](uint64_t va) {
        emit::AddressFacts af;
        if (lm->isImmutable(va, 1)) {
          af.mapped = true;
          af.writable = false;
          af.variable = true;
        }
        return af;
      };
    }
    if (!emitOpts.emit.symbols && options.names) {
      const pass::NameAt names = options.names;
      emitOpts.emit.symbols = [names](uint64_t va) {
        const pass::SymbolName sym = names(va);
        emit::SymbolRef ref;
        ref.name = sym.name;
        ref.isFunction = sym.isFunction;
        return ref;
      };
    }
    emitOpts.structure = options.structure;
    emitOpts.profile = options.profile;
    emitOpts.abi = options.abi.has_value() ? &*options.abi : nullptr;
    emitOpts.computeEmitRedundancy = options.computeEmitRedundancy;
    emitOpts.computeObfuscationProfile = options.computeObfuscationProfile;
    emitOpts.captureAst = options.captureAst;

    RenderToCResult rendered = renderToC(*function, emitOpts);
    return DecompileToCResult{std::move(function),        std::move(driverReport),
                              std::move(rendered.variables), rendered.frame,
                              std::move(rendered.structured), std::move(rendered.cSource),
                              std::move(rendered.ast),       std::move(rendered.report)};
  });
}

}  // namespace xdec::decompile
