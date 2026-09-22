// Instruction semantics summaries, checked against the encodings that a
// consumer replaying a recorded trace gets wrong when all it has is a mnemonic:
// the constant an `and` masks with, which direction a variable shift goes, the
// half a `movk` leaves alone. Each case asserts the rendered expression rather
// than a classification, because the expression is what the summary promises.
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "spec/spec_test_support.h"
#include "xdec/exec/exec_ir.h"
#include "xdec/exec/insn_semantics.h"
#include "xdec/exec/memory.h"
#include "xdec/spec/engine.h"

using namespace xdec;
using namespace xdec::exec;

namespace {

const spec::SpecEngine& engine() {
  static const std::unique_ptr<spec::SpecEngine> instance = [] {
    auto loaded = spec::loadSpecFile(spec::testing::arm64SpecPath());
    REQUIRE(loaded);
    return std::move(*loaded);
  }();
  return *instance;
}

std::string hex(uint64_t value) {
  if (value == 0) {
    return "0";
  }
  std::string text;
  while (value != 0) {
    text.insert(text.begin(), "0123456789abcdef"[value & 0xF]);
    value >>= 4;
  }
  return text;
}

/// Renders a node as a nested call, with register leaves by name and constants
/// in hex. Sharing in the graph is expanded back out: a summary small enough to
/// read is small enough to print twice.
std::string render(const InstructionSemantics& semantics, const il::RegisterFile& registers,
                   uint16_t index) {
  const SemanticNode& node = semantics.nodes[index];
  switch (node.op) {
    case il::ExprOp::Const:
      return "#" + hex(node.immediate);
    case il::ExprOp::Undef:
      return "?";
    case il::ExprOp::Value:
      if (leafKind(node.immediate) == LeafKind::Register) {
        return std::string(registers.nameOf(il::RegId{leafIndex(node.immediate)}));
      }
      return "load" + std::to_string(leafIndex(node.immediate));
    default:
      break;
  }
  std::string text(il::toString(node.op));
  text += '(';
  for (unsigned operand = 0; operand < node.operandCount; ++operand) {
    if (operand != 0) {
      text += ", ";
    }
    text += render(semantics, registers, node.operands[operand]);
  }
  if (node.op == il::ExprOp::Extract || node.op == il::ExprOp::ZExt ||
      node.op == il::ExprOp::SExt || node.op == il::ExprOp::Trunc) {
    text += ":i" + std::to_string(node.width);
  }
  if (node.op == il::ExprOp::Extract) {
    text += "@" + std::to_string(node.immediate);
  }
  text += ')';
  return text;
}

struct Summary {
  std::string disassembly;
  std::vector<std::string> effects;
  bool complete = false;
};

/// Compiles one instruction on its own and summarises it. A block stops at a
/// terminator, so a non-branch needs a `ret` after it to keep the block from
/// running off the end of the seeded page.
Summary summarize(uint32_t word) {
  GuestAddressSpace memory;
  std::vector<std::byte> bytes;
  for (const uint32_t each : {word, 0xD65F03C0u}) {  // ret
    for (unsigned shift = 0; shift < 32; shift += 8) {
      bytes.push_back(static_cast<std::byte>(each >> shift));
    }
  }
  REQUIRE(memory.seed(0x1000, bytes,
                      MemoryPermission::Read | MemoryPermission::Execute));
  auto compiled = compileExecBlock(engine(), memory, 0x1000);
  REQUIRE(compiled);
  const ExecInstruction* instruction = (*compiled)->instructionAt(0x1000);
  REQUIRE(instruction != nullptr);

  const il::RegisterFile& registers = (*compiled)->lifted.function->registers();
  Summary out;
  out.disassembly = instruction->disassembly;
  out.complete = instruction->semantics.complete;
  for (const SemanticEffect& effect : instruction->semantics.effects) {
    if (effect.kind == SemanticEffect::Kind::WriteReg) {
      out.effects.push_back(std::string(registers.nameOf(effect.reg)) + " = " +
                            render(instruction->semantics, registers, effect.valueNode));
    } else {
      out.effects.push_back(
          "[" + render(instruction->semantics, registers, effect.addressNode) +
          "]:i" + std::to_string(effect.width) + " = " +
          render(instruction->semantics, registers, effect.valueNode));
    }
  }
  return out;
}

}  // namespace

TEST_CASE("semantics: shifted register operand carries the shift", "[exec][semantics]") {
  const Summary summary = summarize(0x0B080528);
  CHECK(summary.disassembly == "add w8, w9, w8, lsl #1");
  CHECK(summary.complete);
  REQUIRE(summary.effects.size() == 1);
  CHECK(summary.effects[0] == "w8 = add(w9, shl(w8, #1))");
}

TEST_CASE("semantics: a variable shift names its direction", "[exec][semantics]") {
  const Summary summary = summarize(0x1ACE25AE);
  CHECK(summary.disassembly == "lsr w14, w13, w14");
  CHECK(summary.complete);
  REQUIRE(summary.effects.size() == 1);
  CHECK(summary.effects[0] == "w14 = shr.u(w13, and(w14, #1f))");
}

TEST_CASE("semantics: a logical immediate is decoded, not left in the encoding",
          "[exec][semantics]") {
  const Summary summary = summarize(0x92610109);
  CHECK(summary.disassembly == "and x9, x8, #0x80000000");
  CHECK(summary.complete);
  REQUIRE(summary.effects.size() == 1);
  CHECK(summary.effects[0] == "x9 = and(x8, #80000000)");
}

TEST_CASE("semantics: movk keeps the half it does not write", "[exec][semantics]") {
  const Summary summary = summarize(0x72BC33E9);
  CHECK(summary.disassembly == "movk w9, #0xe19f, lsl #16");
  CHECK(summary.complete);
  REQUIRE(summary.effects.size() == 1);
  CHECK(summary.effects[0] == "w9 = or(and(w9, #ffff), #e19f0000)");
}

// `neg` is `sub` from the zero register, and a consumer reading the trace's
// register rows sees one source where the mnemonic's two operands suggest two.
// The summary says negation outright, so there is nothing to infer.
//
// The shift by zero is not folded away. Nothing here folds: an identity the
// evaluator collapses for free is not worth a second home for algebraic rules,
// and a summary that rewrites what the lifter said is one more thing that can
// be wrong.
TEST_CASE("semantics: neg is a negation, not a subtract from a register",
          "[exec][semantics]") {
  const Summary summary = summarize(0x4B0803E8);
  CHECK(summary.disassembly == "neg w8, w8");
  CHECK(summary.complete);
  REQUIRE(summary.effects.size() == 1);
  CHECK(summary.effects[0] == "w8 = neg(shl(w8, #0))");
}

// Spelled out as the rotate-and-mask the architecture defines sbfm to be. This
// is the case that makes the whole approach worth it: classifying it would mean
// recognising this idiom as "sign-extend byte" and hoping the recognition and
// the spec never disagree, where evaluating it just produces the right answer.
TEST_CASE("semantics: sxtb is the bitfield the architecture defines it as",
          "[exec][semantics]") {
  const Summary summary = summarize(0x13001D8B);
  CHECK(summary.disassembly == "sxtb w11, w12");
  CHECK(summary.complete);
  REQUIRE(summary.effects.size() == 1);
  CHECK(summary.effects[0] ==
        "w11 = or(and(sext(extract(w12:i1@7):i32), #ffffff00), "
        "and(and(rotr(w12, #0), #ff), #ff))");
}

// The extend reads the whole 64-bit register and narrows, which is why the leaf
// is x11 where the disassembly says w11. A consumer pairing leaves with the
// trace's register rows needs that to be the register the trace recorded, and
// it is: the read comes from the same lifted op the trace captured.
TEST_CASE("semantics: an extended register operand carries the extension",
          "[exec][semantics]") {
  const Summary summary = summarize(0x4B2B818B);
  CHECK(summary.disassembly == "sub w11, w12, w11, sxtb");
  CHECK(summary.complete);
  REQUIRE(summary.effects.size() == 1);
  CHECK(summary.effects[0] == "w11 = sub(w12, shl(sext(trunc(x11:i8):i32), #0))");
}

TEST_CASE("semantics: a load names the access its value came from",
          "[exec][semantics]") {
  // ldr w8, [x9, #4]
  const Summary summary = summarize(0xB9400528);
  CHECK(summary.disassembly == "ldr w8, [x9, #0x4]");
  CHECK(summary.complete);
  REQUIRE(summary.effects.size() == 1);
  CHECK(summary.effects[0] == "w8 = load0");
}

// A system-register read is a value the IL declines to invent, so the write
// still appears -- the register really is written -- but its value is an
// unknown and the summary stops claiming to be complete. A consumer checking a
// recorded execution against the summary has to skip it rather than report the
// disagreement it would otherwise find.
TEST_CASE("semantics: an unmodelled value is an unknown, not a guess",
          "[exec][semantics]") {
  const Summary summary = summarize(0xD53BD040);
  CHECK(summary.disassembly == "mrs x0, tpidr_el0");
  CHECK_FALSE(summary.complete);
  REQUIRE(summary.effects.size() == 1);
  CHECK(summary.effects[0] == "x0 = ?");
}
