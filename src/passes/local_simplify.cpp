// makeLocalSimplifyPass: the block-local cleanup fixpoint (see the header).
#include "xdec/passes/local_simplify.h"

#include <algorithm>
#include <chrono>

#include "algebra.h"
#include "transform.h"
#include "xdec/il/function.h"
#include "xdec/support/log.h"

namespace xdec::passes {

// Sub-phase timings inside local-simplify. Set XDEC_LOG=local=debug.
XDEC_DEFINE_LOG_CATEGORY(localLog, "local")

namespace {

/// Milliseconds since the last lap, for the sub-phase timings.
class Clock {
 public:
  [[nodiscard]] int64_t lap() {
    const auto now = std::chrono::steady_clock::now();
    const auto since = now - at_;
    at_ = now;
    return std::chrono::duration_cast<std::chrono::milliseconds>(since).count();
  }

 private:
  std::chrono::steady_clock::time_point at_ = std::chrono::steady_clock::now();
};

[[nodiscard]] std::size_t maxBlockOps(const il::Function& function) {
  std::size_t maxOps = 0;
  for (const il::BlockId blockId : function.blockHandles()) {
    maxOps = std::max(maxOps, function.block(blockId).ops.size());
  }
  return maxOps;
}

class LocalSimplify final : public pass::FunctionPass {
 public:
  LocalSimplify()
      : FunctionPass([] {
          pass::PassInfo info;
          info.name = "local-simplify";
          info.level = il::Maturity::Lifted;
          info.produces = il::Maturity::Local;
          info.fixpoint = true;
          return info;
        }()) {}

  Result<bool> run(pass::Context& context) override {
    il::Function& function = context.function();
    Clock clock;
    // Algebra before folding: the rules strip filler the constant evaluator
    // cannot see through (and(x,0) over an unknown x, say), and folding then
    // collapses what the rules exposed. Both run again next iteration.
    const bool algebraChanged = simplifyAlgebra(function);
    const int64_t algebraMs = clock.lap();
    XDEC_LOG_DEBUG(localLog(), "  algebra {}ms{}", algebraMs, algebraChanged ? " changed" : "");
    const bool folded = foldConstants(function);
    const int64_t foldMs = clock.lap();
    XDEC_LOG_DEBUG(localLog(), "  fold {}ms{}", foldMs, folded ? " changed" : "");
    const bool unflagged = foldFlagConditions(function);
    const int64_t flagsMs = clock.lap();
    XDEC_LOG_DEBUG(localLog(), "  flags {}ms{}", flagsMs, unflagged ? " changed" : "");
    bool blockChanged = false;
    int64_t copyMs = 0;
    int64_t loadMs = 0;
    int64_t dceMs = 0;
    std::size_t megaBlocks = 0;
    for (const il::BlockId blockId : function.blockHandles()) {
      const il::Block& block = function.block(blockId);
      // copyPropagateBlock and forwardRedundantLoads both rebuild substituted
      // expression trees that grow with the block; past kMegaBlockOpThreshold
      // that growth is what hangs a straight-line MBA block for minutes (see
      // eval/FINDINGS.md's "mega-block local-simplify" note). dceBlock stays:
      // its cost tracks distinct expression nodes, not accumulated tree size.
      const bool isMegaBlock = block.ops.size() > kMegaBlockOpThreshold;
      bool copied = false;
      bool forwarded = false;
      int64_t copyBlockMs = 0;
      int64_t loadBlockMs = 0;
      if (isMegaBlock) {
        ++megaBlocks;
        (void)clock.lap();  // exclude the skip itself from the next timed section
      } else {
        copied = copyPropagateBlock(function, blockId);
        copyBlockMs = clock.lap();
        forwarded = forwardRedundantLoads(function, blockId);
        loadBlockMs = clock.lap();
      }
      copyMs += copyBlockMs;
      loadMs += loadBlockMs;
      const bool dead = dceBlock(function, blockId);
      const int64_t dceBlockMs = clock.lap();
      dceMs += dceBlockMs;
      blockChanged |= copied || forwarded || dead;
      if (block.ops.size() >= 256) {
        XDEC_LOG_DEBUG(localLog(), "  b{} @0x{:x} {} op(s){}: copy {}ms, loads {}ms, dce {}ms",
                       blockId.index(), block.va, block.ops.size(),
                       isMegaBlock ? " [mega-block: copy/loads skipped]" : "", copyBlockMs,
                       loadBlockMs, dceBlockMs);
      }
    }
    const bool changed = algebraChanged || folded || unflagged || blockChanged;
    // This pass runs to fixpoint and a single sub-phase can dominate on one
    // large MBA block, so the pass-level total from the manager is not enough.
    XDEC_LOG_DEBUG(
        localLog(),
        "algebra {}ms{}, fold {}ms{}, flags {}ms{}, copy {}ms, loads {}ms, dce {}ms; "
        "{} block(s) ({} mega) max {} op(s), {} op(s) {} expr(s)",
        algebraMs, algebraChanged ? " changed" : "", foldMs, folded ? " changed" : "", flagsMs,
        unflagged ? " changed" : "", copyMs, loadMs, dceMs, function.blockCount(), megaBlocks,
        maxBlockOps(function), function.opCount(), function.exprCount());
    return changed;
  }
};

}  // namespace

std::unique_ptr<pass::Pass> makeLocalSimplifyPass() {
  return std::make_unique<LocalSimplify>();
}

}  // namespace xdec::passes
