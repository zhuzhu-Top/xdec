// The decompilation driver, end to end: a function whose only indirect
// branch hides behind a global pointer. The lifter cannot follow it, the
// pipeline resolves it, the driver lifts what it points at — one round each.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>

#include "xdec/decompile/driver.h"
#include "xdec/il/function.h"
#include "xdec/passes/builtin.h"
#include "xdec/spec/compile.h"

#include "../spec/spec_test_support.h"

namespace il = xdec::il;
using xdec::ByteReader;
using xdec::Result;

namespace {

const xdec::spec::SpecEngine& engine() {
  static const std::unique_ptr<xdec::spec::SpecEngine> kEngine = [] {
    auto loaded = xdec::spec::loadSpecFile(xdec::spec::testing::arm64SpecPath());
    if (!loaded) {
      FAIL(loaded.error().format());
    }
    return std::move(loaded).value();
  }();
  return *kEngine;
}

/// Instruction words plus table qwords; reads of anything else are unmapped.
class FlatMemory {
 public:
  void putInsn(uint64_t va, uint32_t word) { insns_[va] = word; }
  void putQword(uint64_t va, uint64_t value) { qwords_[va] = value; }

  [[nodiscard]] ByteReader reader() const {
    return [this](uint64_t va, std::span<std::byte> out) -> Result<void> {
      if (out.size() == 4) {
        if (const auto found = insns_.find(va); found != insns_.end()) {
          for (unsigned i = 0; i < 4; ++i) {
            out[i] = static_cast<std::byte>((found->second >> (i * 8)) & 0xff);
          }
          return xdec::ok();
        }
      }
      if (out.size() <= 8) {
        if (const auto found = qwords_.find(va); found != qwords_.end()) {
          for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = static_cast<std::byte>((found->second >> (i * 8)) & 0xff);
          }
          return xdec::ok();
        }
      }
      return xdec::err(xdec::DiagCode::BadFormat, "unmapped");
    };
  }

 private:
  std::map<uint64_t, uint32_t> insns_;
  std::map<uint64_t, uint64_t> qwords_;
};

/// The program under test: `x8 = *(0x2000); br x8`, with the table slot
/// holding `target`. At 0x1010 sits a lone `ret` the lifter cannot reach.
FlatMemory program(uint64_t target) {
  FlatMemory memory;
  memory.putInsn(0x1000, 0xB0000008);  // adrp x8, #0x2000 (from 0x1000)
  memory.putInsn(0x1004, 0xF9400108);  // ldr x8, [x8]
  memory.putInsn(0x1008, 0xD61F0100);  // br x8
  memory.putInsn(0x1010, 0xD65F03C0);  // ret
  memory.putQword(0x2000, target);
  return memory;
}

TEST_CASE("the driver discovers through an indirect branch and resolves it",
          "[decompile][driver]") {
  const FlatMemory memory = program(0x1010);
  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);

  xdec::decompile::DriverOptions options;
  auto result = xdec::decompile::decompile(engine(), memory.reader(), 0x1000, registry,
                                           options);
  const std::string error = result ? std::string{} : result.error().format();
  INFO(error);
  REQUIRE(result);

  // Two, and the second is not idle: round one discovers 0x1010 and proves the
  // edge that leads there in one go (the candidate set rides along with the
  // discovery -- see pass::Discovery), so round two is already the one that
  // lifts the block *and* builds SSA over a CFG containing that edge. A
  // pipeline that stopped at one would analyse the discovered block as
  // unreachable code (see driver.h).
  CHECK(result->report.rounds == 2);
  CHECK(result->report.converged);
  REQUIRE(result->report.extraEntries.size() == 1);
  CHECK(result->report.extraEntries[0] == 0x1010);

  const il::Function& function = *result->function;
  CHECK(function.maturity() == il::Maturity::Resolved);
  const il::BlockId target = function.blockAt(0x1010);
  REQUIRE(target.valid());
  const il::Block& source = function.block(function.blockAt(0x1000));
  const il::Op& brind = function.op(source.ops.back());
  // Resolved to exactly one target, so fold-resolved-branch turns it into a
  // plain Branch -- see tests/passes/test_fold_resolved_branch.cpp for that
  // pass in isolation.
  REQUIRE(brind.code == il::OpCode::Branch);
  const auto targets = function.targets(brind);
  REQUIRE(targets.size() == 1);
  CHECK(targets[0] == target);
}

// A round budget too small to reach the fixpoint is the everyday case on an
// obfuscated function, where each round widens a dispatcher by a few targets.
// The rounds spent are worth keeping either way, so the cap emits rather than
// fails, and only marks the result as possibly incomplete.
TEST_CASE("a run the round cap cuts short still yields what it proved",
          "[decompile][driver]") {
  const FlatMemory memory = program(0x1010);
  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);

  xdec::decompile::DriverOptions options;
  options.maxRounds = 1;  // the loop needs two (above); it gets one
  // A wall, not a budget -- which is what a caller bounding its own runtime
  // asks for, and what the CLI passes when --rounds is given explicitly.
  options.extendWhileProving = false;
  auto result = xdec::decompile::decompile(engine(), memory.reader(), 0x1000, registry,
                                           options);
  const std::string error = result ? std::string{} : result.error().format();
  INFO(error);
  REQUIRE(result);

  CHECK(result->report.rounds == 1);
  CHECK_FALSE(result->report.converged);
  // What the one round did learn is in the result: the block behind the branch.
  CHECK(result->function->blockAt(0x1010).valid());
}

// The budget is a guess at how deep the function goes, and being one level out
// is the ordinary way to be wrong. A round still proving edges when the budget
// runs out buys another one, so the guess costs coverage only past the ceiling.
TEST_CASE("a budget one round short of the fixpoint extends itself",
          "[decompile][driver]") {
  const FlatMemory memory = program(0x1010);
  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);

  xdec::decompile::DriverOptions options;
  options.maxRounds = 1;  // same budget as above, without the wall
  auto result = xdec::decompile::decompile(engine(), memory.reader(), 0x1000, registry,
                                           options);
  const std::string error = result ? std::string{} : result.error().format();
  INFO(error);
  REQUIRE(result);

  CHECK(result->report.rounds == 2);
  CHECK(result->report.converged);
  const il::BlockId target = result->function->blockAt(0x1010);
  REQUIRE(target.valid());
  const il::Block& source = result->function->block(result->function->blockAt(0x1000));
  CHECK(result->function->op(source.ops.back()).code == il::OpCode::Branch);
}

// The shape the discovery-size warnings exist to name, made small enough to
// assert on directly: two indirect branches that are both reachable without
// any discovery at all (no fence needed -- see DriverOptions::maxTotalEntries
// for why the cap is unconditional rather than fence-gated), each naming a
// fresh address in the very first round. A real dispatcher chain never hands
// over two brand-new entries at once like this; an entry that is not really a
// function start is exactly the shape that would.
TEST_CASE("a run capped mid-round on total entries still finishes",
          "[decompile][driver]") {
  FlatMemory memory;
  memory.putInsn(0x1000, 0xB40000C0);  // cbz x0, #0x1018
  memory.putInsn(0x1004, 0xB0000008);  // adrp x8, #0x2000 (from 0x1004's page)
  memory.putInsn(0x1008, 0xF9400108);  // ldr x8, [x8]
  memory.putInsn(0x100c, 0xD61F0100);  // br x8
  memory.putInsn(0x1018, 0xD0000009);  // adrp x9, #0x3000 (from 0x1018's page)
  memory.putInsn(0x101c, 0xF9400129);  // ldr x9, [x9]
  memory.putInsn(0x1020, 0xD61F0120);  // br x9
  memory.putQword(0x2000, 0x5000);
  memory.putQword(0x3000, 0x5010);
  memory.putInsn(0x5000, 0xD65F03C0);  // ret
  memory.putInsn(0x5010, 0xD65F03C0);  // ret

  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);

  xdec::decompile::DriverOptions options;
  // `known` starts at {0x1000}; this admits exactly one of the round's two
  // simultaneous discoveries before the wall bites, deterministically (the
  // set the driver walks them in is address-ordered) admitting the lower one.
  options.maxTotalEntries = 2;
  // The other branch's target is now permanently uncapped-for, so it never
  // resolves -- sealing is what turns that into a hole in the output instead
  // of the honest failure tests/decompile/test_driver.cpp's own "pointing
  // nowhere" case checks for.
  options.sealUnresolvedBranches = true;
  auto result = xdec::decompile::decompile(engine(), memory.reader(), 0x1000, registry,
                                           options);
  const std::string error = result ? std::string{} : result.error().format();
  INFO(error);
  REQUIRE(result);

  REQUIRE(result->report.extraEntries.size() == 1);
  CHECK(result->report.extraEntries[0] == 0x5000);
  CHECK(result->function->blockAt(0x5000).valid());
  CHECK_FALSE(result->function->blockAt(0x5010).valid());
}

TEST_CASE("a table slot pointing nowhere stays unresolved and fails honestly",
          "[decompile][driver]") {
  const FlatMemory memory = program(0x9999);  // unmapped
  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);

  xdec::decompile::DriverOptions options;
  auto result = xdec::decompile::decompile(engine(), memory.reader(), 0x1000, registry,
                                           options);
  REQUIRE(!result);
  CHECK(result.error().format().find("unresolved") != std::string::npos);
}

}  // namespace
