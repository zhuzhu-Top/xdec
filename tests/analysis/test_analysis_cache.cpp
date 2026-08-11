// AnalysisCache: lazy computation, stats that prove the laziness, and
// tag-selective invalidation.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

#include "il/il_test_support.h"
#include "xdec/analysis/analysis_cache.h"
#include "xdec/il/function.h"

using xdec::Arch;
using xdec::analysis::AnalysisCache;
using xdec::il::BlockId;
using xdec::il::Function;

namespace {

/// The same bare CFG builder tests/analysis/test_analysis.cpp uses, kept
/// local rather than shared: each test file's version stays a two-line
/// dependency, not a fixture with its own maintenance surface.
class Cfg {
 public:
  Cfg() : function_(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {}

  BlockId add() {
    const BlockId block = function_.createBlock(0x1000 + 4 * next_++);
    if (!function_.entryBlock().valid()) {
      function_.setEntryBlock(block);
    }
    return block;
  }

  void link(BlockId from, BlockId to) {
    function_.appendBranch(from, function_.block(from).va, to);
  }

  void fork(BlockId from, BlockId taken, BlockId fallthrough) {
    const xdec::il::RegId x0 = function_.registers().find("x0");
    const xdec::il::ValueId cond = function_.appendReadReg(from, function_.block(from).va, x0);
    function_.appendCondBranch(from, function_.block(from).va, function_.valueRef(cond), taken,
                               fallthrough);
  }

  void end(BlockId block) { function_.appendReturn(block, function_.block(block).va); }

  Function& finish() {
    function_.rebuildEdges();
    return function_;
  }

 private:
  Function function_;
  uint32_t next_ = 0;
};

//        a
//      /   +
//     b     c
//      +   /
//        d
[[nodiscard]] Function& diamond(Cfg& cfg) {
  const BlockId a = cfg.add();
  const BlockId b = cfg.add();
  const BlockId c = cfg.add();
  const BlockId d = cfg.add();
  cfg.fork(a, b, c);
  cfg.link(b, d);
  cfg.link(c, d);
  cfg.end(d);
  return cfg.finish();
}

TEST_CASE("AnalysisCache computes each analysis at most once across repeated reads",
          "[analysis][cache]") {
  Cfg cfg;
  const Function& function = diamond(cfg);
  const AnalysisCache cache(function);

  // Three reads each, in an order that would surface a "recomputes on every
  // call" bug (loops() first, which itself reads dominators() through the
  // cache -- see AnalysisCache::loops()'s own doc comment -- so a bug that
  // bypassed the cache internally would double dominators' count too).
  (void)cache.loops();
  (void)cache.loops();
  (void)cache.dominators();
  (void)cache.postDominators();
  (void)cache.postDominators();
  (void)cache.stackFrame();
  (void)cache.stackFrame();
  (void)cache.stackFrame();

  const auto& stats = cache.stats();
  CHECK(stats.dominatorsComputed == 1);
  CHECK(stats.postDominatorsComputed == 1);
  CHECK(stats.loopsComputed == 1);
  CHECK(stats.stackFrameComputed == 1);
}

TEST_CASE("AnalysisCache answers match computing the analyses directly", "[analysis][cache]") {
  Cfg cfg;
  const Function& function = diamond(cfg);
  const AnalysisCache cache(function);

  const xdec::analysis::Dominators direct = xdec::analysis::Dominators::compute(function);
  const xdec::analysis::Dominators& cached = cache.dominators();
  REQUIRE(cached.rpo().size() == direct.rpo().size());
  for (std::size_t i = 0; i < direct.rpo().size(); ++i) {
    CHECK(cached.idom(direct.rpo()[i]) == direct.idom(direct.rpo()[i]));
  }
}

TEST_CASE("invalidate() with no tags drops every cached analysis", "[analysis][cache]") {
  Cfg cfg;
  const Function& function = diamond(cfg);
  AnalysisCache cache(function);

  (void)cache.dominators();
  (void)cache.postDominators();
  (void)cache.loops();
  (void)cache.stackFrame();
  REQUIRE(cache.stats().dominatorsComputed == 1);
  REQUIRE(cache.stats().stackFrameComputed == 1);

  cache.invalidate();
  (void)cache.dominators();
  (void)cache.postDominators();
  (void)cache.loops();
  (void)cache.stackFrame();

  CHECK(cache.stats().dominatorsComputed == 2);
  CHECK(cache.stats().postDominatorsComputed == 2);
  CHECK(cache.stats().loopsComputed == 2);
  CHECK(cache.stats().stackFrameComputed == 2);
}

TEST_CASE("invalidate() only drops the analyses its tags name", "[analysis][cache]") {
  Cfg cfg;
  const Function& function = diamond(cfg);
  AnalysisCache cache(function);

  (void)cache.dominators();
  (void)cache.postDominators();
  (void)cache.loops();
  (void)cache.stackFrame();

  // "dominators" is exactly what cfg_finalize/ssa_construct/ssa_optimize
  // declare in their own PassInfo::invalidates (see pass/pass.h and
  // passes/cfg_finalize.cpp) -- the vocabulary a caller bridging a real
  // pass's report into this call would actually pass.
  const std::array<std::string_view, 1> tags{"dominators"};
  cache.invalidate(tags);

  (void)cache.dominators();
  (void)cache.postDominators();
  (void)cache.loops();
  CHECK(cache.stats().dominatorsComputed == 2);
  CHECK(cache.stats().postDominatorsComputed == 2);
  CHECK(cache.stats().loopsComputed == 2);

  // Untagged: still the one original computation.
  (void)cache.stackFrame();
  CHECK(cache.stats().stackFrameComputed == 1);
}

}  // namespace
