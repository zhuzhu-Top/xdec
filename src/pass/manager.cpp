#include "xdec/pass/manager.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <optional>

#include "xdec/il/verify.h"
#include "xdec/obs/context.h"
#include "xdec/obs/job.h"
#include "xdec/obs/log.h"
#include "xdec/obs/metrics.h"
#include "xdec/obs/span.h"

namespace xdec::pass {

// What each pass cost, and how big the function was when it ran.
//
// A pipeline over a few hundred blocks finishes before anyone wonders; over a
// few thousand it can take minutes, and then the only question is which pass —
// which is not answerable from the outside, because the pipeline reports nothing
// until it is done. Set XDEC_LOG=pass=debug for a line per pass.
XDEC_DEFINE_LOG_CATEGORY(passLog, "pass")
XDEC_DEFINE_COUNTER(passRuns, "pass.runs")
XDEC_DEFINE_COUNTER(passIterations, "pass.iterations")

namespace {

[[nodiscard]] std::size_t largestBlockOpCount(const il::Function& function) {
  std::size_t maxOps = 0;
  for (const il::BlockId blockId : function.blockHandles()) {
    maxOps = std::max(maxOps, function.block(blockId).ops.size());
  }
  return maxOps;
}

}  // namespace

Result<std::vector<RunStats>> Manager::run(
    il::Function& function, std::span<Pass* const> pipeline) const {
  std::vector<RunStats> stats;
  stats.reserve(pipeline.size());

  for (std::size_t index = 0; index < pipeline.size(); ++index) {
    Pass* pass = pipeline[index];
    const PassInfo& info = pass->info();

    // The resolver checked this statically, but a hand-assembled pipeline can
    // reach run() too, and the error should say the same thing either way.
    if (function.maturity() != info.level) {
      return err(DiagCode::Internal, "pass '{}' expects the function at {}, but it is at {}",
                 info.name, il::toString(info.level), il::toString(function.maturity()));
    }

    // Provenance: every op the pass appends carries its name.
    function.setCurrentPass(function.internPass(info.name));

    if (observer_ != nullptr) {
      observer_->beforePass(*pass, function);
    }

    Context context(function);
    if (image_.has_value()) {
      context.setImage(*image_);
    }
    context.setMemoryFacts(memory_);
    context.setSealUnresolvedBranches(seal_);
    context.setTypeDatabase(types_);
    context.setSyscallTable(syscalls_);
    context.setEntryRegFacts(entryRegs_);
    context.setPathExploreOptions(pathExplore_);
    context.setAbi(abi_);
    context.setRegions(regions_);
    if (names_) {
      context.setNames(names_);
    }
    if (discoverySink_) {
      context.setDiscoverySink(discoverySink_);
    }
    RunStats slot{.passName = info.name, .iterations = 0, .changed = false};
    slot.blocksBefore = function.blockCount();
    slot.opsBefore = function.opCount();
    slot.exprsBefore = function.exprCount();
    obs::checkpoint(info.name);
    XDEC_LOG_CONTEXT("pass", info.name);
    obs::reportProgress("pipeline", std::format("pass {}/{} {}", index + 1, pipeline.size(), info.name),
                        index + 1, pipeline.size());
    obs::Span passSpan{"pass", info.name,
                       obs::kv("blocks", slot.blocksBefore, "ops", slot.opsBefore, "exprs",
                               slot.exprsBefore)};
    passRuns().add();
    const auto started = std::chrono::steady_clock::now();
    XDEC_LOG_DEBUG(passLog(), "{:>18} start {} block(s) {} op(s) {} expr(s)", info.name,
                   function.blockCount(), function.opCount(), function.exprCount());
    do {
      const auto iterationStart = std::chrono::steady_clock::now();
      std::optional<obs::Span> iterationSpan;
      if (info.fixpoint) {
        iterationSpan.emplace("pass.iter", info.name, obs::kv("iteration", slot.iterations + 1));
        obs::checkpoint(info.name);
      }
      passIterations().add();
      Result<bool> changed = pass->run(context);
      if (!changed) {
        // The pipeline stops here, so the failing pass names itself; without
        // this the error surfaces stages away from the code that caused it.
        std::unique_ptr<Diag> diag = std::move(changed).takeUnexpected().release();
        diag->note(std::format("while running pass '{}'", info.name));
        return Unexpected{std::move(diag)};
      }
      ++slot.iterations;
      slot.changed = slot.changed || *changed;
      if (iterationSpan) {
        iterationSpan->set("changed", *changed);
        iterationSpan->set("ops", function.opCount());
        iterationSpan->set("exprs", function.exprCount());
        iterationSpan->end();
      }
      if (info.fixpoint) {
        const int64_t iterationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - iterationStart)
                                        .count();
        XDEC_LOG_DEBUG(passLog(), "{:>18} iter {:>2} {:6}ms changed={} {} op(s) {} expr(s)",
                       info.name, slot.iterations, iterationMs, *changed ? "yes" : "no ",
                       function.opCount(), function.exprCount());
        // A single iteration this slow is not "converging slowly" (the
        // kFixpointLimit bound below already catches that): it means one op or
        // block inside this run() call is doing disproportionate work, and the
        // per-iteration total gives no clue which. Set XDEC_LOG=local=debug (or
        // the pass's own category) for the sub-phase breakdown; see
        // eval/FINDINGS.md's "mega-block local-simplify" note for the playbook
        // this line exists to point at.
        constexpr int64_t kSlowIterationWarningMs = 30'000;
        if (iterationMs >= kSlowIterationWarningMs) {
          XDEC_LOG_WARN(passLog(),
                       "{} iteration {} took {}ms on {} op(s), {} block(s) (max {} op(s) in one "
                       "block) -- a sub-phase inside the pass is likely the bottleneck, not the "
                       "fixpoint loop itself",
                       info.name, slot.iterations, iterationMs, function.opCount(),
                       function.blockCount(), largestBlockOpCount(function));
        }
      }
      if (!info.fixpoint || !*changed) {
        break;
      }
      if (slot.iterations >= kFixpointLimit) {
        return err(DiagCode::AnalysisLimit,
                   "fixpoint pass '{}' did not converge after {} iterations", info.name,
                   kFixpointLimit);
      }
    } while (true);

    const auto ran = std::chrono::steady_clock::now();
    slot.elapsed = ran - started;

    // The produced level's invariants are the pass's contract; check them
    // before anyone else builds on the result. A pass that made no changes and
    // stayed at the already-verified maturity cannot have broken them.
    il::VerifyReport report;
    const bool needsVerify = slot.changed || info.produces != function.maturity();
    if (needsVerify) {
      const obs::Span verifySpan{"verify", info.name};
      report = il::verify(function, info.produces);
    }
    slot.verifyElapsed = std::chrono::steady_clock::now() - ran;
    slot.blocksAfter = function.blockCount();
    slot.opsAfter = function.opCount();
    slot.exprsAfter = function.exprCount();
    passSpan.set("blocks_out", slot.blocksAfter);
    passSpan.set("ops_out", slot.opsAfter);
    passSpan.set("exprs_out", slot.exprsAfter);
    passSpan.set("iterations", slot.iterations);
    passSpan.set("changed", slot.changed);
    // Verification separately from the pass: it walks the whole function on
    // every pass boundary, so on a large one it can be the expensive half, and a
    // total that hides which half it was answers nothing.
    XDEC_LOG_DEBUG(passLog(), "{:>18} {:6}ms (+{:4}ms verify) {} block(s){}", info.name,
                   std::chrono::duration_cast<std::chrono::milliseconds>(slot.elapsed).count(),
                   std::chrono::duration_cast<std::chrono::milliseconds>(slot.verifyElapsed)
                       .count(),
                   function.blockCount(),
                   slot.iterations > 1 ? std::format(" x{}", slot.iterations) : "");
    if (!report.ok()) {
      return err(DiagCode::VerifyFailure,
                 "pass '{}' produced IL that fails verification at {}:\n{}", info.name,
                 il::toString(info.produces), report.format());
    }
    function.setMaturity(info.produces);

    if (observer_ != nullptr) {
      observer_->afterPass(*pass, function, slot);
    }
    stats.push_back(std::move(slot));
  }

  if (observer_ != nullptr) {
    observer_->pipelineDone(function);
  }
  return stats;
}

Result<std::vector<RunStats>> Manager::runTo(il::Function& function,
                                             const Registry& registry,
                                             il::Maturity target) const {
  XDEC_TRY(const std::vector<Pass*> pipeline,
           registry.resolve(function.maturity(), target));
  return run(function, pipeline);
}

}  // namespace xdec::pass
