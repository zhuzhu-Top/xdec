// Whether the preamble pulls in xdec_helpers.h: exactly when the body used a
// helper the header defines, never for the two that are not header material
// (intrin, syscall), and only through COptions::helpersHeader's own say.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "il/il_test_support.h"
#include "xdec/analysis/dominators.h"
#include "xdec/analysis/loops.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/analysis/variables.h"
#include "xdec/emit/c_printer.h"
#include "xdec/emit/structure.h"
#include "xdec/il/function.h"
#include "xdec/passes/recover_syscall.h"

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

  void observe(ExprId value, uint32_t width = 64) {
    function.appendStore(entry, 0x1000, Type::integer(width), i64(0x400000), value);
  }

  std::string emit(const COptions& options = {}) {
    function.appendReturn(entry, 0x1010);
    function.rebuildEdges();
    const StackFrame frame = StackFrame::compute(function);
    const VariableTable variables = VariableTable::recover(function, frame);
    const Dominators dominators = Dominators::compute(function);
    const PostDominators postDominators = PostDominators::compute(function);
    const std::vector<NaturalLoop> loops = naturalLoops(function, dominators);
    return printFunction(function, variables, frame,
                         structureFunction(function, dominators, postDominators,
                                           loops),
                         options);
  }

  Function function;
  BlockId entry;
};

[[nodiscard]] bool contains(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("a body with no header-backed helper gets no include",
          "[emit][helpers-header]") {
  Fixture f;
  f.observe(f.function.binary(ExprOp::Add, f.entryReg("x0"), f.i64(1)));

  const std::string text = f.emit();
  INFO(text);
  CHECK(!contains(text, "xdec_helpers.h"));
}

TEST_CASE("a rotate pulls in the helpers header", "[emit][helpers-header]") {
  Fixture f;
  f.observe(f.function.binary(ExprOp::RotR, f.entryReg("x0"), f.i64(3)));

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "#include \"xdec_helpers.h\""));
}

TEST_CASE("a byte swap also pulls in the helpers header", "[emit][helpers-header]") {
  Fixture f;
  f.observe(f.function.unary(ExprOp::ByteSwap, f.entryReg("x0")));

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "#include \"xdec_helpers.h\""));
  CHECK(contains(text, "bswap64(a0)"));
}

TEST_CASE("an embedder stub also pulls in the helpers header",
          "[emit][helpers-header]") {
  // clz/ctz/mulhi/brev/flagbit/float ops are declared in the header now,
  // not just left as a comment -- so they need the include too, same as the
  // portable helpers.
  Fixture f;
  f.observe(f.function.unary(ExprOp::Clz, f.entryReg("x0")));

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "#include \"xdec_helpers.h\""));
  CHECK(contains(text, "xdec_clz64(a0)"));
}

TEST_CASE("an unknown syscall alone does not pull in the helpers header",
          "[emit][helpers-header]") {
  // __xdec_syscall's declaration is not header material (see
  // c_helpers.cpp): it is the one ad hoc declaration that predates the
  // header and still lives beside it.
  Fixture f;
  const std::vector<ExprId> operands{f.function.constant(Type::integer(16), 0),
                                     f.function.constant(Type::integer(64), 9999)};
  f.function.appendIntrinsic(f.entry, 0x1000, xdec::passes::kSyscallIntrinsic,
                             Type::integer(64), operands);

  const std::string text = f.emit();
  INFO(text);
  CHECK(!contains(text, "xdec_helpers.h"));
  CHECK(contains(text, "__xdec_syscall(9999"));
}

TEST_CASE("helpersHeader can be overridden to a different path",
          "[emit][helpers-header]") {
  Fixture f;
  f.observe(f.function.binary(ExprOp::RotL, f.entryReg("x0"), f.i64(3)));

  COptions options;
  options.helpersHeader = "xdec/xdec_helpers.h";
  const std::string text = f.emit(options);
  INFO(text);
  CHECK(contains(text, "#include \"xdec/xdec_helpers.h\""));
  CHECK(!contains(text, "#include \"xdec_helpers.h\""));
}

TEST_CASE("an empty helpersHeader suppresses the include entirely",
          "[emit][helpers-header]") {
  // The caller's escape hatch: make the names available some other way (an
  // amalgamated build, a different include path) and print no directive at
  // all rather than one that would not resolve.
  Fixture f;
  f.observe(f.function.binary(ExprOp::RotR, f.entryReg("x0"), f.i64(3)));

  COptions options;
  options.helpersHeader.clear();
  const std::string text = f.emit(options);
  INFO(text);
  CHECK(!contains(text, "xdec_helpers.h"));
  CHECK(contains(text, "rotr64(a0, 0x3)"));
}
