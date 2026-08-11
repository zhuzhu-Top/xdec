// Emit-level regression for load-inline (see analysis/load_inline.h): a Load
// through a Global or Other (argument-plus-offset) address with exactly one
// live, fresh reader prints the dereference directly at that use instead of
// materializing it into a temporary first.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "il/il_test_support.h"
#include "xdec/analysis/dominators.h"
#include "xdec/analysis/loops.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/analysis/variables.h"
#include "xdec/emit/c_printer.h"
#include "xdec/emit/structure.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::Dominators;
using xdec::analysis::NaturalLoop;
using xdec::analysis::PostDominators;
using xdec::analysis::StackFrame;
using xdec::analysis::VariableTable;
using xdec::emit::printFunction;
using xdec::emit::structureFunction;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::Type;

namespace {

struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {
    entry = function.createBlock(0x1000);
    function.setEntryBlock(entry);
  }

  ExprId i64(uint64_t value) { return function.constant(Type::integer(64), value); }
  ExprId entryReg(std::string_view name) {
    return function.entryReg(function.registers().find(name));
  }
  ExprId argPlus(std::string_view name, int64_t offset) {
    const ExprId base = entryReg(name);
    return offset == 0 ? base : function.binary(ExprOp::Add, base, i64(static_cast<uint64_t>(offset)));
  }

  std::string emit() {
    function.rebuildEdges();
    const StackFrame frame = StackFrame::compute(function);
    const VariableTable variables = VariableTable::recover(function, frame);
    const Dominators dominators = Dominators::compute(function);
    const PostDominators postDominators = PostDominators::compute(function);
    const std::vector<NaturalLoop> loops = naturalLoops(function, dominators);
    return printFunction(function, variables, frame,
                         structureFunction(function, dominators, postDominators, loops));
  }

  Function function;
  BlockId entry;
};

[[nodiscard]] std::size_t occurrences(const std::string& text, std::string_view needle) {
  std::size_t count = 0;
  std::size_t pos = 0;
  while ((pos = text.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

}  // namespace

TEST_CASE("a global load with one fresh reader prints the dereference in place, not a temp",
          "[emit][load-inline]") {
  Fixture f;
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1000, Type::integer(32), f.i64(0x400900));
  f.function.appendStore(f.entry, 0x1004, Type::integer(32), f.i64(0x9000),
                         f.function.valueRef(loaded));
  f.function.appendReturn(f.entry, 0x1008);

  const std::string text = f.emit();
  INFO(text);
  CHECK(occurrences(text, "t0 =") == 0);
  CHECK(occurrences(text, "(*(uint32_t*)(0x9000)) = (*(uint32_t*)(0x400900));") == 1);
}

TEST_CASE("an argument-plus-offset load with one fresh reader folds the same way",
          "[emit][load-inline]") {
  Fixture f;
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1000, Type::integer(32), f.argPlus("x0", 0x18));
  f.function.appendCall(f.entry, 0x1004, f.i64(0x8000));
  f.function.appendStore(f.entry, 0x1008, Type::integer(32), f.i64(0x9000),
                         f.function.valueRef(loaded));
  f.function.appendReturn(f.entry, 0x100c);

  // Two variants of the same shape: one with nothing between the load and
  // its use (folds), one with an intervening call (does not).
  const std::string text = f.emit();
  INFO(text);
  CHECK(occurrences(text, "(*(uint32_t*)(0x9000))") == 1);
}

TEST_CASE("a call between the load and its use keeps the ordinary temporary",
          "[emit][load-inline]") {
  Fixture f;
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1000, Type::integer(32), f.i64(0x400900));
  f.function.appendCall(f.entry, 0x1004, f.i64(0x8000));
  f.function.appendStore(f.entry, 0x1008, Type::integer(32), f.i64(0x9000),
                         f.function.valueRef(loaded));
  f.function.appendReturn(f.entry, 0x100c);

  const std::string text = f.emit();
  INFO(text);
  CHECK(occurrences(text, "= (*(uint32_t*)(0x400900));") == 1);
}
