#include "xdec/decompile/emit.h"

#include <chrono>
#include <string_view>
#include <utility>

#include "xdec/analysis/analysis_cache.h"
#include "xdec/analysis/dominators.h"
#include "xdec/analysis/loops.h"
#include "xdec/analysis/typed_variables.h"
#include "xdec/types/binder.h"
#include "xdec/types/database.h"
#include "xdec/types/syscall_table.h"

namespace xdec::decompile {

XDEC_DEFINE_LOG_CATEGORY(emitLog, "emit")

Result<DecompileToCResult> decompileToC(const spec::SpecEngine& engine, const ByteReader& reader,
                                         uint64_t entry, pass::Registry& registry,
                                         const DecompileToCOptions& options) {
  XDEC_TRY(DriverResult driven, decompile(engine, reader, entry, registry, options.driver));
  RenderToCResult rendered = renderToC(*driven.function, options);
  return DecompileToCResult{std::move(driven.function),     std::move(driven.report),
                            std::move(rendered.variables),  rendered.frame,
                            std::move(rendered.structured), std::move(rendered.cSource),
                            std::move(rendered.report)};
}

RenderToCResult renderToC(const il::Function& function, const DecompileToCOptions& options) {
  DecompileReport report;
  // Times one stage of the analyse-and-emit tail, the same way driver.cpp
  // times a round: on a large flattened function this is where the wall
  // clock goes, and both a caller with XDEC_LOG=emit=debug set and a caller
  // reading `report.stageTimings` back from the result want to see which
  // stage -- the same measurement, kept in sync by construction rather than
  // by two call sites agreeing to measure the same span.
  const auto timed = [&report](std::string_view stage, auto&& fn) {
    const auto started = std::chrono::steady_clock::now();
    auto out = std::forward<decltype(fn)>(fn)();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    XDEC_LOG_DEBUG(emitLog(), "{:>16} {:>6}ms", stage, elapsed.count());
    report.stageTimings.push_back({stage, elapsed});
    return out;
  };

  // Lazy by construction, but every stage below asks for each analysis
  // exactly once in this single sequential call, so today this is a
  // behaviour-neutral stand-in for the direct ::compute() calls it replaces
  // -- the payoff is a caller that asks this same function's analyses more
  // than once (see analysis/analysis_cache.h's own doc comment), which
  // renderToC() itself is not.
  const analysis::AnalysisCache cache(function);
  const analysis::StackFrame frame =
      timed("stack-frame", [&] { return cache.stackFrame(); });
  analysis::VariableTable variables =
      timed("vars", [&] { return analysis::VariableTable::recover(function, frame); });

  // Bridges pass::NameAt (what the driver's discovery and type-directed
  // folding read symbols through) to types::NameAt (what a header binder
  // reads them through): one conversion, shared by typed-variable recovery
  // below and by the printer's own binder, so a callee resolves to the same
  // name on both sides of maturity Vars.
  const types::NameAt namesForTypes = [&](uint64_t va) {
    const pass::SymbolName symbol = options.driver.names ? options.driver.names(va) : pass::SymbolName{};
    return types::BoundName{symbol.name, symbol.isFunction};
  };

  analysis::TypedVariables typedVariables;
  if (options.driver.types != nullptr || options.driver.syscalls != nullptr) {
    typedVariables = timed("typed-variables", [&] {
      return analysis::TypedVariables::recover(function, frame, options.driver.types,
                                                options.driver.syscalls, namesForTypes, {},
                                                options.driver.memory, options.profile);
    });
    if (options.driver.types != nullptr) {
      const types::TypeBinder binder(*options.driver.types, namesForTypes);
      variables.applyImportedTypes(typedVariables, binder);
    }
  }

  // Each of these `timed()` calls into the cache still copies its result
  // into a named local exactly as the direct ::compute() calls this replaced
  // did (auto-by-value strips the reference cache.dominators() et al.
  // return): renderToC() reads every one of these exactly once, so there is
  // no second reader here for the cache to save a recomputation for (see
  // AnalysisCache's own doc comment for who that second reader actually is).
  const analysis::Dominators dominators = timed("dominators", [&] { return cache.dominators(); });
  const analysis::PostDominators postDominators =
      timed("post-dominators", [&] { return cache.postDominators(); });
  const std::vector<analysis::NaturalLoop> loops = timed("loops", [&] { return cache.loops(); });
  emit::StructuredFunction structured = timed("structure", [&] {
    return emit::structureFunction(function, dominators, postDominators, loops, options.structure);
  });
  report.labeledBlockCount = structured.labeled.size();

  emit::COptions cOptions = options.emit;
  cOptions.types = options.driver.types;
  cOptions.syscalls = options.driver.syscalls;
  cOptions.typedVariables = &typedVariables;
  cOptions.names = namesForTypes;
  cOptions.memory = options.driver.memory;
  std::string text = timed("print", [&] {
    return emit::printFunction(function, variables, frame, structured, cOptions);
  });

  if (options.computeEmitRedundancy) {
    report.emitRedundancy = timed("emit-redundancy", [&] {
      return analysis::analyzeEmitRedundancy(function, frame, variables);
    });
  }
  if (options.computeObfuscationProfile) {
    report.obfuscationProfile = timed("profile", [&] { return analysis::profile(function); });
  }

  return RenderToCResult{std::move(variables), frame, std::move(structured), std::move(text),
                         std::move(report)};
}

}  // namespace xdec::decompile
