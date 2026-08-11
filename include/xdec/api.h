// The public surface: what an embedder -- an MCP tool, a finetuning corpus
// builder, a research script comparing xdec against another decompiler --
// includes when all it wants is "binary in, C text out" and does not want to
// learn xdec's internal library layout (xdec_decompile/xdec_emit/xdec_analysis
// etc, under include/xdec/<domain>/) to get it.
//
// This header adds no logic of its own: every name here is a re-export of
// something xdec::decompile (see xdec/decompile/emit.h) already defines and
// the CLI's own `decompile` command already calls (cmd_pipeline.cpp). That is
// deliberate -- a public API that duplicated the pipeline, even thinly, could
// drift from what the CLI actually ships; aliasing it instead makes drift
// impossible; and a caller that outgrows this thin surface can `#include`
// xdec/decompile/emit.h directly for the exact same types under their
// original names.
#pragma once

#include "xdec/decompile/emit.h"

namespace xdec {

/// Everything one decompileToC() call needs beyond the binary itself: which
/// analyses to run it with (`driver`), how to format the result (`emit`),
/// and whether to pay for the optional emit-redundancy scan
/// (`computeEmitRedundancy`). See xdec::decompile::DecompileToCOptions for
/// the full field list.
using DecompileOptions = decompile::DecompileToCOptions;

/// What one decompileToC() call produces: the lifted+driven IL function, the
/// driver's own discovery report, the recovered variables/stack frame, the
/// structured control flow, the printed C text, and `report` -- stage
/// timings plus (when asked for) the emit-redundancy scan. See
/// xdec::decompile::DecompileToCResult for the full field list.
using DecompileResult = decompile::DecompileToCResult;

/// Stage timings and emit-side statistics for one decompileToC() call. See
/// xdec::decompile::DecompileReport for the full field list.
using DecompileReport = decompile::DecompileReport;

/// Lifts `entry` out of `reader` (an image's raw bytes), drives it to Vars
/// IL, and renders it to C -- the same call the CLI's `decompile` command
/// makes and PipelineFixture's `decompileToCFromBinary` wraps for tests (see
/// tests/fixture/pipeline_fixture.h). `engine` is the compiled architecture
/// spec (see xdec::spec::loadSpecFile) and `registry` the builtin (or
/// plugin-extended) pass set (see xdec::passes::registerBuiltinPasses) --
/// both cheap to build once and reuse across many `entry` values in the same
/// binary, which is why neither is owned by `options`.
using decompile::decompileToC;

}  // namespace xdec
