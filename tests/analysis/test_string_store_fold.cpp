// findFoldableStringStores: the safety rules that decide when a run of
// constant Stores decodes to one printable, NUL-terminated C string (see the
// header for the full rule list).
#include <catch2/catch_test_macros.hpp>

#include "il/il_test_support.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/analysis/string_store_fold.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::findFoldableStringStores;
using xdec::analysis::StackFrame;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::OpId;
using xdec::il::RegId;
using xdec::il::Type;

namespace {

struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {
    entry = function.createBlock(0x1000);
    function.setEntryBlock(entry);
  }

  RegId reg(std::string_view name) { return function.registers().find(name); }
  ExprId i64(uint64_t value) { return function.constant(Type::integer(64), value); }
  ExprId slot(int64_t delta) {
    const ExprId sp = function.entryReg(reg("sp"));
    return delta < 0 ? function.binary(ExprOp::Sub, sp, i64(static_cast<uint64_t>(-delta)))
                     : function.binary(ExprOp::Add, sp, i64(static_cast<uint64_t>(delta)));
  }

  Function function;
  BlockId entry;
};

}  // namespace

TEST_CASE("a two-store run spelling a short string folds", "[analysis][string-store-fold]") {
  Fixture f;
  // "hi\0" packed little-endian across a 2-byte store and a 1-byte store,
  // exactly the shape a compiler leaves for `strcpy(dst, "hi")`.
  const OpId first =
      f.function.appendStore(f.entry, 0x1000, Type::integer(16), f.slot(-0x10), f.i64(0x6968));
  const OpId second =
      f.function.appendStore(f.entry, 0x1004, Type::integer(8), f.slot(-0xe), f.i64(0x0));
  f.function.appendReturn(f.entry, 0x1008);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto folds = findFoldableStringStores(f.function, frame);
  REQUIRE(folds.contains(first.index()));
  const auto& fold = folds.at(first.index());
  CHECK(fold.delta == -0x10);
  CHECK(fold.text == "hi");
  REQUIRE(fold.continuationOps.size() == 1);
  CHECK(fold.continuationOps[0] == second.index());
  CHECK(!folds.contains(second.index()));
}

TEST_CASE("a longer run spelling \"com.apple.absd\" folds, matching the dyld-cache shape",
          "[analysis][string-store-fold]") {
  Fixture f;
  // The exact bytes sub_192464d44 (docs/22) writes for "com.apple.absd":
  // an 8-byte, a 4-byte, a 2-byte and a 1-byte store, back to back.
  const OpId s0 = f.function.appendStore(f.entry, 0x1000, Type::integer(64), f.slot(-0x6b),
                                         f.i64(0x6c7070612e6d6f63));
  const OpId s1 =
      f.function.appendStore(f.entry, 0x1004, Type::integer(32), f.slot(-0x63), f.i64(0x62612e65));
  const OpId s2 =
      f.function.appendStore(f.entry, 0x1008, Type::integer(16), f.slot(-0x5f), f.i64(0x6473));
  const OpId s3 =
      f.function.appendStore(f.entry, 0x100c, Type::integer(8), f.slot(-0x5d), f.i64(0x0));
  f.function.appendReturn(f.entry, 0x1010);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const auto folds = findFoldableStringStores(f.function, frame);
  REQUIRE(folds.contains(s0.index()));
  const auto& fold = folds.at(s0.index());
  CHECK(fold.delta == -0x6b);
  CHECK(fold.text == "com.apple.absd");
  CHECK(fold.continuationOps == std::vector<uint32_t>{s1.index(), s2.index(), s3.index()});
}

TEST_CASE("a single constant store never folds on its own", "[analysis][string-store-fold]") {
  Fixture f;
  f.function.appendStore(f.entry, 0x1000, Type::integer(16), f.slot(-0x10), f.i64(0x6968));
  f.function.appendReturn(f.entry, 0x1004);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  CHECK(findFoldableStringStores(f.function, frame).empty());
}

TEST_CASE("a gap between two stores' byte ranges blocks the fold",
          "[analysis][string-store-fold]") {
  Fixture f;
  // Second store starts one byte past where the first's range ends: not the
  // contiguous initializer layout this analysis targets.
  f.function.appendStore(f.entry, 0x1000, Type::integer(16), f.slot(-0x10), f.i64(0x6968));
  f.function.appendStore(f.entry, 0x1004, Type::integer(8), f.slot(-0xd), f.i64(0x0));
  f.function.appendReturn(f.entry, 0x1008);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  CHECK(findFoldableStringStores(f.function, frame).empty());
}

TEST_CASE("an op between the two stores blocks the fold", "[analysis][string-store-fold]") {
  Fixture f;
  // Nothing may sit between a run's Stores, not even something that could
  // never alias them: adjacency in the block's own op order is the rule,
  // not just non-interference (see the header).
  f.function.appendStore(f.entry, 0x1000, Type::integer(16), f.slot(-0x10), f.i64(0x6968));
  f.function.appendReadReg(f.entry, 0x1002, f.reg("x0"));
  f.function.appendStore(f.entry, 0x1004, Type::integer(8), f.slot(-0xe), f.i64(0x0));
  f.function.appendReturn(f.entry, 0x1008);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  CHECK(findFoldableStringStores(f.function, frame).empty());
}

TEST_CASE("a non-constant value in the run blocks the fold", "[analysis][string-store-fold]") {
  Fixture f;
  f.function.appendStore(f.entry, 0x1000, Type::integer(16), f.slot(-0x10), f.i64(0x6968));
  f.function.appendStore(f.entry, 0x1004, Type::integer(8), f.slot(-0xe),
                         f.function.entryReg(f.reg("x0")));
  f.function.appendReturn(f.entry, 0x1008);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  CHECK(findFoldableStringStores(f.function, frame).empty());
}

TEST_CASE("a run with a non-printable byte does not decode as a string",
          "[analysis][string-store-fold]") {
  Fixture f;
  // 0x01 is not printable ASCII, so this is some other constant-initialized
  // buffer, not a string -- this analysis declines to guess.
  f.function.appendStore(f.entry, 0x1000, Type::integer(16), f.slot(-0x10), f.i64(0x0169));
  f.function.appendStore(f.entry, 0x1004, Type::integer(8), f.slot(-0xe), f.i64(0x0));
  f.function.appendReturn(f.entry, 0x1008);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  CHECK(findFoldableStringStores(f.function, frame).empty());
}

TEST_CASE("a NUL byte before the run's own end does not decode as a string",
          "[analysis][string-store-fold]") {
  Fixture f;
  // The NUL terminator lands mid-run, not as the very last byte: rule 4
  // requires exactly one NUL, at the end.
  f.function.appendStore(f.entry, 0x1000, Type::integer(16), f.slot(-0x10), f.i64(0x0068));
  f.function.appendStore(f.entry, 0x1004, Type::integer(8), f.slot(-0xe), f.i64(0x69));
  f.function.appendReturn(f.entry, 0x1008);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  CHECK(findFoldableStringStores(f.function, frame).empty());
}

TEST_CASE("a run to a global address is left alone", "[analysis][string-store-fold]") {
  Fixture f;
  f.function.appendStore(f.entry, 0x1000, Type::integer(16), f.i64(0x30c420), f.i64(0x6968));
  f.function.appendStore(f.entry, 0x1004, Type::integer(8), f.i64(0x30c422), f.i64(0x0));
  f.function.appendReturn(f.entry, 0x1008);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  CHECK(findFoldableStringStores(f.function, frame).empty());
}
