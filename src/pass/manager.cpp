#include "xdec/pass/manager.h"

#include <chrono>

#include "xdec/il/verify.h"
#include "xdec/support/log.h"

namespace xdec::pass {

// What each pass cost, and how big the function was when it ran.
//
// A pipeline over a few hundred blocks finishes before anyone wonders; over a
// few thousand it can take minutes, and then the only question is which pass —
// which is not answerable from the outside, because the pipeline reports nothing
// until it is done. Set XDEC_LOG=pass=debug for a line per pass.
XDEC_DEFINE_LOG_CATEGORY(passLog, "pass")

Result<std::vector<RunStats>> Manager::run(
    il::Function& function, std::span<Pass* const> pipeline) const {
  std::vector<RunStats> stats;
  stats.reserve(pipeline.size());

  for (Pass* pass : pipeline) {
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
    if (names_) {
      context.setNames(names_);
    }
    if (discoverySink_) {
      context.setDiscoverySink(discoverySink_);
    }
    RunStats slot{.passName = info.name, .iterations = 0, .changed = false};
    const auto started = std::chrono::steady_clock::now();
    do {
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

    // The produced level's invariants are the pass's contract; check them
    // before anyone else builds on the result.
    const il::VerifyReport report = il::verify(function, info.produces);
    // Verification separately from the pass: it walks the whole function on
    // every pass boundary, so on a large one it can be the expensive half, and a
    // total that hides which half it was answers nothing.
    XDEC_LOG_DEBUG(passLog(), "{:>18} {:6}ms (+{:4}ms verify) {} block(s){}", info.name,
                   std::chrono::duration_cast<std::chrono::milliseconds>(ran - started)
                       .count(),
                   std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - ran)
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
