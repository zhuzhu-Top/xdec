// recover-tailcall: which `br xN` leaves the function, and which one stays in it.
//
// Every case here lifts real ARM64 words and runs the stock pipeline, because
// the claim under test is about evidence the earlier stages produce -- the entry
// leaf copy propagation exposes, the argument versions SSA construction
// records, the loader's account of a slot. Assembling the branch by hand would
// test the assembly instead.
//
// The negative cases stop at Ssa on purpose: what they assert is that the
// branch is *still* an indirect branch, which is exactly the state
// resolve-indirect exists to consume.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>

#include "xdec/il/printer.h"
#include "xdec/il/verify.h"
#include "xdec/pass/manager.h"
#include "xdec/pass/registry.h"
#include "xdec/passes/builtin.h"
#include "xdec/spec/lift.h"

#include "../spec/spec_test_support.h"

namespace il = xdec::il;
using xdec::Result;
using xdec::il::Function;
using xdec::il::Maturity;
using xdec::il::OpCode;
using xdec::il::OpId;

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

/// Code at 0x1000 and a data page at 0x9000, in one byte map so that a read of
/// any size is answered the way an image answers it. Both matter: the pass asks
/// whether a constant is an address in this image, and "is it readable" is how
/// it asks.
class Space {
 public:
  static constexpr uint64_t kCode = 0x1000;
  static constexpr uint64_t kSlot = 0x9000;

  void word(uint64_t va, uint32_t value) { write(va, value, 4); }
  void slot(uint64_t va, uint64_t value) { write(va, value, 8); }

  [[nodiscard]] xdec::ByteReader reader() const {
    return [this](uint64_t va, std::span<std::byte> out) -> Result<void> {
      for (std::size_t index = 0; index < out.size(); ++index) {
        const auto found = bytes_.find(va + index);
        if (found == bytes_.end()) {
          return xdec::err(xdec::DiagCode::BadFormat, "unmapped read at {:#x}", va);
        }
        out[index] = found->second;
      }
      return xdec::ok();
    };
  }

  /// Code is executable; nothing is immutable (a slot a loader fills is not),
  /// and one slot is bound to another module the way a PLT's is.
  [[nodiscard]] static xdec::MemoryFacts facts(bool boundSlot) {
    xdec::MemoryFacts facts;
    facts.executable = [](uint64_t va) { return va >= kCode && va < kCode + 0x100; };
    if (boundSlot) {
      facts.loader = [](uint64_t va) {
        xdec::LoaderValue value;
        if (va == kSlot) {
          value.importName = "dlsym";
        }
        return value;
      };
    }
    return facts;
  }

 private:
  void write(uint64_t va, uint64_t value, unsigned size) {
    for (unsigned index = 0; index < size; ++index) {
      bytes_[va + index] = static_cast<std::byte>((value >> (index * 8)) & 0xff);
    }
  }

  std::map<uint64_t, std::byte> bytes_;
};

// The instruction words, each verified against `xdec decode`.
[[nodiscard]] constexpr uint32_t br(unsigned rn) { return 0xd61f0000U | (rn << 5U); }
/// `mov x<rd>, x<rm>`, encoded as `orr xd, xzr, xm`.
[[nodiscard]] constexpr uint32_t movReg(unsigned rd, unsigned rm) {
  return 0xaa0003e0U | (rm << 16U) | rd;
}
/// `mov x<rd>, #imm16`.
[[nodiscard]] constexpr uint32_t movImm(unsigned rd, uint16_t imm) {
  return 0xd2800000U | (static_cast<uint32_t>(imm) << 5U) | rd;
}
/// `ldr x<rt>, [x<rn>]`.
[[nodiscard]] constexpr uint32_t ldr(unsigned rt, unsigned rn) {
  return 0xf9400000U | (rn << 5U) | rt;
}
/// `ldr x<rt>, [x<rn>, x<rm>, lsl #3]` -- an array of pointers, indexed.
[[nodiscard]] constexpr uint32_t ldrScaled(unsigned rt, unsigned rn, unsigned rm) {
  return 0xf8607800U | (rm << 16U) | (rn << 5U) | rt;
}
/// `add x<rd>, x<rn>, x<rm>, lsl #2` -- a jump table's base plus a scaled index.
[[nodiscard]] constexpr uint32_t addScaled(unsigned rd, unsigned rn, unsigned rm) {
  return 0x8b000000U | (rm << 16U) | (2U << 10U) | (rn << 5U) | rd;
}
/// `csel x<rd>, x<rn>, x<rm>, eq`.
[[nodiscard]] constexpr uint32_t cselEq(unsigned rd, unsigned rn, unsigned rm) {
  return 0x9a800000U | (rm << 16U) | (rn << 5U) | rd;
}
/// `cmp x<rn>, #0`, encoded as `subs xzr, xn, #0`.
[[nodiscard]] constexpr uint32_t cmpZero(unsigned rn) { return 0xf100001fU | (rn << 5U); }

struct Ran {
  std::unique_ptr<Function> function;
  il::BlockId block;
  /// The last two ops of the entry block, which is where the exit is.
  OpId last;
  OpId beforeLast;
};

/// Lifts at 0x1000 and runs the pipeline to `level`, then hands back the entry
/// block's tail so a test can say what the exit turned into.
[[nodiscard]] Ran run(const Space& space, Maturity level, bool boundSlot = false) {
  auto lifted = xdec::spec::liftFunction(engine(), space.reader(), Space::kCode);
  const std::string liftError = lifted ? std::string{} : lifted.error().format();
  INFO(liftError);
  REQUIRE(lifted);

  auto function = std::move(lifted->function);
  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);
  xdec::pass::Manager manager;
  manager.setImage(space.reader());
  manager.setMemoryFacts(Space::facts(boundSlot));
  auto result = manager.runTo(*function, registry, level);
  const std::string runError = result ? std::string{} : result.error().format();
  INFO(runError);
  REQUIRE(result);

  const il::VerifyReport report = il::verify(*function, level);
  for (const xdec::Diag& diag : report.errors) {
    INFO(diag.format());
  }
  REQUIRE(report.ok());

  Ran out;
  out.block = function->entryBlock();
  const auto& ops = function->block(out.block).ops;
  REQUIRE(!ops.empty());
  out.last = ops.back();
  out.beforeLast = ops.size() >= 2 ? ops[ops.size() - 2] : OpId::invalid();
  out.function = std::move(function);
  return out;
}

/// The call a rewritten tail call left behind, with its note and arguments.
struct TailCall {
  OpId call;
  std::size_t args = 0;
  std::string note;
  std::string target;
};

[[nodiscard]] TailCall expectTailCall(const Ran& ran) {
  const il::Op& last = ran.function->op(ran.last);
  const std::string exit = il::printOp(*ran.function, ran.last);
  INFO("block exit: " << exit);
  REQUIRE(last.code == OpCode::Return);
  REQUIRE(ran.beforeLast.valid());
  const il::Op& call = ran.function->op(ran.beforeLast);
  const std::string before = il::printOp(*ran.function, ran.beforeLast);
  INFO("op before it: " << before);
  REQUIRE(call.code == OpCode::Call);

  const auto operands = ran.function->operands(call);
  TailCall out;
  out.call = ran.beforeLast;
  out.args = operands.size() - 1;
  out.note = std::string{ran.function->noteOn(ran.beforeLast)};
  out.target = il::printExpr(*ran.function, operands[0]);
  // The return hands back what the call produced: that is what makes the
  // rewrite a tail call rather than a call whose result is dropped.
  const auto returned = ran.function->operands(last);
  REQUIRE(returned.size() == 1);
  CHECK(returned[0] == ran.function->valueRef(ran.function->resultOf(out.call)));
  return out;
}

/// The branch a declined case left alone, with the block that computes it: the
/// destination is an expression in some cases and a loaded value in others, so
/// the whole block is what a test can ask about either way.
[[nodiscard]] std::string expectIndirectBranch(const Ran& ran) {
  const il::Op& last = ran.function->op(ran.last);
  const std::string exit = il::printOp(*ran.function, ran.last);
  INFO("block exit: " << exit);
  REQUIRE(last.code == OpCode::IndirectBranch);
  std::string text;
  for (const OpId opId : ran.function->block(ran.block).ops) {
    text += il::printOp(*ran.function, opId);
    text += "\n";
  }
  return text;
}

}  // namespace

// The base case: nothing in this function says where x0 points, so the branch
// through it goes somewhere this function is not.
TEST_CASE("a branch through an argument register becomes a call and a return",
          "[passes][tailcall]") {
  Space space;
  space.word(0x1000, br(0));

  const Ran ran = run(space, Maturity::Ssa);
  const TailCall tail = expectTailCall(ran);
  CHECK(tail.target == "entry:i64(x0)");
  // resolve-call adds its own account of the target after this one, so the note
  // is a history rather than a single sentence.
  CHECK(tail.note.starts_with("tail call through a pointer the caller passed in"));
  // The ABI snapshot: with no prototype every argument register might be one.
  CHECK(tail.args == 8);
}

// Why the snapshot has to be taken at SSA construction. The `mov` that puts x2
// in x1 is dead by the time anything can tell this is a call -- its only reader
// is the callee -- so if the argument versions were not recorded before the
// optimiser ran, the call would be emitted with the wrong ones.
TEST_CASE("the argument shuffle before a tail call is what it passes",
          "[passes][tailcall]") {
  Space space;
  space.word(0x1000, movReg(1, 2));
  space.word(0x1004, br(0));

  const Ran ran = run(space, Maturity::Ssa);
  const TailCall tail = expectTailCall(ran);
  REQUIRE(tail.args == 8);
  const auto operands = ran.function->operands(ran.function->op(tail.call));
  // Operand 0 is the target, so x1 is operand 2.
  CHECK(il::printExpr(*ran.function, operands[2]) == "entry:i64(x2)");
}

// A pointer read out of an array the caller passed. The destination is still
// the caller's to know: this image says nothing about what is in that array.
TEST_CASE("a branch through a pointer loaded from a caller's array is a tail call",
          "[passes][tailcall]") {
  Space space;
  space.word(0x1000, ldrScaled(0, 0, 1));
  space.word(0x1004, br(0));

  const Ran ran = run(space, Maturity::Ssa);
  const TailCall tail = expectTailCall(ran);
  CHECK(tail.note.starts_with("tail call through a pointer the caller passed in"));
}

// The PLT's shape. The loader's account of the slot is the whole evidence, and
// it is conclusive: a slot bound to another module holds no address this image
// chose, so no table in it can be enumerated.
TEST_CASE("a branch through a bound import slot is a tail call to the import",
          "[passes][tailcall]") {
  Space space;
  space.word(0x1000, movImm(9, Space::kSlot));
  space.word(0x1004, ldr(8, 9));
  space.word(0x1008, br(8));
  space.slot(Space::kSlot, 0x1000);

  const Ran ran = run(space, Maturity::Ssa, /*boundSlot=*/true);
  const TailCall tail = expectTailCall(ran);
  CHECK(tail.note.starts_with("tail call through the 'dlsym' import slot"));
}

// The case that decides whether this pass can be trusted at all: a dispatcher
// whose table is in this image and whose index is a parameter mentions that
// parameter, and calling it a tail call would replace real control flow with a
// call that never happens.
TEST_CASE("a computed jump anchored in this image is left for resolution",
          "[passes][tailcall]") {
  Space space;
  space.word(0x1000, movImm(9, Space::kSlot));
  space.word(0x1004, addScaled(8, 9, 1));
  space.word(0x1008, br(8));
  space.slot(Space::kSlot, 0x1000);

  const Ran ran = run(space, Maturity::Ssa);
  const std::string block = expectIndirectBranch(ran);
  INFO("block: " << block);
  // The parameter is in there, as the index; the base is this image's.
  CHECK(block.find("entry:i64(x1)") != std::string::npos);
  CHECK(block.find("0x9000") != std::string::npos);
}

// `fn = cond ? f : g; return fn(a, b)` in the source, `csel` + `br` in the
// binary. Both arms are addresses right here, and the parameter that picks
// between them says nothing about where either points.
TEST_CASE("a select between two addresses in this image is not a tail call",
          "[passes][tailcall]") {
  Space space;
  space.word(0x1000, movImm(9, Space::kCode));
  space.word(0x1004, movImm(10, Space::kCode + 4));
  space.word(0x1008, cmpZero(0));
  space.word(0x100c, cselEq(8, 9, 10));
  space.word(0x1010, br(8));

  const Ran ran = run(space, Maturity::Ssa);
  const std::string block = expectIndirectBranch(ran);
  INFO("block: " << block);
  CHECK(block.find("select") != std::string::npos);
}

// A code pointer out of this function's own frame is not a pointer the caller
// handed over, whatever the caller stored there. The frame is this function's,
// so the branch is this function's problem.
TEST_CASE("a branch through a pointer loaded from the frame is not a tail call",
          "[passes][tailcall]") {
  Space space;
  space.word(0x1000, ldr(8, 31));  // ldr x8, [sp]
  space.word(0x1004, br(8));

  const Ran ran = run(space, Maturity::Ssa);
  const std::string block = expectIndirectBranch(ran);
  INFO("block: " << block);
  CHECK(block.find("entry:i64(sp)") != std::string::npos);
}

// End to end, past the level this pass produces: a rewritten tail call has to
// survive resolution, argument recovery and verification like any other call,
// or the rewrite has only moved the failure.
TEST_CASE("a recovered tail call carries through to Vars", "[passes][tailcall]") {
  Space space;
  space.word(0x1000, movReg(1, 2));
  space.word(0x1004, br(0));

  const Ran ran = run(space, Maturity::Vars);
  const TailCall tail = expectTailCall(ran);
  CHECK(tail.args == 8);
}
