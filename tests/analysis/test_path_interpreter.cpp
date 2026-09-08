// PathInterpreter/PathState: cross-block, path-sensitive execution -- the
// two things this adds over analysis::ImageEval (see path_state.h): a phi
// resolves to the one edge a path actually took, not the union of every
// edge, and a load can read back what an earlier store on the same path put
// somewhere the image itself does not define.
#include <catch2/catch_test_macros.hpp>

#include <map>

#include "il/il_test_support.h"
#include "xdec/analysis/path_interpreter.h"
#include "xdec/analysis/path_state.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::ByteReader;
using xdec::analysis::PathInterpreter;
using xdec::analysis::PathOutcome;
using xdec::analysis::PathState;
using xdec::analysis::PathStop;
using xdec::analysis::SymValueSet;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::Function;
using xdec::il::Type;

namespace {

/// A byte image assembled from 8-byte qwords, mirroring test_image_eval.cpp's
/// own fixture.
struct FakeImage {
  std::map<uint64_t, uint64_t> qwords;

  [[nodiscard]] ByteReader reader() const {
    return [this](uint64_t va, std::span<std::byte> out) -> xdec::Result<void> {
      const auto found = qwords.find(va);
      if (found == qwords.end() || out.size() > 8) {
        return xdec::err(xdec::DiagCode::UnmappedAddress, "not in the fake image");
      }
      for (std::size_t index = 0; index < out.size(); ++index) {
        out[index] = static_cast<std::byte>(found->second >> (index * 8));
      }
      return {};
    };
  }
};

}  // namespace

TEST_CASE("a phi resolves to the value on the edge this path took, not the union",
          "[analysis][path-interpreter]") {
  // b1 -> merge, b2 -> merge; merge's phi is 10 from b1, 20 from b2. Two
  // paths through the same IL should read two different, singleton answers
  // -- exactly what ImageEval's union-of-all-arms reading (see its own
  // "select over an unknown condition unions its arms" test) cannot give.
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId b1 = function.createBlock(0x1000);
  const BlockId b2 = function.createBlock(0x1004);
  const BlockId merge = function.createBlock(0x1008);
  function.setEntryBlock(b1);
  function.appendBranch(b1, 0x1000, merge);
  function.appendBranch(b2, 0x1004, merge);
  function.rebuildEdges();  // merge.predecessors == [b1, b2]

  const ExprId ten = function.constant(Type::integer(64), 10);
  const ExprId twenty = function.constant(Type::integer(64), 20);
  const ExprId incoming[] = {ten, twenty};
  const il::OpId phiOp = function.appendPhi(merge, 0x1008, Type::integer(64), incoming);
  function.setOperands(phiOp, incoming);
  const il::ValueId phiValue = function.op(phiOp).result;
  function.appendReturn(merge, 0x100c);

  FakeImage image;
  const PathInterpreter interp(function);

  PathState fromB1(function, image.reader(), nullptr);
  const PathOutcome outcomeB1 = interp.stepBlock(merge, b1, fromB1);
  CHECK(outcomeB1.stop == PathStop::Return);
  const SymValueSet valueViaB1 = fromB1.eval(function.valueRef(phiValue));
  REQUIRE(!valueViaB1.isTop());
  REQUIRE(valueViaB1.values().size() == 1);
  CHECK(valueViaB1.values()[0] == 10);

  PathState fromB2(function, image.reader(), nullptr);
  const PathOutcome outcomeB2 = interp.stepBlock(merge, b2, fromB2);
  CHECK(outcomeB2.stop == PathStop::Return);
  const SymValueSet valueViaB2 = fromB2.eval(function.valueRef(phiValue));
  REQUIRE(!valueViaB2.isTop());
  REQUIRE(valueViaB2.values().size() == 1);
  CHECK(valueViaB2.values()[0] == 20);
}

TEST_CASE("an edge predecessorIndex cannot identify binds the phi to top, not a guess",
          "[analysis][path-interpreter]") {
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId b1 = function.createBlock(0x1000);
  const BlockId merge = function.createBlock(0x1008);
  function.setEntryBlock(b1);
  function.appendBranch(b1, 0x1000, merge);
  function.rebuildEdges();

  const ExprId ten = function.constant(Type::integer(64), 10);
  const ExprId incoming[] = {ten};
  const il::OpId phiOp = function.appendPhi(merge, 0x1008, Type::integer(64), incoming);
  function.setOperands(phiOp, incoming);
  const il::ValueId phiValue = function.op(phiOp).result;
  function.appendReturn(merge, 0x100c);

  FakeImage image;
  const PathInterpreter interp(function);
  // cameFrom names a block that is not actually a predecessor of merge.
  const BlockId stray = function.createBlock(0x9000);
  PathState strayState(function, image.reader(), nullptr);
  (void)interp.stepBlock(merge, stray, strayState);
  CHECK(strayState.eval(function.valueRef(phiValue)).isTop());
}

TEST_CASE("a load reads back what an earlier store on the same path wrote",
          "[analysis][path-interpreter]") {
  // Store(0x9999, 0x55) then Load(0x9999) in the same block; the address is
  // not in the fake image at all, so a correct answer can only have come
  // from this path's own store record (see PathState::loadFrom).
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x2000);
  const BlockId entry = function.createBlock(0x2000);
  function.setEntryBlock(entry);

  const ExprId address = function.constant(Type::integer(64), 0x9999);
  const ExprId stored = function.constant(Type::integer(64), 0x55);
  function.appendStore(entry, 0x2000, Type::integer(64), address, stored);
  const il::ValueId loaded = function.appendLoad(entry, 0x2004, Type::integer(64), address);
  function.appendReturn(entry, 0x2008);
  function.rebuildEdges();

  FakeImage image;  // deliberately empty: 0x9999 is unmapped
  const PathInterpreter interp(function);
  PathState state(function, image.reader(), nullptr);
  const PathOutcome outcome = interp.stepBlock(entry, il::BlockId::invalid(), state);
  CHECK(outcome.stop == PathStop::Return);

  const SymValueSet value = state.eval(function.valueRef(loaded));
  REQUIRE(!value.isTop());
  REQUIRE(value.values().size() == 1);
  CHECK(value.values()[0] == 0x55);
}

TEST_CASE("a store through an address this path cannot pin down forgets prior records",
          "[analysis][path-interpreter]") {
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x3000);
  const BlockId entry = function.createBlock(0x3000);
  function.setEntryBlock(entry);

  const ExprId knownAddress = function.constant(Type::integer(64), 0x9999);
  const ExprId firstValue = function.constant(Type::integer(64), 0x55);
  function.appendStore(entry, 0x3000, Type::integer(64), knownAddress, firstValue);

  // An address this evaluator cannot resolve at all (an unbound argument
  // register) might have overwritten anything, including the slot above.
  const ExprId unknownAddress =
      function.entryReg(function.registers().find("x0"));
  const ExprId secondValue = function.constant(Type::integer(64), 0x66);
  function.appendStore(entry, 0x3004, Type::integer(64), unknownAddress, secondValue);

  const il::ValueId loaded =
      function.appendLoad(entry, 0x3008, Type::integer(64), knownAddress);
  function.appendReturn(entry, 0x300c);
  function.rebuildEdges();

  FakeImage image;
  const PathInterpreter interp(function);
  PathState state(function, image.reader(), nullptr);
  (void)interp.stepBlock(entry, il::BlockId::invalid(), state);

  CHECK(state.eval(function.valueRef(loaded)).isTop());
}
