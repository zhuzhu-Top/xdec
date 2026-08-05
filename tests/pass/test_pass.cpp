// The pass framework: registration, pipeline resolution, managed execution.
//
// The passes under test are toys on purpose. What is being checked is the
// framework's contract — ordering, convergence, verification gates,
// provenance — and a toy that appends one Nop exercises exactly that, with no
// real transformation logic to hide a framework bug behind.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include "il/il_test_support.h"
#include "xdec/il/function.h"
#include "xdec/pass/manager.h"
#include "xdec/pass/registry.h"

using xdec::Arch;
using xdec::il::BlockId;
using xdec::il::Function;
using xdec::il::Maturity;
using xdec::pass::Context;
using xdec::pass::FunctionPass;
using xdec::pass::Manager;
using xdec::pass::Observer;
using xdec::pass::Pass;
using xdec::pass::PassInfo;
using xdec::pass::Registry;
using xdec::pass::RunStats;
using xdec::Result;

namespace {

[[nodiscard]] PassInfo makeInfo(std::string name, Maturity level, Maturity produces,
                            std::vector<std::string> requirements = {},
                            bool fixpoint = false) {
  PassInfo out;
  out.name = std::move(name);
  out.level = level;
  out.produces = produces;
  out.requirements = std::move(requirements);
  out.fixpoint = fixpoint;
  return out;
}

/// Changes nothing. The backbone of a pipeline walk.
class NoopTransform : public FunctionPass {
 public:
  using FunctionPass::FunctionPass;
  Result<bool> run(Context&) override { return false; }
};

/// Appends one Nop to the entry block per run, up to a budget; afterwards
/// reports unchanged. Drives fixpoint scheduling and change statistics.
class NopAppend : public FunctionPass {
 public:
  NopAppend(PassInfo info, unsigned budget) : FunctionPass(std::move(info)), budget_(budget) {}

  Result<bool> run(Context& context) override {
    if (runs_++ >= budget_) {
      return false;
    }
    Function& function = context.function();
    function.appendNop(function.entryBlock(), 0x2000 + runs_);
    return true;
  }

 private:
  unsigned budget_;
  unsigned runs_ = 0;
};

/// Finishes the CFG honestly: return terminator plus rebuilt edges.
class TerminateCfg : public FunctionPass {
 public:
  using FunctionPass::FunctionPass;

  Result<bool> run(Context& context) override {
    Function& function = context.function();
    const xdec::il::OpId last = function.block(function.entryBlock()).terminator();
    if (last.valid() && function.op(last).isTerminator()) {
      return false;
    }
    function.appendReturn(function.entryBlock(), 0x2ffc);
    function.rebuildEdges();
    return true;
  }
};

/// Builds a correct-looking CFG but skips rebuildEdges(), so the stored edges
/// disagree with the terminators. The manager's verification gate is the test
/// target here.
class DishonestCfg : public FunctionPass {
 public:
  using FunctionPass::FunctionPass;

  Result<bool> run(Context& context) override {
    Function& function = context.function();
    const BlockId extra = function.createBlock(0x3000);
    function.appendBranch(function.entryBlock(), 0x2ff8, extra);
    function.appendReturn(extra, 0x3000);
    return true;
  }
};

[[nodiscard]] Function makeFunction() {
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId entry = function.createBlock(0x1000);
  function.setEntryBlock(entry);
  const xdec::il::RegId x0 = function.registers().find("x0");
  const xdec::il::ValueId value = function.appendReadReg(entry, 0x1000, x0);
  function.appendWriteReg(entry, 0x1004, x0, function.valueRef(value));
  return function;
}

[[nodiscard]] std::vector<std::string> namesOf(std::span<Pass* const> pipeline) {
  std::vector<std::string> names;
  for (const Pass* pass : pipeline) {
    names.push_back(pass->info().name);
  }
  return names;
}

}  // namespace

TEST_CASE("registration rejects invalid passes", "[pass]") {
  Registry registry;

  SECTION("duplicate names") {
    REQUIRE(registry.add(std::make_unique<NoopTransform>(
        makeInfo("cleanup", Maturity::Lifted, Maturity::Local))));
    const auto duplicate = registry.add(std::make_unique<NoopTransform>(
        makeInfo("cleanup", Maturity::Local, Maturity::Local)));
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error().format().find("duplicate") != std::string::npos);
  }

  SECTION("an empty name") {
    const auto result =
        registry.add(std::make_unique<NoopTransform>(makeInfo("", Maturity::Lifted, Maturity::Local)));
    REQUIRE_FALSE(result);
  }

  SECTION("produces below level") {
    const auto result = registry.add(std::make_unique<NoopTransform>(
        makeInfo("time-travel", Maturity::Cfg, Maturity::Lifted)));
    REQUIRE_FALSE(result);
  }
}

TEST_CASE("resolve orders a pipeline", "[pass]") {
  Registry registry;
  // Registered deliberately out of order: the resolver, not the registration
  // sequence, decides the pipeline.
  REQUIRE(registry.add(std::make_unique<NoopTransform>(
      makeInfo("ssa-build", Maturity::Cfg, Maturity::Ssa, {"local-simplify"}))));
  REQUIRE(registry.add(std::make_unique<NoopTransform>(
      makeInfo("cfg-build", Maturity::Local, Maturity::Cfg, {"cleanup"}))));
  REQUIRE(registry.add(std::make_unique<NoopTransform>(
      makeInfo("cleanup", Maturity::Lifted, Maturity::Local))));
  REQUIRE(registry.add(std::make_unique<NoopTransform>(
      makeInfo("local-simplify", Maturity::Local, Maturity::Local))));

  const auto pipeline = registry.resolve(Maturity::Lifted, Maturity::Ssa);
  const std::string pipelineError = pipeline ? std::string{} : pipeline.error().format();
  INFO(pipelineError);
  REQUIRE(pipeline);
  CHECK(namesOf(*pipeline) ==
        std::vector<std::string>{"cleanup", "local-simplify", "cfg-build", "ssa-build"});
}

TEST_CASE("resolve reports configuration errors by name", "[pass]") {
  SECTION("unknown requirement") {
    Registry registry;
    REQUIRE(registry.add(std::make_unique<NoopTransform>(
        makeInfo("ssa-build", Maturity::Cfg, Maturity::Ssa, {"does-not-exist"}))));
    const auto pipeline = registry.resolve(Maturity::Lifted, Maturity::Ssa);
    REQUIRE_FALSE(pipeline);
    CHECK(pipeline.error().format().find("does-not-exist") != std::string::npos);
  }

  SECTION("requirement cycle") {
    Registry registry;
    REQUIRE(registry.add(std::make_unique<NoopTransform>(
        makeInfo("alpha", Maturity::Local, Maturity::Local, {"beta"}))));
    REQUIRE(registry.add(std::make_unique<NoopTransform>(
        makeInfo("beta", Maturity::Local, Maturity::Local, {"alpha"}))));
    const auto pipeline = registry.resolve(Maturity::Local, Maturity::Local);
    REQUIRE_FALSE(pipeline);
    CHECK(pipeline.error().format().find("cycle") != std::string::npos);
  }

  SECTION("a gap in the maturity walk") {
    Registry registry;
    REQUIRE(registry.add(std::make_unique<NoopTransform>(
        makeInfo("cleanup", Maturity::Lifted, Maturity::Local))));
    REQUIRE(registry.add(std::make_unique<NoopTransform>(
        makeInfo("ssa-build", Maturity::Cfg, Maturity::Ssa))));
    const auto pipeline = registry.resolve(Maturity::Lifted, Maturity::Ssa);
    REQUIRE_FALSE(pipeline);
    CHECK(pipeline.error().format().find("gap") != std::string::npos);
  }

  SECTION("a requirement already satisfied by the starting maturity") {
    Registry registry;
    // Ran at Lifted; when the walk starts at Local it is already behind us.
    REQUIRE(registry.add(std::make_unique<NoopTransform>(
        makeInfo("cleanup", Maturity::Lifted, Maturity::Local))));
    REQUIRE(registry.add(std::make_unique<NoopTransform>(
        makeInfo("cfg-build", Maturity::Local, Maturity::Cfg, {"cleanup"}))));
    const auto pipeline = registry.resolve(Maturity::Local, Maturity::Cfg);
    REQUIRE(pipeline);
    CHECK(namesOf(*pipeline) == std::vector<std::string>{"cfg-build"});
  }
}

TEST_CASE("the manager runs a pipeline and advances maturity", "[pass]") {
  Registry registry;
  REQUIRE(registry.add(std::make_unique<NoopTransform>(
      makeInfo("cleanup", Maturity::Lifted, Maturity::Local))));
  REQUIRE(registry.add(std::make_unique<TerminateCfg>(
      makeInfo("cfg-build", Maturity::Local, Maturity::Cfg))));
  REQUIRE(registry.add(std::make_unique<NoopTransform>(
      makeInfo("ssa-build", Maturity::Cfg, Maturity::Ssa))));

  Function function = makeFunction();
  Manager manager;
  const auto stats = manager.runTo(function, registry, Maturity::Ssa);
  const std::string statsError = stats ? std::string{} : stats.error().format();
  INFO(statsError);
  REQUIRE(stats);
  CHECK(function.maturity() == Maturity::Ssa);
  REQUIRE(stats->size() == 3);
  CHECK(stats->at(0).passName == "cleanup");
  CHECK_FALSE(stats->at(0).changed);
  CHECK(stats->at(1).changed);  // cfg-build appended the return
  CHECK(function.op(function.block(function.entryBlock()).terminator()).isTerminator());
}

TEST_CASE("the manager loops a fixpoint pass to convergence", "[pass]") {
  Registry registry;
  REQUIRE(registry.add(
      std::make_unique<NopAppend>(makeInfo("nop-fill", Maturity::Lifted, Maturity::Lifted,
                                       {}, /*fixpoint=*/true),
                                  /*budget=*/3)));

  Function function = makeFunction();
  Manager manager;
  const auto stats = manager.runTo(function, registry, Maturity::Lifted);
  REQUIRE(stats);
  REQUIRE(stats->size() == 1);
  CHECK(stats->front().iterations == 4);  // 3 changes + 1 run that saw no change
  CHECK(stats->front().changed);
  CHECK(function.block(function.entryBlock()).ops.size() == 2 + 3);
}

TEST_CASE("the manager reports a fixpoint pass that does not converge", "[pass]") {
  Registry registry;
  REQUIRE(registry.add(
      std::make_unique<NopAppend>(makeInfo("oscillator", Maturity::Lifted, Maturity::Lifted,
                                       {}, /*fixpoint=*/true),
                                  /*budget=*/1000)));

  Function function = makeFunction();
  Manager manager;
  const auto stats = manager.runTo(function, registry, Maturity::Lifted);
  REQUIRE_FALSE(stats);
  CHECK(stats.error().format().find("oscillator") != std::string::npos);
  CHECK(stats.error().format().find("converge") != std::string::npos);
}

TEST_CASE("the verification gate names the dishonest pass", "[pass]") {
  Registry registry;
  REQUIRE(registry.add(std::make_unique<NoopTransform>(
      makeInfo("cleanup", Maturity::Lifted, Maturity::Local))));
  REQUIRE(registry.add(std::make_unique<DishonestCfg>(
      makeInfo("dishonest-cfg", Maturity::Local, Maturity::Cfg))));

  Function function = makeFunction();
  Manager manager;
  const auto stats = manager.runTo(function, registry, Maturity::Cfg);
  REQUIRE_FALSE(stats);
  CHECK(stats.error().format().find("dishonest-cfg") != std::string::npos);
  // The function must not have been advanced past the failure.
  CHECK(function.maturity() == Maturity::Local);
}

TEST_CASE("a hand-assembled pipeline is still checked", "[pass]") {
  NoopTransform cfgLevel(makeInfo("cfg-work", Maturity::Cfg, Maturity::Cfg));
  Function function = makeFunction();
  Manager manager;
  std::vector<Pass*> pipeline{&cfgLevel};
  const auto stats = manager.run(function, pipeline);
  REQUIRE_FALSE(stats);
  CHECK(stats.error().format().find("cfg-work") != std::string::npos);
}

TEST_CASE("ops appended by a pass carry its name", "[pass]") {
  Registry registry;
  REQUIRE(registry.add(std::make_unique<NopAppend>(
      makeInfo("nop-append", Maturity::Lifted, Maturity::Lifted), /*budget=*/1)));

  Function function = makeFunction();
  Manager manager;
  REQUIRE(manager.runTo(function, registry, Maturity::Lifted));

  const BlockId entry = function.entryBlock();
  const xdec::il::OpId last = function.block(entry).ops.back();
  CHECK(function.passName(function.op(last).origin) == "nop-append");
}

TEST_CASE("the observer sees the pipeline in order", "[pass]") {
  struct Recorder : Observer {
    std::vector<std::string> events;

    void beforePass(const Pass& pass, const Function&) override {
      events.push_back("before:" + pass.info().name);
    }
    void afterPass(const Pass& pass, const Function&, const RunStats&) override {
      events.push_back("after:" + pass.info().name);
    }
    void pipelineDone(const Function&) override { events.push_back("done"); }
  } recorder;

  Registry registry;
  REQUIRE(registry.add(std::make_unique<NoopTransform>(
      makeInfo("cleanup", Maturity::Lifted, Maturity::Local))));
  REQUIRE(registry.add(std::make_unique<TerminateCfg>(
      makeInfo("cfg-build", Maturity::Local, Maturity::Cfg))));

  Function function = makeFunction();
  Manager manager(&recorder);
  REQUIRE(manager.runTo(function, registry, Maturity::Cfg));
  CHECK(recorder.events ==
        std::vector<std::string>{"before:cleanup", "after:cleanup", "before:cfg-build",
                                 "after:cfg-build", "done"});
}
