// analyzeEmitRedundancy: the IL-level counts a phase's own before/after is
// measured against (see the header for why these are IL-level, not text).
#include <catch2/catch_test_macros.hpp>

#include "il/il_test_support.h"
#include "xdec/analysis/emit_redundancy.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/analysis/variables.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::StackFrame;
using xdec::analysis::VariableTable;
using xdec::analysis::analyzeEmitRedundancy;
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

TEST_CASE("a folded load and a dead store both count towards the report",
          "[analysis][emit-redundancy]") {
  Fixture f;
  // Shape F: read right after write, nothing between -- folds.
  f.function.appendStore(f.entry, 0x1000, Type::integer(32), f.slot(-0x10),
                         f.function.entryReg(f.reg("x0")));
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1004, Type::integer(32), f.slot(-0x10));
  f.function.appendStore(f.entry, 0x1008, Type::integer(32), f.i64(0x9000),
                         f.function.valueRef(loaded));
  // Shape H1: a write-only slot, never read back at all.
  f.function.appendStore(f.entry, 0x100c, Type::integer(32), f.slot(-0x20),
                         f.function.entryReg(f.reg("x1")));
  f.function.appendReturn(f.entry, 0x1010);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const VariableTable variables = VariableTable::recover(f.function, frame);
  const auto report = analyzeEmitRedundancy(f.function, frame, variables);
  CHECK(report.stackLoads == 1);
  CHECK(report.stackLoadsFolded == 1);
  CHECK(report.stackStores == 2);
  CHECK(report.stackStoresDead == 1);
  CHECK(report.writeOnlyLocals == 1);
  CHECK(!report.format().empty());
}
