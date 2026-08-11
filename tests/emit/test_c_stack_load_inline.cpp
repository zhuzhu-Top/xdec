// Emit-level regression for stack-load-fold (see
// analysis/stack_load_fold.h): a Load from a stack slot with exactly one
// live, fresh reader prints the slot's own name in place of a temporary, and
// a slot whose only reader treats it as an address gets promoted to a
// pointer-typed local (analysis::VariableTable's stack-slot pointer
// refinement) instead of an explicit cast at every use.
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
  ExprId slot(int64_t delta) {
    const ExprId sp = function.entryReg(function.registers().find("sp"));
    return delta < 0
               ? function.binary(ExprOp::Sub, sp, i64(static_cast<uint64_t>(-delta)))
               : function.binary(ExprOp::Add, sp, i64(static_cast<uint64_t>(delta)));
  }
  ExprId entryReg(std::string_view name) {
    return function.entryReg(function.registers().find(name));
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

TEST_CASE("a scalar load with one fresh reader prints the local's name, not a temp",
          "[emit][stack-load-fold]") {
  Fixture f;
  // var_984 = a1; t = load(var_984); store [0x9000] <- (t + 1). Left to the
  // ordinary path this would print `t0 = var_984; ...; (t0 + 1)`.
  f.function.appendStore(f.entry, 0x1000, Type::integer(32), f.slot(-0x984), f.entryReg("x1"));
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1004, Type::integer(32), f.slot(-0x984));
  f.function.appendStore(
      f.entry, 0x1008, Type::integer(32), f.i64(0x9000),
      f.function.binary(ExprOp::Add, f.function.valueRef(loaded), f.i64(1)));
  f.function.appendReturn(f.entry, 0x100c);

  const std::string text = f.emit();
  INFO(text);
  CHECK(occurrences(text, "var_984") >= 1);
  // No temporary ever holds the reloaded value.
  CHECK(occurrences(text, "= var_984;") == 0);
  CHECK(occurrences(text, "(var_984 + 0x1)") == 1);
}

TEST_CASE("a load whose only reader is a store's address promotes the slot to a pointer",
          "[emit][stack-load-fold]") {
  Fixture f;
  // var_980 = a2 (a spilled pointer); p = load(var_980); *p = a3. The slot's
  // only reader is the store's address, so variables.cpp's pointer
  // refinement should type var_980 itself as a pointer rather than leaving
  // it a plain integer dereferenced through a cast at the one use.
  f.function.appendStore(f.entry, 0x1000, Type::integer(64), f.slot(-0x980), f.entryReg("x2"));
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1004, Type::integer(64), f.slot(-0x980));
  f.function.appendStore(f.entry, 0x1008, Type::integer(32), f.function.valueRef(loaded),
                         f.entryReg("x3"));
  f.function.appendReturn(f.entry, 0x100c);

  const std::string text = f.emit();
  INFO(text);
  // The local is declared as a pointer, and nothing prints a temp for the
  // reload in between.
  CHECK(occurrences(text, "uint32_t* var_980;") == 1);
  CHECK(occurrences(text, "= var_980;") == 0);
  CHECK(occurrences(text, "var_980") >= 2);  // declaration + the store's address
}

TEST_CASE("two live readers of one load in the same block both name the local directly",
          "[emit][stack-load-fold]") {
  Fixture f;
  f.function.appendStore(f.entry, 0x1000, Type::integer(32), f.slot(-0x10), f.entryReg("x0"));
  const il::ValueId loaded =
      f.function.appendLoad(f.entry, 0x1004, Type::integer(32), f.slot(-0x10));
  f.function.appendStore(f.entry, 0x1008, Type::integer(32), f.i64(0x9000),
                         f.function.valueRef(loaded));
  f.function.appendStore(f.entry, 0x100c, Type::integer(32), f.i64(0x9008),
                         f.function.valueRef(loaded));
  f.function.appendReturn(f.entry, 0x1010);

  const std::string text = f.emit();
  INFO(text);
  // Both sinks name the local directly; no temp ever holds the reload.
  CHECK(occurrences(text, "= var_10;") == 2);   // each sink names the local directly
  CHECK(occurrences(text, "t0 = var_10;") == 0);
  CHECK(occurrences(text, "(*(uint32_t*)(0x9000)) = var_10;") == 1);
  CHECK(occurrences(text, "(*(uint32_t*)(0x9008)) = var_10;") == 1);
}
