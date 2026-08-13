// Vtable dispatch at the emit layer (Phase 4c): a computed call through a
// slot analysis::findConfirmedVtableCalls confirmed gets a `/* vtable slot
// ... */` note naming the offset; nothing else about the call changes, since
// neither a struct layout nor a class name is available to spell instead
// (see vtable_call.h).
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

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
using xdec::emit::COptions;
using xdec::emit::printFunction;
using xdec::emit::structureFunction;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::Maturity;
using xdec::il::Type;

namespace {

struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {
    entry = function.createBlock(0x1000);
    function.setEntryBlock(entry);
    function.setMaturity(Maturity::Vars);
  }

  ExprId i64(uint64_t value) { return function.constant(Type::integer(64), value); }
  ExprId entryReg(std::string_view name) {
    return function.entryReg(function.registers().find(name));
  }

  std::string emit(const COptions& options = {}) {
    function.rebuildEdges();
    const StackFrame frame = StackFrame::compute(function);
    const VariableTable variables = VariableTable::recover(function, frame);
    const Dominators dominators = Dominators::compute(function);
    const PostDominators postDominators = PostDominators::compute(function);
    const std::vector<NaturalLoop> loops = naturalLoops(function, dominators);
    return printFunction(
        function, variables, frame,
        structureFunction(function, dominators, postDominators, loops), options);
  }

  Function function;
  BlockId entry;
};

[[nodiscard]] bool contains(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("a call through a slot confirmed as a vtable is annotated with the offset",
          "[emit][vtable-call]") {
  Fixture f;
  const ExprId object = f.entryReg("x0");
  const ExprId firstAddress = f.function.binary(ExprOp::Add, object, f.i64(0x10));
  const il::ValueId firstTarget =
      f.function.appendLoad(f.entry, 0x1000, Type::integer(64), firstAddress);
  f.function.appendCall(f.entry, 0x1004, f.function.valueRef(firstTarget));

  const ExprId secondAddress = f.function.binary(ExprOp::Add, object, f.i64(0x18));
  const il::ValueId secondTarget =
      f.function.appendLoad(f.entry, 0x1008, Type::integer(64), secondAddress);
  f.function.appendCall(f.entry, 0x100c, f.function.valueRef(secondTarget));
  f.function.appendReturn(f.entry, 0x1010);

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "/* vtable slot 0x10 */"));
  CHECK(contains(text, "/* vtable slot 0x18 */"));
}

TEST_CASE("a call through a plain function pointer -- only one slot ever seen on its "
          "object -- gets no vtable note",
          "[emit][vtable-call]") {
  Fixture f;
  const ExprId object = f.entryReg("x0");
  const ExprId address = f.function.binary(ExprOp::Add, object, f.i64(0x10));
  const il::ValueId target = f.function.appendLoad(f.entry, 0x1000, Type::integer(64), address);
  f.function.appendCall(f.entry, 0x1004, f.function.valueRef(target));
  f.function.appendReturn(f.entry, 0x1008);

  const std::string text = f.emit();
  INFO(text);
  CHECK(!contains(text, "vtable slot"));
}
