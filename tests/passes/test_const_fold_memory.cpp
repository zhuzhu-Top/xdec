// const-fold-memory: reads of memory the program can never change become the
// constants they always yield. The tests run the whole pipeline to Ssa with a
// synthetic address space, because the point of the pass is what the passes
// after it can then see.
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "il/il_test_support.h"
#include "xdec/il/function.h"
#include "xdec/il/verify.h"
#include "xdec/pass/manager.h"
#include "xdec/pass/registry.h"
#include "xdec/passes/builtin.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::Maturity;
using xdec::il::OpCode;
using xdec::il::Type;

namespace {

/// A two-region address space: `0x9000` read-only, `0xa000` writable. Reads
/// succeed in both, so a test that folds the writable one is a test that folded
/// on readability rather than on immutability.
struct Space {
  static constexpr uint64_t kReadOnly = 0x9000;
  static constexpr uint64_t kWritable = 0xa000;
  static constexpr uint64_t kRegionSize = 0x100;

  Space() : bytes(2 * kRegionSize, std::byte{0}) {}

  void store64(uint64_t va, uint64_t value) {
    const std::size_t at = offsetOf(va);
    for (unsigned index = 0; index < 8; ++index) {
      bytes[at + index] = static_cast<std::byte>((value >> (index * 8)) & 0xff);
    }
  }

  [[nodiscard]] xdec::ByteReader reader() const {
    return [this](uint64_t va, std::span<std::byte> out) -> xdec::Result<void> {
      if (!mapped(va, out.size())) {
        return xdec::err(xdec::DiagCode::Internal, "unmapped read at {:#x}", va);
      }
      std::memcpy(out.data(), bytes.data() + offsetOf(va), out.size());
      return xdec::ok();
    };
  }

  [[nodiscard]] static xdec::MemoryFacts facts() {
    xdec::MemoryFacts facts;
    facts.immutable = [](uint64_t va, uint64_t size) {
      return va >= kReadOnly && va + size <= kReadOnly + kRegionSize;
    };
    return facts;
  }

  [[nodiscard]] static bool mapped(uint64_t va, uint64_t size) {
    const bool inReadOnly = va >= kReadOnly && va + size <= kReadOnly + kRegionSize;
    const bool inWritable = va >= kWritable && va + size <= kWritable + kRegionSize;
    return inReadOnly || inWritable;
  }

  [[nodiscard]] static std::size_t offsetOf(uint64_t va) {
    return static_cast<std::size_t>(va >= kWritable ? kRegionSize + (va - kWritable)
                                                    : va - kReadOnly);
  }

  std::vector<std::byte> bytes;
};

struct Builder {
  Builder() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {}

  BlockId block(uint64_t va) {
    const BlockId id = function.createBlock(va);
    if (!function.entryBlock().valid()) {
      function.setEntryBlock(id);
    }
    return id;
  }

  ExprId i64(uint64_t value) { return function.constant(Type::integer(64), value); }

  ExprId load(BlockId at, uint64_t va, ExprId address) {
    return function.valueRef(function.appendLoad(at, va, Type::integer(64), address));
  }

  void atCfg() {
    function.rebuildEdges();
    function.setMaturity(Maturity::Cfg);
  }

  Function function;
};

/// The stock pipeline to Ssa. `facts` decides whether immutability is wired at
/// all, which is the difference between the pass having something to do and
/// having nothing it is permitted to do.
void runToSsa(Function& function, const Space& space, bool facts = true) {
  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);
  xdec::pass::Manager manager;
  manager.setImage(space.reader());
  if (facts) {
    manager.setMemoryFacts(Space::facts());
  }
  auto ran = manager.runTo(function, registry, Maturity::Ssa);
  const std::string error = ran ? std::string{} : ran.error().format();
  INFO(error);
  REQUIRE(ran);
  const il::VerifyReport report = il::verify(function, Maturity::Ssa);
  for (const xdec::Diag& diag : report.errors) {
    INFO(diag.format());
  }
  REQUIRE(report.ok());
}

[[nodiscard]] std::size_t loadCount(const Function& function) {
  std::size_t count = 0;
  for (const BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      if (function.op(opId).code == OpCode::Load) {
        ++count;
      }
    }
  }
  return count;
}

/// The value the block's Store writes, as a constant.
[[nodiscard]] bool storedConstant(const Function& function, BlockId block, uint64_t& out) {
  for (const il::OpId opId : function.block(block).ops) {
    const il::Op& op = function.op(opId);
    if (op.code == OpCode::Store) {
      return function.asConstant(function.operands(op)[1], out);
    }
  }
  return false;
}

}  // namespace

//   entry: t = load(0x9000); store(0xa000, t + 1); ret
// Read-only memory holds 0x41, so the whole store operand is knowable: the
// load goes and the addition folds, which is the propagation this pass exists
// to unblock rather than the load removal itself.
TEST_CASE("a load of read-only memory becomes its constant, and the arithmetic above it folds",
          "[passes][const-fold-memory]") {
  Space space;
  space.store64(Space::kReadOnly, 0x41);

  Builder b;
  const BlockId entry = b.block(0x1000);
  const ExprId loaded = b.load(entry, 0x1000, b.i64(Space::kReadOnly));
  b.function.appendStore(entry, 0x1004, Type::integer(64), b.i64(Space::kWritable),
                         b.function.binary(ExprOp::Add, loaded, b.i64(1)));
  b.function.appendReturn(entry, 0x1008);
  b.atCfg();

  runToSsa(b.function, space);

  CHECK(loadCount(b.function) == 0);
  uint64_t stored = 0;
  REQUIRE(storedConstant(b.function, entry, stored));
  CHECK(stored == 0x42);
}

//   entry: t = load(0xa000); store(0xa080, t); ret
// The same shape over writable memory. Reading it succeeds, so anything that
// folds here folded on the wrong question.
TEST_CASE("a load of writable memory is left alone", "[passes][const-fold-memory]") {
  Space space;
  space.store64(Space::kWritable, 0x41);

  Builder b;
  const BlockId entry = b.block(0x1000);
  const ExprId loaded = b.load(entry, 0x1000, b.i64(Space::kWritable));
  b.function.appendStore(entry, 0x1004, Type::integer(64), b.i64(Space::kWritable + 0x80),
                         loaded);
  b.function.appendReturn(entry, 0x1008);
  b.atCfg();

  runToSsa(b.function, space);

  CHECK(loadCount(b.function) == 1);
}

// A pointer in read-only memory pointing at read-only memory: folding the
// outer load needs the inner one folded first, which is what the pass's own
// fixpoint is for.
TEST_CASE("a chain of read-only loads folds through", "[passes][const-fold-memory]") {
  Space space;
  space.store64(Space::kReadOnly, Space::kReadOnly + 0x40);
  space.store64(Space::kReadOnly + 0x40, 0x1234);

  Builder b;
  const BlockId entry = b.block(0x1000);
  const ExprId outer =
      b.load(entry, 0x1004, b.load(entry, 0x1000, b.i64(Space::kReadOnly)));
  b.function.appendStore(entry, 0x1008, Type::integer(64), b.i64(Space::kWritable), outer);
  b.function.appendReturn(entry, 0x100c);
  b.atCfg();

  runToSsa(b.function, space);

  CHECK(loadCount(b.function) == 0);
  uint64_t stored = 0;
  REQUIRE(storedConstant(b.function, entry, stored));
  CHECK(stored == 0x1234);
}

// Bytes with no immutability claim behind them are just bytes. A pipeline that
// wires the reader alone must fold nothing rather than trust the reader's
// answer to a question it was never asked.
TEST_CASE("without immutability facts nothing folds", "[passes][const-fold-memory]") {
  Space space;
  space.store64(Space::kReadOnly, 0x41);

  Builder b;
  const BlockId entry = b.block(0x1000);
  const ExprId loaded = b.load(entry, 0x1000, b.i64(Space::kReadOnly));
  b.function.appendStore(entry, 0x1004, Type::integer(64), b.i64(Space::kWritable), loaded);
  b.function.appendReturn(entry, 0x1008);
  b.atCfg();

  runToSsa(b.function, space, /*facts=*/false);

  CHECK(loadCount(b.function) == 1);
}

// An address that is not statically known stays a load however permissive the
// memory is: the pass folds reads of constant addresses, not reads it hopes are
// constant.
TEST_CASE("a computed address is not folded", "[passes][const-fold-memory]") {
  Space space;
  space.store64(Space::kReadOnly, 0x41);

  Builder b;
  const BlockId entry = b.block(0x1000);
  const ExprId dynamic = b.function.binary(
      ExprOp::Add, b.function.entryReg(b.function.registers().find("x0")),
      b.i64(Space::kReadOnly));
  const ExprId loaded = b.load(entry, 0x1000, dynamic);
  b.function.appendStore(entry, 0x1004, Type::integer(64), b.i64(Space::kWritable), loaded);
  b.function.appendReturn(entry, 0x1008);
  b.atCfg();

  runToSsa(b.function, space);

  CHECK(loadCount(b.function) == 1);
}
