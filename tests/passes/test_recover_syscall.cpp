// recover-syscall: which syscall is behind an `svc`, and how many of x0..x5 it
// reads.
//
// These lift real ARM64 words rather than building the intrinsic by hand, so
// the spec's operand layout (specs/arm64/system.xspec) is under test too: if
// the ABI ever stops being named at lift time, the recovery stops working and
// these cases are what says so.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>

#include "xdec/il/verify.h"
#include "xdec/pass/manager.h"
#include "xdec/pass/registry.h"
#include "xdec/passes/builtin.h"
#include "xdec/passes/recover_syscall.h"
#include "xdec/spec/lift.h"
#include "xdec/types/syscall_table.h"

#include "../spec/spec_test_support.h"

namespace il = xdec::il;
using xdec::Result;
using xdec::il::Function;
using xdec::il::Maturity;
using xdec::il::OpId;
using xdec::types::SyscallTable;

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

const SyscallTable& table() {
  static const SyscallTable kTable = [] {
    const Result<std::string> path = SyscallTable::resolvePath(SyscallTable::defaultName());
    if (!path) {
      FAIL(path.error().format());
    }
    Result<SyscallTable> loaded = SyscallTable::loadFile(*path);
    if (!loaded) {
      FAIL(loaded.error().format());
    }
    return std::move(*loaded);
  }();
  return kTable;
}

class WordMemory {
 public:
  void put(uint64_t va, uint32_t word) { words_[va] = word; }

  [[nodiscard]] xdec::spec::ByteReader reader() const {
    return [this](uint64_t va, std::span<std::byte> out) -> Result<void> {
      const auto found = words_.find(va);
      if (found == words_.end() || out.size() != 4) {
        return xdec::err(xdec::DiagCode::BadFormat, "unmapped");
      }
      for (unsigned i = 0; i < 4; ++i) {
        out[i] = static_cast<std::byte>((found->second >> (i * 8)) & 0xff);
      }
      return xdec::ok();
    };
  }

 private:
  std::map<uint64_t, uint32_t> words_;
};

/// `movz x<rd>, #imm16` — the usual way a syscall number reaches x8.
[[nodiscard]] constexpr uint32_t movz64(unsigned rd, uint16_t imm) {
  return 0xd2800000U | (static_cast<uint32_t>(imm) << 5U) | rd;
}
/// `movz w<rd>, #imm16` — the 32-bit form, which leaves the value zero-extended.
[[nodiscard]] constexpr uint32_t movz32(unsigned rd, uint16_t imm) {
  return 0x52800000U | (static_cast<uint32_t>(imm) << 5U) | rd;
}
/// `mov x<rd>, x<rm>`, encoded as `orr xd, xzr, xm`.
[[nodiscard]] constexpr uint32_t movReg(unsigned rd, unsigned rm) {
  return 0xaa0003e0U | (rm << 16U) | rd;
}
constexpr uint32_t kSvc0 = 0xd4000001U;
constexpr uint32_t kRet = 0xd65f03c0U;

struct Lifted {
  std::unique_ptr<Function> function;
  OpId syscall;
};

/// Lifts a straight-line program at 0x1000 and runs the stock pipeline with (or
/// without) a syscall table wired up, then finds the one svc intrinsic.
[[nodiscard]] Lifted run(const WordMemory& memory, const SyscallTable* syscalls) {
  auto lifted = xdec::spec::liftFunction(engine(), memory.reader(), 0x1000);
  const std::string liftError = lifted ? std::string{} : lifted.error().format();
  INFO(liftError);
  REQUIRE(lifted);

  auto function = std::move(lifted->function);
  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);
  xdec::pass::Manager manager;
  manager.setImage([](uint64_t, std::span<std::byte>) -> Result<void> {
    return xdec::err(xdec::DiagCode::Internal, "no image in this test");
  });
  manager.setSyscallTable(syscalls);
  auto ran = manager.runTo(*function, registry, Maturity::Vars);
  const std::string runError = ran ? std::string{} : ran.error().format();
  INFO(runError);
  REQUIRE(ran);

  const il::VerifyReport report = il::verify(*function, Maturity::Vars);
  for (const xdec::Diag& diag : report.errors) {
    INFO(diag.format());
  }
  REQUIRE(report.ok());

  Lifted out;
  const uint32_t name = function->internName(xdec::passes::kSyscallIntrinsic);
  for (const il::BlockId blockId : function->blockHandles()) {
    for (const OpId opId : function->block(blockId).ops) {
      const il::Op& op = function->op(opId);
      if (op.code == il::OpCode::Intrinsic && op.payload == name) {
        out.syscall = opId;
      }
    }
  }
  out.function = std::move(function);
  return out;
}

/// Argument operands still attached, past `imm16` and the syscall number.
[[nodiscard]] std::size_t argCount(const Lifted& lifted) {
  REQUIRE(lifted.syscall.valid());
  const auto operands = lifted.function->operands(lifted.function->op(lifted.syscall));
  REQUIRE(operands.size() >= xdec::passes::kSyscallFirstArgOperand);
  return operands.size() - xdec::passes::kSyscallFirstArgOperand;
}

[[nodiscard]] std::string noteOf(const Lifted& lifted) {
  REQUIRE(lifted.syscall.valid());
  return std::string{lifted.function->noteOn(lifted.syscall)};
}

}  // namespace

// The base case, and the one that proves the lift carries the ABI at all:
// `mov x8, #64; svc #0` is a three-argument write, so x3..x5 come off.
TEST_CASE("a constant syscall number names the call and trims to its arity",
          "[passes][recover-syscall]") {
  WordMemory memory;
  memory.put(0x1000, movz64(8, 64));
  memory.put(0x1004, kSvc0);
  memory.put(0x1008, kRet);

  const Lifted lifted = run(memory, &table());
  CHECK(argCount(lifted) == 3);
  CHECK(noteOf(lifted) == "syscall write (64)");
}

// `mov w8, #169` writes the low half, so the intrinsic reads a zero-extension
// of a 32-bit constant. That is the form a compiler emits for small numbers,
// and refusing to look through it would leave most real syscalls unrecovered.
TEST_CASE("a 32-bit move of the number is seen through", "[passes][recover-syscall]") {
  WordMemory memory;
  memory.put(0x1000, movz32(8, 169));
  memory.put(0x1004, kSvc0);
  memory.put(0x1008, kRet);

  const Lifted lifted = run(memory, &table());
  CHECK(noteOf(lifted) == "syscall gettimeofday (169)");
  CHECK(argCount(lifted) == 2);
}

// The number arriving through a register copy. Nothing here scans backwards:
// copy propagation ran first, so by the time this pass looks, the operand is
// the constant.
TEST_CASE("a number copied through another register still resolves",
          "[passes][recover-syscall]") {
  WordMemory memory;
  memory.put(0x1000, movz64(3, 172));
  memory.put(0x1004, movReg(8, 3));
  memory.put(0x1008, kSvc0);
  memory.put(0x100c, kRet);

  const Lifted lifted = run(memory, &table());
  CHECK(noteOf(lifted) == "syscall getpid (172)");
  // getpid takes nothing, so every argument register comes off.
  CHECK(argCount(lifted) == 0);
}

// A number the kernel does not define. The table's silence is about the table,
// so the arguments stay and the note says which number went unmatched.
TEST_CASE("an unknown syscall number is reported, not guessed",
          "[passes][recover-syscall]") {
  WordMemory memory;
  memory.put(0x1000, movz64(8, 9999));
  memory.put(0x1004, kSvc0);
  memory.put(0x1008, kRet);

  const Lifted lifted = run(memory, &table());
  CHECK(argCount(lifted) == 6);
  CHECK(noteOf(lifted) == "syscall 9999 is not in the 'aarch64' table");
}

// x8 is whatever this function's caller left in it: a thunk. Nothing can be
// trimmed, because with an unknown callee any register might be an argument.
TEST_CASE("a non-constant syscall number keeps every argument",
          "[passes][recover-syscall]") {
  WordMemory memory;
  memory.put(0x1000, kSvc0);
  memory.put(0x1004, kRet);

  const Lifted lifted = run(memory, &table());
  CHECK(argCount(lifted) == 6);
  CHECK(noteOf(lifted) ==
        "syscall number (x8) is not a constant here; arguments are the raw x0-x5 "
        "registers");
}

// No table wired up is the default pipeline, and it must not change what the
// output says beyond recording the number it could see.
TEST_CASE("without a syscall table the number is still recorded",
          "[passes][recover-syscall]") {
  WordMemory memory;
  memory.put(0x1000, movz64(8, 64));
  memory.put(0x1004, kSvc0);
  memory.put(0x1008, kRet);

  const Lifted lifted = run(memory, nullptr);
  CHECK(argCount(lifted) == 6);
  CHECK(noteOf(lifted) == "syscall 64 (no syscall table loaded)");
}
