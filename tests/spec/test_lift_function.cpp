// liftFunction: recursive-descent discovery of a whole function.
//
// The programs under test are hand-assembled ARM64 fragments held in a map,
// so discovery edge cases — a target landing mid-block, a loop, an unresolved
// indirect branch — are exercised without needing a binary on disk.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <span>

#include "xdec/il/verify.h"
#include "xdec/spec/compile.h"
#include "xdec/spec/lift.h"

#include "../spec/spec_test_support.h"

using xdec::Result;
using xdec::spec::ByteReader;
using xdec::spec::LiftedFunction;

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

/// A flat memory of 32-bit instruction words; reads outside fail like an
/// unmapped address would.
class WordMemory {
 public:
  void put(uint64_t va, uint32_t word) { words_[va] = word; }

  [[nodiscard]] ByteReader reader() const {
    return [this](uint64_t va, std::span<std::byte> out) -> Result<void> {
      if (out.size() != 4) {
        return xdec::err(xdec::DiagCode::Internal, "test memory is 32-bit words");
      }
      const auto found = words_.find(va);
      if (found == words_.end()) {
        return xdec::err(xdec::DiagCode::BadFormat, "unmapped");
      }
      const uint32_t word = found->second;
      for (unsigned i = 0; i < 4; ++i) {
        out[i] = static_cast<std::byte>((word >> (i * 8)) & 0xff);
      }
      return xdec::ok();
    };
  }

 private:
  std::map<uint64_t, uint32_t> words_;
};

[[nodiscard]] LiftedFunction lift(const WordMemory& memory, uint64_t entry) {
  auto lifted = liftFunction(engine(), memory.reader(), entry);
  const std::string error = lifted ? std::string{} : lifted.error().format();
  INFO(error);
  REQUIRE(lifted);
  return std::move(*lifted);
}

[[nodiscard]] bool hasSuccessor(const xdec::il::Function& function,
                                const xdec::il::Block& block, uint64_t va) {
  for (const xdec::il::BlockId successor : block.successors) {
    if (function.block(successor).va == va) {
      return true;
    }
  }
  return false;
}

// Hand-assembled words (see C4/C6 of the ARM ARM):
constexpr uint32_t kMovX0_0 = 0xd2800000;   // mov x0, #0
constexpr uint32_t kMovX0_1 = 0xd2800020;   // mov x0, #1
constexpr uint32_t kMovX0_2 = 0xd2800040;   // mov x0, #2
constexpr uint32_t kMovX1_1 = 0xd2800021;   // mov x1, #1
constexpr uint32_t kCbzX0_pC = 0xb4000060;  // cbz x0, +12
constexpr uint32_t kB_p8 = 0x14000002;      // b +8
constexpr uint32_t kB_m4 = 0x17ffffff;      // b -4
constexpr uint32_t kRet = 0xd65f03c0;       // ret
constexpr uint32_t kBrX0 = 0xd61f0000;      // br x0

TEST_CASE("a diamond becomes three blocks with two successors at the fork",
          "[lift][function]") {
  WordMemory memory;
  memory.put(0x1000, kMovX0_0);
  memory.put(0x1004, kCbzX0_pC);  // 0x1004 + 12 = 0x1010
  memory.put(0x1008, kMovX0_1);
  memory.put(0x100c, kRet);
  memory.put(0x1010, kMovX0_2);
  memory.put(0x1014, kRet);

  const LiftedFunction lifted = lift(memory, 0x1000);
  const xdec::il::Function& function = *lifted.function;

  REQUIRE(function.blockCount() == 3);
  REQUIRE(function.block(function.entryBlock()).va == 0x1000);
  CHECK(lifted.unresolved.empty());

  const xdec::il::Block& fork = function.block(function.entryBlock());
  REQUIRE(fork.successors.size() == 2);
  CHECK(hasSuccessor(function, fork, 0x1008));
  CHECK(hasSuccessor(function, fork, 0x1010));

  const xdec::il::VerifyReport verified =
      xdec::il::verify(function, xdec::il::Maturity::Lifted);
  for (const xdec::Diag& diag : verified.errors) {
    INFO(diag.format());
  }
  CHECK(verified.ok());
}

TEST_CASE("a backward branch landing mid-block splits it on rescan", "[lift][function]") {
  WordMemory memory;
  memory.put(0x3000, kMovX0_0);
  memory.put(0x3004, kMovX1_1);
  memory.put(0x3008, kB_m4);  // back to 0x3004, inside the block first scanned

  const LiftedFunction lifted = lift(memory, 0x3000);
  const xdec::il::Function& function = *lifted.function;

  // 0x3004 was discovered strictly inside [0x3000, 0x300c): the coarse extent
  // must have been rescanned into [0x3000, 0x3004) + loop block [0x3004, 0x300c).
  REQUIRE(function.blockCount() == 2);

  const xdec::il::Block& head = function.block(function.entryBlock());
  CHECK(head.va == 0x3000);
  CHECK(head.endVa == 0x3004);
  REQUIRE(head.successors.size() == 1);

  const xdec::il::Block& loop = function.block(head.successors.front());
  CHECK(loop.va == 0x3004);
  CHECK(loop.endVa == 0x300c);
  REQUIRE(loop.successors.size() == 1);
  CHECK(function.block(loop.successors.front()).va == 0x3004);  // self-loop
}

TEST_CASE("an indirect branch ends its block unresolved, by design", "[lift][function]") {
  WordMemory memory;
  memory.put(0x4000, kMovX0_0);
  memory.put(0x4004, kBrX0);

  const LiftedFunction lifted = lift(memory, 0x4000);
  const xdec::il::Function& function = *lifted.function;

  REQUIRE(function.blockCount() == 1);
  REQUIRE(lifted.unresolved.size() == 1);
  CHECK(lifted.unresolved.front() == 0x4000);

  const xdec::il::Block& block = function.block(function.entryBlock());
  CHECK(block.successors.empty());
  CHECK(function.op(block.ops.back()).isTerminator());
}

TEST_CASE("a branch into the unmapped resolves to a stub block", "[lift][function]") {
  WordMemory memory;
  memory.put(0x5000, kB_p8);  // 0x5008, which is not in the map
  // 0x5004 intentionally absent.

  const LiftedFunction lifted = lift(memory, 0x5000);
  REQUIRE(lifted.external.size() == 1);
  CHECK(lifted.external.front() == 0x5008);
  // The real block plus the stub: the branch resolves to a real BlockId whose
  // address the caller can read back, same contract as liftBasicBlock.
  REQUIRE(lifted.function->blockCount() == 2);
  const xdec::il::Block& entryBlock = lifted.function->block(lifted.entry);
  REQUIRE(entryBlock.successors.size() == 1);
  const xdec::il::Block& stub = lifted.function->block(entryBlock.successors.front());
  CHECK(stub.va == 0x5008);
  CHECK(stub.empty());
}

}  // namespace
