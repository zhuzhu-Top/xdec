// DumpObserver: per-pass dumps, op censuses, and the pipeline index.
//
// The passes are the same toys as the framework tests — what is under test
// here is the observer's file output, not transformation logic.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "il/il_test_support.h"
#include "xdec/il/function.h"
#include "xdec/pass/manager.h"
#include "xdec/pass/observe.h"
#include "xdec/pass/registry.h"

using xdec::Arch;
using xdec::il::BlockId;
using xdec::il::Function;
using xdec::il::Maturity;
using xdec::pass::Context;
using xdec::pass::DumpObserver;
using xdec::pass::FunctionPass;
using xdec::pass::Manager;
using xdec::pass::PassInfo;
using xdec::pass::Registry;
using xdec::Result;

namespace {

[[nodiscard]] PassInfo makeInfo(std::string name, Maturity level, Maturity produces,
                                bool fixpoint = false) {
  PassInfo out;
  out.name = std::move(name);
  out.level = level;
  out.produces = produces;
  out.fixpoint = fixpoint;
  return out;
}

class NoopTransform : public FunctionPass {
 public:
  using FunctionPass::FunctionPass;
  Result<bool> run(Context&) override { return false; }
};

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

[[nodiscard]] Function makeFunction() {
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId entry = function.createBlock(0x1000);
  function.setEntryBlock(entry);
  const xdec::il::RegId x0 = function.registers().find("x0");
  const xdec::il::ValueId value = function.appendReadReg(entry, 0x1000, x0);
  function.appendWriteReg(entry, 0x1004, x0, function.valueRef(value));
  return function;
}

[[nodiscard]] std::filesystem::path freshDir(std::string_view name) {
  const auto dir = std::filesystem::temp_directory_path() /
                   std::filesystem::path(std::string{"xdec-observe-"} + std::string{name});
  std::filesystem::remove_all(dir);
  return dir;
}

[[nodiscard]] std::string slurp(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

TEST_CASE("observer dumps each pass state with an op census and an index", "[observe]") {
  const std::filesystem::path dir = freshDir("walk");

  Registry registry;
  const auto walk = registry.add(std::make_unique<NoopTransform>(
      makeInfo("walk-local", Maturity::Lifted, Maturity::Local)));
  const auto nops = registry.add(std::make_unique<NopAppend>(
      makeInfo("nop-append", Maturity::Local, Maturity::Local, true), 2));
  const auto cfg = registry.add(
      std::make_unique<TerminateCfg>(makeInfo("terminate", Maturity::Local, Maturity::Cfg)));
  REQUIRE(walk);
  REQUIRE(nops);
  REQUIRE(cfg);

  Function function = makeFunction();
  const std::size_t initialOps = function.opCount();

  {
    DumpObserver observer(dir);
    Manager manager(&observer);
    auto ran = manager.runTo(function, registry, Maturity::Cfg);
    const std::string error = ran ? std::string{} : ran.error().format();
    INFO(error);
    REQUIRE(ran);
    REQUIRE_FALSE(observer.failure());
  }

  // The pre-pipeline state is numbered 00 and named after its maturity.
  REQUIRE(std::filesystem::exists(dir / "00-lifted.il"));
  REQUIRE(std::filesystem::exists(dir / "00-lifted.map"));

  // Every pass got a numbered dump pair, in pipeline order.
  for (const char* stem : {"01-walk-local", "02-nop-append", "03-terminate"}) {
    INFO(stem);
    REQUIRE(std::filesystem::exists(dir / (std::string{stem} + ".il")));
    REQUIRE(std::filesystem::exists(dir / (std::string{stem} + ".map")));
  }

  // Census diff answers "which ops did the pass create": nop-append ran its
  // budget of two, and provenance names it on the new ops.
  const std::string before = slurp(dir / "01-walk-local.map");
  const std::string after = slurp(dir / "02-nop-append.map");
  REQUIRE(before.find("from nop-append") == std::string::npos);
  REQUIRE(after.find("from nop-append") != std::string::npos);
  const auto countLines = [](const std::string& text) {
    return static_cast<unsigned>(
        std::count(text.begin(), text.end(), '\n'));
  };
  REQUIRE(countLines(after) == countLines(before) + 2);

  // The index records the level walk and op counts, one line per pass.
  const std::string index = slurp(dir / "index.txt");
  REQUIRE(index.find("01 walk-local lifted->local") != std::string::npos);
  // A fixpoint pass reports every iteration: two appends plus the run that
  // saw nothing left to do.
  REQUIRE(index.find("02 nop-append local->local iter=3 changed=true") != std::string::npos);
  REQUIRE(index.find("03 terminate local->cfg") != std::string::npos);
  REQUIRE(index.find(std::format("ops={}->", initialOps)) != std::string::npos);
}

TEST_CASE("observer reports a broken output directory without failing the pipeline",
          "[observe]") {
  const std::filesystem::path blocker = freshDir("blocked");
  {
    std::ofstream file(blocker);  // a file where the directory must go
    REQUIRE(file.good());
  }

  Registry registry;
  REQUIRE(registry.add(
      std::make_unique<NoopTransform>(makeInfo("walk-local", Maturity::Lifted, Maturity::Local))));

  Function function = makeFunction();
  DumpObserver observer(blocker / "inside");
  Manager manager(&observer);
  auto ran = manager.runTo(function, registry, Maturity::Local);
  const std::string error = ran ? std::string{} : ran.error().format();
  INFO(error);
  REQUIRE(ran);  // dumping must never fail the run
  REQUIRE(observer.failure());

  std::filesystem::remove_all(blocker);
}

}  // namespace
