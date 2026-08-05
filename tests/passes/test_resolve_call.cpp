// resolve-call: either the target is proved, or its shape is stated. The tests
// are built around that dividing line, because the ways to get it wrong are all
// on one side of it -- resolving a target that was never certain.
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
using xdec::il::OpId;
using xdec::il::Type;

namespace {

/// Three regions, so that every reason to decline is reachable: read-only code
/// (a legal target), read-only data (readable and immutable but not code), and
/// writable data (readable but promising nothing).
struct Space {
  static constexpr uint64_t kCode = 0x4000;
  static constexpr uint64_t kRodata = 0x9000;
  static constexpr uint64_t kData = 0xa000;
  static constexpr uint64_t kRegionSize = 0x100;
  /// Two slots the loader relocates, and writable, because that is what a
  /// relocated pointer slot is: one filled from within the image, one from
  /// another module.
  static constexpr uint64_t kRelocated = kData + 0x40;
  static constexpr uint64_t kImported = kData + 0x60;

  Space() : bytes(3 * kRegionSize, std::byte{0}) {}

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
      return within(kCode, va, size) || within(kRodata, va, size);
    };
    facts.executable = [](uint64_t va) { return within(kCode, va, 1); };
    facts.loader = [](uint64_t va) {
      xdec::LoaderValue value;
      if (va == kRelocated) {
        value.address = kCode + 0x48;
        value.hasAddress = true;
      } else if (va == kImported) {
        value.importName = "dlsym";
      }
      return value;
    };
    return facts;
  }

  [[nodiscard]] static bool within(uint64_t base, uint64_t va, uint64_t size) {
    return va >= base && va + size <= base + kRegionSize;
  }

  [[nodiscard]] static bool mapped(uint64_t va, uint64_t size) {
    return within(kCode, va, size) || within(kRodata, va, size) ||
           within(kData, va, size);
  }

  [[nodiscard]] static std::size_t offsetOf(uint64_t va) {
    if (va >= kData) {
      return static_cast<std::size_t>(2 * kRegionSize + (va - kData));
    }
    if (va >= kRodata) {
      return static_cast<std::size_t>(kRegionSize + (va - kRodata));
    }
    return static_cast<std::size_t>(va - kCode);
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

  ExprId entry(const char* name) {
    return function.entryReg(function.registers().find(name));
  }

  void atCfg() {
    function.rebuildEdges();
    function.setMaturity(Maturity::Cfg);
  }

  Function function;
};

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

/// The one call in the function.
[[nodiscard]] OpId theCall(const Function& function) {
  for (const BlockId blockId : function.blockHandles()) {
    for (const OpId opId : function.block(blockId).ops) {
      if (function.op(opId).code == OpCode::Call) {
        return opId;
      }
    }
  }
  return OpId::invalid();
}

/// The address the call goes to, when it goes to a constant one.
[[nodiscard]] bool calledAddress(const Function& function, uint64_t& out) {
  const OpId call = theCall(function);
  REQUIRE(call.valid());
  return function.asConstant(function.operands(function.op(call))[0], out);
}

[[nodiscard]] std::string noteOnCall(const Function& function) {
  const OpId call = theCall(function);
  REQUIRE(call.valid());
  return std::string{function.noteOn(call)};
}

/// Builds `call load(<address>)` and runs the pipeline. The load is what every
/// one of these cases has in common; what differs is where it reads from.
void callThroughPointer(Builder& b, const Space& space, ExprId address,
                        bool facts = true) {
  const BlockId entry = b.block(0x1000);
  b.function.appendCall(entry, 0x1000, b.load(entry, 0x1000, address),
                        Type::integer(64));
  b.function.appendReturn(entry, 0x1004);
  b.atCfg();
  runToSsa(b.function, space, facts);
}

/// A pointer fetched from one of two slots depending on a run-time condition.
///
/// This is the shape that separates this pass from const-fold-memory, which
/// folds loads of *constant* addresses and so already handles the single-slot
/// case before this pass ever sees it. Here the address is not constant, so
/// nothing folds it; what settles the target is that both slots are in memory
/// the program cannot write and both hold the same address, which is a question
/// only a value-set evaluation asks.
[[nodiscard]] ExprId eitherSlot(Builder& b, uint64_t first, uint64_t second) {
  return b.function.select(
      b.function.binary(ExprOp::CmpEq, b.entry("x0"), b.i64(0)), b.i64(first),
      b.i64(second));
}

}  // namespace

// A function pointer in read-only memory pointing into code is the one case
// where a target is a fact about the program rather than about one run of it.
TEST_CASE("a call whose target converges on one read-only code address resolves",
          "[passes][resolve-call]") {
  Space space;
  space.store64(Space::kRodata, Space::kCode + 0x20);
  space.store64(Space::kRodata + 0x40, Space::kCode + 0x20);

  Builder b;
  callThroughPointer(b, space, eitherSlot(b, Space::kRodata, Space::kRodata + 0x40));

  uint64_t target = 0;
  REQUIRE(calledAddress(b.function, target));
  CHECK(target == Space::kCode + 0x20);
  CHECK(noteOnCall(b.function) == "call target proved constant from read-only memory");
}

// Two read-only slots holding two different functions. Both are reachable, so
// neither is the target; a call op has one target and picking either would be
// inventing a call the program may never make.
TEST_CASE("a target that converges on two addresses is not resolved",
          "[passes][resolve-call]") {
  Space space;
  space.store64(Space::kRodata, Space::kCode + 0x20);
  space.store64(Space::kRodata + 0x40, Space::kCode + 0x60);

  Builder b;
  callThroughPointer(b, space, eitherSlot(b, Space::kRodata, Space::kRodata + 0x40));

  uint64_t target = 0;
  CHECK_FALSE(calledAddress(b.function, target));
}

// The same pointer in writable memory. The bytes in the file are its initial
// value at best; whatever the program wrote there before reaching this call is
// what actually gets called, and that is not in the image.
TEST_CASE("a call through a writable pointer is described, not resolved",
          "[passes][resolve-call]") {
  Space space;
  space.store64(Space::kData, Space::kCode + 0x20);

  Builder b;
  callThroughPointer(b, space, b.i64(Space::kData));

  uint64_t target = 0;
  CHECK_FALSE(calledAddress(b.function, target));
  CHECK(noteOnCall(b.function) ==
        "indirect call: target = load(0xa000); writable memory, so the target is not "
        "knowable from the image");
}

// The same writable slot, when a relocation says what goes in it. This is the
// commonest unresolvable call in a real library, and the relocation is the only
// place its target is written down -- so it gets named, with the reason it is
// still not a direct call.
TEST_CASE("a relocated pointer slot reports what the loader puts there",
          "[passes][resolve-call]") {
  Space space;

  Builder b;
  callThroughPointer(b, space, b.i64(Space::kRelocated));

  uint64_t target = 0;
  CHECK_FALSE(calledAddress(b.function, target));
  CHECK(noteOnCall(b.function) ==
        "indirect call: target = load(0xa040); the loader fills that slot with 0x4048 "
        "(writable, so it may have been replaced since)");
}

// A slot filled from another module has no address to give -- which module wins
// is a run-time question -- but the name is the more useful half anyway.
TEST_CASE("an imported pointer slot reports the symbol name",
          "[passes][resolve-call]") {
  Space space;

  Builder b;
  callThroughPointer(b, space, b.i64(Space::kImported));

  CHECK(noteOnCall(b.function) ==
        "indirect call: target = load(0xa060); the loader fills that slot from the "
        "imported symbol 'dlsym' (writable, so it may have been replaced since)");
}

// Immutable and readable, but the value read is not code. Resolving here would
// emit a call to the middle of a string table -- a confident answer that is
// wrong, where "unresolved" was merely unhelpful.
TEST_CASE("a read-only pointer to non-executable memory does not resolve",
          "[passes][resolve-call]") {
  Space space;
  space.store64(Space::kRodata, Space::kRodata + 0x80);
  space.store64(Space::kRodata + 0x40, Space::kRodata + 0x80);

  Builder b;
  callThroughPointer(b, space, eitherSlot(b, Space::kRodata, Space::kRodata + 0x40));

  uint64_t target = 0;
  CHECK_FALSE(calledAddress(b.function, target));
}

// No facts at all means no claim is permitted, even though the reader would
// happily serve the same bytes.
TEST_CASE("without memory facts nothing resolves", "[passes][resolve-call]") {
  Space space;
  space.store64(Space::kRodata, Space::kCode + 0x20);
  space.store64(Space::kRodata + 0x40, Space::kCode + 0x20);

  Builder b;
  callThroughPointer(b, space, eitherSlot(b, Space::kRodata, Space::kRodata + 0x40),
                     /*facts=*/false);

  uint64_t target = 0;
  CHECK_FALSE(calledAddress(b.function, target));
}

// The encrypted-table dispatch: `load(base + i*0x5d0 + j*8) ^ key` with a
// run-time base. Nothing about the target is knowable, and everything about the
// shape is -- both strides and the key.
TEST_CASE("an encrypted dispatch table is recognised and spelled out",
          "[passes][resolve-call]") {
  Space space;

  Builder b;
  const BlockId entry = b.block(0x1000);
  const ExprId index = b.function.binary(ExprOp::Mul, b.entry("x1"), b.i64(0x5d0));
  const ExprId slot = b.function.binary(ExprOp::Mul, b.entry("x2"), b.i64(8));
  const ExprId address = b.function.binary(
      ExprOp::Add, b.function.binary(ExprOp::Add, b.entry("x0"), index), slot);
  const ExprId decoded =
      b.function.binary(ExprOp::Xor, b.load(entry, 0x1000, address), b.i64(0xd2880));
  b.function.appendCall(entry, 0x1004, decoded, Type::integer(64));
  b.function.appendReturn(entry, 0x1008);
  b.atCfg();

  runToSsa(b.function, space);

  CHECK(noteOnCall(b.function) ==
        "indirect call: target = load(v + i*0x5d0 + j*0x8) ^ 0xd2880 "
        "(encrypted dispatch table)");
}

// A target assembled from registers alone reaches no memory, and saying so is
// worth as much as naming a table: there is no table to go looking for.
TEST_CASE("a target computed without a load says so", "[passes][resolve-call]") {
  Space space;

  Builder b;
  const BlockId entry = b.block(0x1000);
  b.function.appendCall(entry, 0x1000,
                        b.function.binary(ExprOp::Xor, b.entry("x0"), b.entry("x1")),
                        Type::integer(64));
  b.function.appendReturn(entry, 0x1004);
  b.atCfg();

  runToSsa(b.function, space);

  CHECK(noteOnCall(b.function) == "indirect call: target computed, not read from memory");
}

// A call the binary spells directly is not this pass's business: it has nothing
// to prove and nothing a reader does not already see.
TEST_CASE("a direct call gets no note", "[passes][resolve-call]") {
  Space space;

  Builder b;
  const BlockId entry = b.block(0x1000);
  b.function.appendCall(entry, 0x1000, b.i64(Space::kCode + 0x40), Type::integer(64));
  b.function.appendReturn(entry, 0x1004);
  b.atCfg();

  runToSsa(b.function, space);

  CHECK(noteOnCall(b.function).empty());
}
