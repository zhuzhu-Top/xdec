// StackEscapeMap: a pointer escape's own delta, closed over every
// contiguous Store above it -- and never past a gap (see the header for why
// a gap has to stop the closure rather than just skip over it).
#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <vector>

#include "il/il_test_support.h"
#include "xdec/analysis/stack_escape.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::StackEscapeMap;
using xdec::analysis::StackFrame;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
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

TEST_CASE("a call argument's own delta escapes with no adjoining store",
          "[analysis][stack-escape]") {
  Fixture f;
  const ExprId address = f.slot(-0x70);
  const il::OpId call = f.function.appendCall(f.entry, 0x1000, f.i64(0x9000));
  f.function.setOperands(call, std::vector<ExprId>{f.i64(0x9000), address});
  f.function.appendReturn(f.entry, 0x1004);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const StackEscapeMap escapes = StackEscapeMap::compute(f.function, frame);
  CHECK(escapes.isEscaped(-0x70));
  CHECK_FALSE(escapes.isEscaped(-0x68));
}

TEST_CASE("stores contiguous with an escaped delta are folded into its region",
          "[analysis][stack-escape]") {
  Fixture f;
  const ExprId address = f.slot(-0x70);
  f.function.appendStore(f.entry, 0x1000, Type::integer(64), address, f.i64(0x18));
  f.function.appendStore(f.entry, 0x1004, Type::integer(64), f.slot(-0x68), f.i64(1));
  f.function.appendStore(f.entry, 0x1008, Type::integer(64), f.slot(-0x60), f.i64(2));
  const il::OpId call = f.function.appendCall(f.entry, 0x100c, f.i64(0x9000));
  f.function.setOperands(call, std::vector<ExprId>{f.i64(0x9000), address});
  f.function.appendReturn(f.entry, 0x1010);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const StackEscapeMap escapes = StackEscapeMap::compute(f.function, frame);
  CHECK(escapes.isEscaped(-0x70));
  CHECK(escapes.isEscaped(-0x68));
  CHECK(escapes.isEscaped(-0x60));
  // One byte past the last qword's own footprint is a different slot.
  CHECK_FALSE(escapes.isEscaped(-0x58));
}

TEST_CASE("a store across a gap from the escaped delta is not pulled in",
          "[analysis][stack-escape]") {
  Fixture f;
  const ExprId address = f.slot(-0x70);
  f.function.appendStore(f.entry, 0x1000, Type::integer(64), address, f.i64(0x18));
  // A separate, unrelated local eight bytes above the buffer's own qword --
  // not reachable by the buffer's own footprint (which ends at -0x68), so
  // it must stay eligible for its own dead-store verdict.
  const il::OpId farStore =
      f.function.appendStore(f.entry, 0x1004, Type::integer(64), f.slot(-0x50), f.i64(3));
  const il::OpId call = f.function.appendCall(f.entry, 0x1008, f.i64(0x9000));
  f.function.setOperands(call, std::vector<ExprId>{f.i64(0x9000), address});
  f.function.appendReturn(f.entry, 0x100c);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const StackEscapeMap escapes = StackEscapeMap::compute(f.function, frame);
  CHECK(escapes.isEscaped(-0x70));
  CHECK_FALSE(escapes.isEscaped(-0x50));
  (void)farStore;
}

TEST_CASE("two unrelated escapes each close over their own region only",
          "[analysis][stack-escape]") {
  Fixture f;
  const ExprId first = f.slot(-0x70);
  const ExprId second = f.slot(-0x30);
  f.function.appendStore(f.entry, 0x1000, Type::integer(64), first, f.i64(1));
  f.function.appendStore(f.entry, 0x1004, Type::integer(64), f.slot(-0x68), f.i64(2));
  f.function.appendStore(f.entry, 0x1008, Type::integer(64), second, f.i64(3));
  const il::OpId call = f.function.appendCall(f.entry, 0x100c, f.i64(0x9000));
  f.function.setOperands(call, std::vector<ExprId>{f.i64(0x9000), first, second});
  f.function.appendReturn(f.entry, 0x1010);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const StackEscapeMap escapes = StackEscapeMap::compute(f.function, frame);
  CHECK(escapes.isEscaped(-0x70));
  CHECK(escapes.isEscaped(-0x68));
  CHECK(escapes.isEscaped(-0x30));
  // The gap between the two regions belongs to neither.
  CHECK_FALSE(escapes.isEscaped(-0x50));
}

TEST_CASE("no escape at all leaves every delta unescaped", "[analysis][stack-escape]") {
  Fixture f;
  f.function.appendStore(f.entry, 0x1000, Type::integer(64), f.slot(-0x10), f.i64(1));
  f.function.appendReturn(f.entry, 0x1004);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const StackEscapeMap escapes = StackEscapeMap::compute(f.function, frame);
  CHECK(escapes.regions().empty());
  CHECK_FALSE(escapes.isEscaped(-0x10));
}
