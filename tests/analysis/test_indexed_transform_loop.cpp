// matchIndexedTransformLoop: recovering `dst[i] = f(src[i], key)` from an
// induction phi plus a load/store pair sharing a block, purely from IL
// shape -- no register allocation or SSA construction pass involved, so
// the loop below is built directly at the level that pass would leave it
// at (compare test_image_eval.cpp's phi-loop fixture).
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "il/il_test_support.h"
#include "xdec/analysis/indexed_transform_loop.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::IndexedTransformLoop;
using xdec::analysis::matchIndexedTransformLoop;
using xdec::analysis::NaturalLoop;
using xdec::analysis::TransformOp;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::Type;
using xdec::il::ValueId;

namespace {

struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {}

  ExprId i64(uint64_t value) { return function.constant(Type::integer(64), value); }
  ExprId entryReg(std::string_view name) {
    return function.entryReg(function.registers().find(name));
  }
  BlockId block(uint64_t va) { return function.createBlock(va); }

  Function function;
};

/// entry: br header
/// header: idx = phi(0 [entry], idx+1 [body]); brc (idx < bound), body, exit
/// body:   v = *(src + idx); store *(dst + idx) = v ^ key; br header
/// exit:   ret
///
/// Returns the loop and the header's induction value.
struct LoopSkeleton {
  BlockId entry;
  BlockId header;
  BlockId body;
  BlockId exit;
  ValueId index;
  NaturalLoop loop;
};

LoopSkeleton buildLoopSkeleton(Fixture& f, uint64_t bound) {
  LoopSkeleton s;
  s.entry = f.block(0x1000);
  s.header = f.block(0x2000);
  s.body = f.block(0x3000);
  s.exit = f.block(0x4000);

  f.function.setEntryBlock(s.entry);
  f.function.appendBranch(s.entry, 0x1000, s.header);
  s.index = f.function.prependPhi(s.header, 0x2000, Type::integer(64));
  const ExprId cond =
      f.function.binary(ExprOp::CmpLtU, f.function.valueRef(s.index), f.i64(bound));
  f.function.appendCondBranch(s.header, 0x2004, cond, s.body, s.exit);
  f.function.appendReturn(s.exit, 0x4000);
  // `rebuildEdges` recomputes every block's predecessors from its terminator
  // each time it runs, in block-creation order -- so `header`'s eventual
  // predecessor order is exactly [entry, body] regardless of when it is
  // called, since entry's edge to header always sorts before body's. The
  // caller appends body's own ops (and calls rebuildEdges once, after) so
  // this phi's operands can be set in that order up front.
  const ExprId next = f.function.binary(ExprOp::Add, f.function.valueRef(s.index), f.i64(1));
  const std::vector<ExprId> incoming{f.i64(0), next};
  f.function.setOperands(f.function.value(s.index).definition, incoming);

  s.loop.header = s.header;
  s.loop.latches = {s.body};
  s.loop.blocks = {s.header, s.body};
  return s;
}

}  // namespace

TEST_CASE("a load xored with a loop-invariant key and stored back at the same "
          "index is an indexed-transform loop",
          "[analysis][indexed-transform-loop]") {
  Fixture f;
  LoopSkeleton s = buildLoopSkeleton(f, 0x10);

  const ExprId srcAddress =
      f.function.binary(ExprOp::Add, f.entryReg("x0"), f.function.valueRef(s.index));
  const ValueId loaded = f.function.appendLoad(s.body, 0x3000, Type::integer(64), srcAddress);
  const ExprId key = f.entryReg("x2");
  const ExprId transformed = f.function.binary(ExprOp::Xor, f.function.valueRef(loaded), key);
  const ExprId dstAddress =
      f.function.binary(ExprOp::Add, f.entryReg("x1"), f.function.valueRef(s.index));
  f.function.appendStore(s.body, 0x3004, Type::integer(64), dstAddress, transformed);
  f.function.appendBranch(s.body, 0x3008, s.header);
  f.function.rebuildEdges();

  const auto result = matchIndexedTransformLoop(f.function, s.loop);
  REQUIRE(result.has_value());
  CHECK(result->header == s.header);
  CHECK(result->index == s.index);
  REQUIRE(result->indexStart.has_value());
  CHECK(*result->indexStart == 0);
  CHECK(result->indexStride == 1);
  CHECK(result->srcBase == f.entryReg("x0"));
  CHECK(result->dstBase == f.entryReg("x1"));
  CHECK(result->elementScale == 1);
  CHECK(result->op == TransformOp::Xor);
  CHECK(result->key == key);
  CHECK(result->loadIsFirstOperand);
}

TEST_CASE("a scaled index (idx*4) rotated by a key is still recovered, "
          "including which side is the loaded value",
          "[analysis][indexed-transform-loop]") {
  Fixture f;
  LoopSkeleton s = buildLoopSkeleton(f, 0x8);

  const ExprId scaledIndex =
      f.function.binary(ExprOp::Mul, f.function.valueRef(s.index), f.i64(4));
  const ExprId srcAddress = f.function.binary(ExprOp::Add, f.entryReg("x0"), scaledIndex);
  const ValueId loaded = f.function.appendLoad(s.body, 0x3000, Type::integer(32), srcAddress);
  const ExprId key = f.entryReg("x2");
  const ExprId rotated =
      f.function.binary(ExprOp::RotL, f.function.valueRef(loaded), key);
  const ExprId dstAddress = f.function.binary(ExprOp::Add, f.entryReg("x1"), scaledIndex);
  f.function.appendStore(s.body, 0x3004, Type::integer(32), dstAddress, rotated);
  f.function.appendBranch(s.body, 0x3008, s.header);
  f.function.rebuildEdges();

  const auto result = matchIndexedTransformLoop(f.function, s.loop);
  REQUIRE(result.has_value());
  CHECK(result->elementScale == 4);
  CHECK(result->op == TransformOp::RotateLeft);
  CHECK(result->key == key);
  CHECK(result->loadIsFirstOperand);
}

TEST_CASE("a store unrelated to the loaded value is not an indexed-transform loop",
          "[analysis][indexed-transform-loop]") {
  Fixture f;
  LoopSkeleton s = buildLoopSkeleton(f, 0x10);

  const ExprId srcAddress =
      f.function.binary(ExprOp::Add, f.entryReg("x0"), f.function.valueRef(s.index));
  f.function.appendLoad(s.body, 0x3000, Type::integer(64), srcAddress);
  // Stores a plain constant -- the loaded value plays no part in it.
  const ExprId dstAddress =
      f.function.binary(ExprOp::Add, f.entryReg("x1"), f.function.valueRef(s.index));
  f.function.appendStore(s.body, 0x3004, Type::integer(64), dstAddress, f.i64(0));
  f.function.appendBranch(s.body, 0x3008, s.header);
  f.function.rebuildEdges();

  CHECK(!matchIndexedTransformLoop(f.function, s.loop).has_value());
}

TEST_CASE("a header phi that is not a simple increment is not an induction variable",
          "[analysis][indexed-transform-loop]") {
  Fixture f;
  LoopSkeleton s = buildLoopSkeleton(f, 0x10);
  // Overwrite the back edge to something that is not `idx + const`.
  const std::vector<ExprId> incoming{f.i64(0), f.entryReg("x3")};
  f.function.setOperands(f.function.value(s.index).definition, incoming);

  const ExprId srcAddress =
      f.function.binary(ExprOp::Add, f.entryReg("x0"), f.function.valueRef(s.index));
  const ValueId loaded = f.function.appendLoad(s.body, 0x3000, Type::integer(64), srcAddress);
  const ExprId transformed =
      f.function.binary(ExprOp::Xor, f.function.valueRef(loaded), f.entryReg("x2"));
  const ExprId dstAddress =
      f.function.binary(ExprOp::Add, f.entryReg("x1"), f.function.valueRef(s.index));
  f.function.appendStore(s.body, 0x3004, Type::integer(64), dstAddress, transformed);
  f.function.appendBranch(s.body, 0x3008, s.header);
  f.function.rebuildEdges();

  CHECK(!matchIndexedTransformLoop(f.function, s.loop).has_value());
}
