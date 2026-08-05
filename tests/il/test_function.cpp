#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "il/il_test_support.h"
#include "xdec/il/function.h"
#include "xdec/il/printer.h"

using namespace xdec::il;
using xdec::test::arm64Registers;

namespace {

Function makeFunction() { return Function{xdec::Arch::AArch64, arm64Registers(), 0x1000}; }

RegId reg(const char* name) {
  const RegId id = arm64Registers().find(name);
  REQUIRE(id.valid());
  return id;
}

/// A diamond: entry branches on a flag, the two arms rejoin.
struct Diamond {
  BlockId entry;
  BlockId ifTrue;
  BlockId ifFalse;
  BlockId join;
};

Diamond buildDiamond(Function& function) {
  Diamond blocks;
  blocks.entry = function.createBlock(0x1000);
  blocks.ifTrue = function.createBlock(0x1010);
  blocks.ifFalse = function.createBlock(0x1020);
  blocks.join = function.createBlock(0x1030);

  const ValueId lhs = function.appendReadReg(blocks.entry, 0x1000, reg("x0"));
  const ExprId zero = function.constant(Type::integer(64), 0);
  const ExprId operands[] = {function.valueRef(lhs), zero};
  const ExprId flags = function.flagDef(FlagOp::Sub, 64, operands);
  const ExprId condition = function.flagCondition(flags, ConditionCode::Equal);
  function.appendCondBranch(blocks.entry, 0x1004, condition, blocks.ifTrue, blocks.ifFalse);

  function.appendWriteReg(blocks.ifTrue, 0x1010, reg("x1"),
                          function.constant(Type::integer(64), 1));
  function.appendBranch(blocks.ifTrue, 0x1014, blocks.join);

  function.appendWriteReg(blocks.ifFalse, 0x1020, reg("x1"),
                          function.constant(Type::integer(64), 2));
  function.appendBranch(blocks.ifFalse, 0x1024, blocks.join);

  function.appendReturn(blocks.join, 0x1030);
  function.rebuildEdges();
  return blocks;
}

}  // namespace

TEST_CASE("a fresh function knows the lifter as pass zero", "[il][function]") {
  const Function function = makeFunction();
  CHECK(function.arch() == xdec::Arch::AArch64);
  CHECK(function.entryVa() == 0x1000);
  CHECK(function.maturity() == Maturity::Lifted);
  CHECK(function.passCount() == 1);
  CHECK(function.passName(kPassLifter) == "lift");
  CHECK(function.blockCount() == 0);
}

TEST_CASE("register reads define values, writes consume them", "[il][function]") {
  Function function = makeFunction();
  const BlockId block = function.createBlock(0x1000);

  const ValueId source = function.appendReadReg(block, 0x1000, reg("x0"));
  REQUIRE(function.hasValue(source));
  const ValueInfo& info = function.value(source);
  CHECK(info.type == Type::integer(64));
  CHECK(info.block == block);
  CHECK(function.op(info.definition).code == OpCode::ReadReg);
  CHECK(function.op(info.definition).va == 0x1000);

  const ExprId sum = function.binary(ExprOp::Add, function.valueRef(source),
                                     function.constant(Type::integer(64), 8));
  const OpId write = function.appendWriteReg(block, 0x1000, reg("x1"), sum);
  CHECK(function.operands(function.op(write)).size() == 1);
  CHECK(function.operands(function.op(write))[0] == sum);
  CHECK_FALSE(function.op(write).result.valid());

  SECTION("a sub-register read reports the narrower type") {
    const ValueId narrow = function.appendReadReg(block, 0x1004, reg("w0"));
    CHECK(function.value(narrow).type == Type::integer(32));
  }

  SECTION("a flags register read is typed flags, not an integer") {
    const ValueId flags = function.appendReadReg(block, 0x1004, reg("nzcv"));
    CHECK(function.value(flags).type == Type::flags());
  }

  SECTION("the entry block is the first one created") {
    CHECK(function.entryBlock() == block);
  }
}

TEST_CASE("a void intrinsic defines nothing and a typed one defines a value",
          "[il][function]") {
  Function function = makeFunction();
  const BlockId block = function.createBlock(0x1000);
  const ExprId argument = function.constant(Type::integer(64), 0x20);
  const ExprId arguments[] = {argument};

  const OpId barrier = function.appendIntrinsic(block, 0x1000, "aarch64.dmb", Type::voidType(),
                                                arguments);
  CHECK_FALSE(function.op(barrier).result.valid());

  const OpId read = function.appendIntrinsic(block, 0x1004, "aarch64.mrs", Type::integer(64),
                                             arguments);
  REQUIRE(function.op(read).result.valid());
  CHECK(function.value(function.op(read).result).type == Type::integer(64));
}

TEST_CASE("edges are derived from terminators", "[il][function][cfg]") {
  Function function = makeFunction();
  const Diamond blocks = buildDiamond(function);

  CHECK(function.block(blocks.entry).successors ==
        std::vector<BlockId>{blocks.ifTrue, blocks.ifFalse});
  CHECK(function.block(blocks.entry).predecessors.empty());
  CHECK(function.block(blocks.join).predecessors ==
        std::vector<BlockId>{blocks.ifTrue, blocks.ifFalse});
  CHECK(function.block(blocks.join).successors.empty());

  SECTION("rebuilding twice is idempotent") {
    const std::vector<BlockId> before = function.block(blocks.join).predecessors;
    function.rebuildEdges();
    CHECK(function.block(blocks.join).predecessors == before);
  }

  SECTION("a conditional branch to one block keeps both edges") {
    // Duplicate edges are not noise: a phi has one operand per predecessor
    // entry, so collapsing them would misalign the operands.
    Function collapsed = makeFunction();
    const BlockId entry = collapsed.createBlock(0x2000);
    const BlockId target = collapsed.createBlock(0x2010);
    collapsed.appendCondBranch(entry, 0x2000, collapsed.boolean(true), target, target);
    collapsed.appendReturn(target, 0x2010);
    collapsed.rebuildEdges();
    CHECK(collapsed.block(entry).successors.size() == 2);
    CHECK(collapsed.block(target).predecessors == std::vector<BlockId>{entry, entry});
  }

  SECTION("reverse post-order visits a block before its successors") {
    const std::vector<BlockId> order = function.reversePostOrder();
    REQUIRE(order.size() == 4);
    const auto positionOf = [&order](BlockId id) {
      return std::distance(order.begin(), std::find(order.begin(), order.end(), id));
    };
    CHECK(positionOf(blocks.entry) == 0);
    CHECK(positionOf(blocks.entry) < positionOf(blocks.ifTrue));
    CHECK(positionOf(blocks.entry) < positionOf(blocks.ifFalse));
    CHECK(positionOf(blocks.ifTrue) < positionOf(blocks.join));
    CHECK(positionOf(blocks.ifFalse) < positionOf(blocks.join));
  }
}

TEST_CASE("reverse post-order terminates on cyclic and irreducible graphs",
          "[il][function][cfg]") {
  Function function = makeFunction();
  // Two blocks that jump into each other's middle is the shape a flattened
  // dispatcher produces once its indirect branches are resolved, and it is
  // exactly what a recursive traversal would choke on.
  const BlockId entry = function.createBlock(0x1000);
  const BlockId first = function.createBlock(0x1010);
  const BlockId second = function.createBlock(0x1020);

  function.appendCondBranch(entry, 0x1000, function.boolean(true), first, second);
  function.appendBranch(first, 0x1010, second);
  function.appendBranch(second, 0x1020, first);
  function.rebuildEdges();

  const std::vector<BlockId> order = function.reversePostOrder();
  CHECK(order.size() == 3);
  CHECK(order.front() == entry);
}

TEST_CASE("unreachable blocks are excluded from reverse post-order",
          "[il][function][cfg]") {
  Function function = makeFunction();
  const BlockId entry = function.createBlock(0x1000);
  const BlockId orphan = function.createBlock(0x1010);
  function.appendReturn(entry, 0x1000);
  function.appendReturn(orphan, 0x1010);
  function.rebuildEdges();

  CHECK(function.blockCount() == 2);
  CHECK(function.reversePostOrder() == std::vector<BlockId>{entry});
}

TEST_CASE("an indirect branch starts unresolved and can be resolved later",
          "[il][function][cfg]") {
  Function function = makeFunction();
  const BlockId entry = function.createBlock(0x1000);
  const BlockId a = function.createBlock(0x1010);
  const BlockId b = function.createBlock(0x1020);

  const ValueId target = function.appendReadReg(entry, 0x1000, reg("x0"));
  const OpId branch = function.appendIndirectBranch(entry, 0x1004, function.valueRef(target));
  function.appendReturn(a, 0x1010);
  function.appendReturn(b, 0x1020);
  function.rebuildEdges();

  // Zero targets is a real state that says "not yet resolved", not a missing
  // field. It is what the resolution pass looks for.
  CHECK(function.targets(function.op(branch)).empty());
  CHECK(function.block(entry).successors.empty());

  const BlockId resolved[] = {a, b};
  function.setTargets(branch, resolved);
  function.rebuildEdges();
  CHECK(function.targets(function.op(branch)).size() == 2);
  CHECK(function.block(entry).successors == std::vector<BlockId>{a, b});
  CHECK(function.reversePostOrder().size() == 3);
}

TEST_CASE("a target list can grow and shrink", "[il][function]") {
  Function function = makeFunction();
  const BlockId entry = function.createBlock(0x1000);
  const BlockId a = function.createBlock(0x1010);
  const BlockId b = function.createBlock(0x1020);
  const OpId branch = function.appendIndirectBranch(entry, 0x1000, function.boolean(true));
  function.appendReturn(a, 0x1010);
  function.appendReturn(b, 0x1020);

  const BlockId one[] = {a};
  function.setTargets(branch, one);
  REQUIRE(function.targets(function.op(branch)).size() == 1);
  CHECK(function.targets(function.op(branch))[0] == a);

  const BlockId two[] = {a, b};
  function.setTargets(branch, two);
  REQUIRE(function.targets(function.op(branch)).size() == 2);
  CHECK(function.targets(function.op(branch))[1] == b);

  // Back to one: a resolution pass that later proves an edge impossible must be
  // able to remove it.
  function.setTargets(branch, one);
  CHECK(function.targets(function.op(branch)).size() == 1);
}

TEST_CASE("phi operands can be filled in after the fact", "[il][function]") {
  Function function = makeFunction();
  const BlockId entry = function.createBlock(0x1000);
  const BlockId join = function.createBlock(0x1010);

  // A phi is created before its incoming values exist, which is the normal case
  // during SSA construction.
  const OpId phi = function.appendPhi(join, 0x1010, Type::integer(64), {});
  CHECK(function.operands(function.op(phi)).empty());
  REQUIRE(function.op(phi).result.valid());

  const ValueId incoming = function.appendReadReg(entry, 0x1000, reg("x0"));
  function.appendBranch(entry, 0x1004, join);
  const ExprId operands[] = {function.valueRef(incoming),
                             function.constant(Type::integer(64), 0)};
  function.setOperands(phi, operands);

  CHECK(function.operands(function.op(phi)).size() == 2);
  CHECK(function.operands(function.op(phi))[0] == operands[0]);
}

TEST_CASE("names and pass identities are interned", "[il][function]") {
  Function function = makeFunction();

  const uint32_t first = function.internName("aarch64.dmb");
  CHECK(function.internName("aarch64.dmb") == first);
  CHECK(function.internName("aarch64.isb") != first);
  CHECK(function.nameOf(first) == "aarch64.dmb");
  // An out-of-range id reads as empty rather than as garbage.
  CHECK(function.nameOf(9999).empty());

  const PassId pass = function.internPass("deflatten");
  CHECK(pass != kPassLifter);
  CHECK(function.internPass("deflatten") == pass);
  CHECK(function.passName(pass) == "deflatten");
}

TEST_CASE("provenance records the pass that created each op", "[il][function]") {
  Function function = makeFunction();
  const BlockId block = function.createBlock(0x1000);
  const OpId lifted = function.appendNop(block, 0x1000);
  CHECK(function.op(lifted).origin == kPassLifter);

  const PassId pass = function.internPass("deflatten");
  function.setCurrentPass(pass);
  const OpId synthesised = function.appendReturn(block, kNoOpAddress);
  CHECK(function.op(synthesised).origin == pass);
  CHECK(function.op(synthesised).va == kNoOpAddress);
  // Provenance shows up in the text, so a dump says which pass to blame.
  CHECK(printOp(function, synthesised) == "ret !from(deflatten)");
}

TEST_CASE("blocks can be found by their start address", "[il][function]") {
  Function function = makeFunction();
  const BlockId first = function.createBlock(0x1000);
  const BlockId second = function.createBlock(0x1010);
  CHECK(function.blockAt(0x1000) == first);
  CHECK(function.blockAt(0x1010) == second);
  CHECK_FALSE(function.blockAt(0x1008).valid());
}
