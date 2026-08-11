// decompileToC: the driver plus everything after it, as one call.
//
// `decompile()` (driver.h) only reaches Vars IL -- the CFG analyses, the
// structurizer and the C printer used to run exclusively inside the CLI's
// `decompile` command (cmd_pipeline.cpp), which meant every other caller that
// wanted C text (a test, a plugin host, an MCP server) either linked the CLI
// or re-typed the same dozen calls in the same order. This header is that
// sequence, extracted once: stack frame, variables, optional typed-variable
// recovery, dominators/post-dominators/loops, structuring, printing.
//
// What stays outside: opening a binary and turning it into a ByteReader plus
// the various xdec::cli::*Of() adapters (symbols, addresses, image bytes,
// names) is binary-format plumbing this layer does not need to know about --
// exactly why `decompile()` itself already takes a bare ByteReader rather
// than a BinaryImage. Nothing here changes that; `xdec_decompile` still does
// not depend on `xdec_binary`.
//
// `il::Maturity::Structured`/`Typed` exist as enum values (maturity.h) but no
// pass ever claims either one, and the architecture plan deliberately defers
// making that real (a verifier over the `Stmt` AST is real work for little
// gain over what is already measured against `eval`/`samples`). Reading
// `DecompileToCResult::structured` -- what this call already produces on the
// way to `cSource` -- is that gap's answer instead: a caller that wants the
// structured tree without the maturity ratchet already has it, no new pass
// required.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "xdec/analysis/emit_redundancy.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/analysis/variables.h"
#include "xdec/binary/target_profile.h"
#include "xdec/decompile/driver.h"
#include "xdec/emit/c_printer.h"
#include "xdec/emit/structure.h"
#include "xdec/il/function.h"
#include "xdec/pass/registry.h"
#include "xdec/spec/engine.h"
#include "xdec/support/log.h"
#include "xdec/support/reader.h"
#include "xdec/support/result.h"

namespace xdec::decompile {

XDEC_DECLARE_LOG_CATEGORY(emitLog)

/// What DriverReport (driver.h) does not carry: everything about the
/// structure-and-print tail, gathered the same way cmd_pipeline.cpp's own
/// `timed()` helper always has, so a caller of decompileToC() -- the CLI's
/// `--emit-report`, an MCP tool, a finetuning corpus builder -- gets one
/// structured answer instead of three separately-timed diagnostics that can
/// drift out of sync with what the pipeline actually did.
struct DecompileReport {
  struct StageTiming {
    std::string_view stage;
    std::chrono::milliseconds elapsed{0};
  };
  /// One entry per stage renderToC() ran, in order (stack-frame, vars,
  /// [typed-variables], dominators, post-dominators, loops, structure,
  /// print, [emit-redundancy]) -- bracketed stages are skipped, not zeroed,
  /// exactly like the debug log lines they replace nothing about (see
  /// XDEC_LOG_DEBUG(emitLog(), ...) at each of these same call sites).
  std::vector<StageTiming> stageTimings;
  /// `structured.labeled.size()` at the point printFunction ran -- how many
  /// blocks still needed a `goto` target, the metric GCSF/SER/AFC exist to
  /// drive toward zero (see docs/16-guard-cascade.md).
  std::size_t labeledBlockCount = 0;
  /// Only present when `DecompileToCOptions::computeEmitRedundancy` asked
  /// for it: computing analyzeEmitRedundancy() is itself a function-sized
  /// scan, so it stays opt-in the same way the CLI's `--emit-report` flag
  /// always has, rather than always paying for it inside every
  /// decompileToC() call.
  std::optional<analysis::EmitRedundancyReport> emitRedundancy;
};

struct DecompileToCOptions {
  DriverOptions driver;
  /// Output formatting only: `name`, `annotateBlocks`, `preferIfOverTernary`,
  /// `indexedArgumentNames`, `securityHintsAsComments`, `symbols`,
  /// `addresses`, `imageReader` and `helpersHeader` are read as given.
  ///
  /// `types`, `syscalls`, `memory`, `names` and `typedVariables` are instead
  /// *derived* from `driver` (and `profile` below) by decompileToC itself --
  /// whatever this struct's copy holds for them going in is overwritten.
  /// They exist on COptions once already, and a second, independently-set
  /// copy here would be exactly the "same fact wired twice" duplication the
  /// CLI had (see cmd_pipeline.cpp before this was extracted): `driver.types`
  /// feeds both the pass pipeline's type-directed folding and the printer's
  /// declarations, and they must be the same TypeDatabase or the two halves
  /// of one decompilation could disagree about a callee's signature.
  emit::COptions emit;
  /// Platform hints for typed-variable recovery's import-slot aliasing (see
  /// analysis::TypedVariables::recover's `profile` parameter). Absent skips
  /// aliasing, same as a caller with no better guess.
  const binary::TargetProfile* profile = nullptr;
  /// See DecompileReport::emitRedundancy. Off by default: the scan is only
  /// worth its cost when something is actually going to read the report.
  bool computeEmitRedundancy = false;
};

struct DecompileToCResult {
  std::unique_ptr<il::Function> function;
  DriverReport driverReport;
  analysis::VariableTable variables;
  analysis::StackFrame frame;
  emit::StructuredFunction structured;
  std::string cSource;
  DecompileReport report;
};

/// Lifts and drives `entry` to Vars IL (see decompile()), then structures and
/// prints it to C. The analyses driver.cpp already logs its own timings for;
/// this adds the same per-stage debug logging (category "emit") for
/// everything after it, which is what a caller diagnosing where the wall
/// clock goes on a large flattened function needs regardless of whether it
/// is the CLI or something else driving this -- and, unlike the log line,
/// leaves the numbers in the result too (see DecompileToCResult::report).
[[nodiscard]] Result<DecompileToCResult> decompileToC(const spec::SpecEngine& engine,
                                                       const ByteReader& reader, uint64_t entry,
                                                       pass::Registry& registry,
                                                       const DecompileToCOptions& options);

/// Everything decompileToC does after the driver hands back a function --
/// the half of `DecompileToCResult` that has nothing to do with lifting or
/// discovery. Split out so a caller that already has an il::Function at Vars
/// maturity (a hand-built one in a test, say -- see
/// tests/fixture/pipeline_fixture.h) can run exactly the same structuring and
/// printing decompileToC uses without a spec engine, a ByteReader or a driver
/// round to get there, and so decompileToC itself has only one place this
/// logic can drift from.
struct RenderToCResult {
  analysis::VariableTable variables;
  analysis::StackFrame frame;
  emit::StructuredFunction structured;
  std::string cSource;
  DecompileReport report;
};

[[nodiscard]] RenderToCResult renderToC(const il::Function& function,
                                        const DecompileToCOptions& options);

}  // namespace xdec::decompile
