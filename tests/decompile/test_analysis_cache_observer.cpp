// AnalysisCacheObserver: bridges pass::Manager's per-pass report to
// AnalysisCache::invalidate() (see decompile/analysis_cache_observer.h).
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "il/il_test_support.h"
#include "xdec/analysis/analysis_cache.h"
#include "xdec/decompile/analysis_cache_observer.h"
#include "xdec/il/function.h"
#include "xdec/pass/manager.h"

using xdec::Arch;
using xdec::analysis::AnalysisCache;
using xdec::decompile::AnalysisCacheObserver;
using xdec::il::BlockId;
using xdec::il::Function;
using xdec::pass::Context;
using xdec::pass::FunctionPass;
using xdec::pass::Manager;
using xdec::pass::Pass;
using xdec::pass::PassInfo;
using xdec::Result;

namespace {

[[nodiscard]] Function makeFunction() {
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId entry = function.createBlock(0x1000);
  function.setEntryBlock(entry);
  const xdec::il::RegId x0 = function.registers().find("x0");
  const xdec::il::ValueId value = function.appendReadReg(entry, 0x1000, x0);
  function.appendWriteReg(entry, 0x1004, x0, function.valueRef(value));
  return function;
}

/// Reports `changed` on its first run only, then always false -- one run per
/// test is all Manager::run() needs, and "reports changed" is the only knob
/// AnalysisCacheObserver actually reads.
class OneShot : public FunctionPass {
 public:
  OneShot(PassInfo info, bool changed) : FunctionPass(std::move(info)), changed_(changed) {}
  Result<bool> run(Context&) override { return changed_; }

 private:
  bool changed_;
};

[[nodiscard]] PassInfo makeInfo(std::string name, std::vector<std::string> invalidates) {
  PassInfo info;
  info.name = std::move(name);
  info.invalidates = std::move(invalidates);
  return info;
}

}  // namespace

TEST_CASE("AnalysisCacheObserver leaves the cache alone when the pass reports no change",
          "[decompile][analysis-cache-observer]") {
  Function function = makeFunction();
  AnalysisCache cache(function);
  (void)cache.dominators();
  REQUIRE(cache.stats().dominatorsComputed == 1);

  OneShot pass(makeInfo("noop", {"dominators"}), /*changed=*/false);
  AnalysisCacheObserver observer(cache);
  Manager manager(&observer);
  std::vector<Pass*> pipeline{&pass};
  REQUIRE(manager.run(function, pipeline));

  (void)cache.dominators();
  CHECK(cache.stats().dominatorsComputed == 1);
}

TEST_CASE("AnalysisCacheObserver forwards a changed pass's declared invalidates tags",
          "[decompile][analysis-cache-observer]") {
  Function function = makeFunction();
  AnalysisCache cache(function);
  (void)cache.dominators();
  (void)cache.stackFrame();
  REQUIRE(cache.stats().dominatorsComputed == 1);
  REQUIRE(cache.stats().stackFrameComputed == 1);

  OneShot pass(makeInfo("cfg-work", {"dominators"}), /*changed=*/true);
  AnalysisCacheObserver observer(cache);
  Manager manager(&observer);
  std::vector<Pass*> pipeline{&pass};
  REQUIRE(manager.run(function, pipeline));

  // "dominators" only: stackFrame is untouched.
  (void)cache.dominators();
  (void)cache.stackFrame();
  CHECK(cache.stats().dominatorsComputed == 2);
  CHECK(cache.stats().stackFrameComputed == 1);
}

TEST_CASE(
    "AnalysisCacheObserver drops everything when a changed pass declares no invalidates tags",
    "[decompile][analysis-cache-observer]") {
  Function function = makeFunction();
  AnalysisCache cache(function);
  (void)cache.dominators();
  (void)cache.stackFrame();
  REQUIRE(cache.stats().dominatorsComputed == 1);
  REQUIRE(cache.stats().stackFrameComputed == 1);

  OneShot pass(makeInfo("undeclared", {}), /*changed=*/true);
  AnalysisCacheObserver observer(cache);
  Manager manager(&observer);
  std::vector<Pass*> pipeline{&pass};
  REQUIRE(manager.run(function, pipeline));

  (void)cache.dominators();
  (void)cache.stackFrame();
  CHECK(cache.stats().dominatorsComputed == 2);
  CHECK(cache.stats().stackFrameComputed == 2);
}
