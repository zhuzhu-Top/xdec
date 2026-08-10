// Import calls at the emit layer: a PLT-resolved name and its noreturn
// annotation (Phase 1-3), and the errno store idiom fold (Phase 4) --
// including the case that must decline it, which is a regression test for a
// real bug this fold shipped with (see foldableErrnoCall in c_stmt.cpp): a
// dispatcher merge's phi reads the call's result on the very register a
// folded call would otherwise stop naming.
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
using xdec::types::BoundName;

namespace {

/// Names `va` under `name` -- the shape `options.names` takes once a PLT stub
/// has resolved to an import (see analysis/plt_stub.h). Nothing else in these
/// tests supplies a symbol table, so this is the only way a call target gets
/// a name at all.
[[nodiscard]] xdec::types::NameAt namedAt(uint64_t va, std::string name) {
  return [va, name = std::move(name)](uint64_t candidate) {
    return candidate == va ? BoundName{name, true} : BoundName{};
  };
}

struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {
    entry = function.createBlock(0x1000);
    function.setEntryBlock(entry);
    // Arity trimming is a post-Vars concern; below that a call still prints
    // every argument register a trailing undef holds.
    function.setMaturity(Maturity::Vars);
  }

  ExprId i64(uint64_t value) { return function.constant(Type::integer(64), value); }
  ExprId entryReg(std::string_view name) {
    return function.entryReg(function.registers().find(name));
  }

  std::string emit(const COptions& options) {
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

TEST_CASE("a direct call to a PLT-resolved import prints under its name",
          "[emit][import-call]") {
  Fixture f;
  f.function.appendCall(f.entry, 0x1000, f.i64(0x1d28a0), Type::integer(64));
  f.function.appendReturn(f.entry, 0x1004);

  COptions options;
  options.names = namedAt(0x1d28a0, "__errno_location");
  const std::string text = f.emit(options);
  INFO(text);
  CHECK(contains(text, "__errno_location()"));
  CHECK(!contains(text, "sub_1d28a0"));
}

TEST_CASE("a call to a known-noreturn import is annotated", "[emit][import-call]") {
  Fixture f;
  f.function.appendCall(f.entry, 0x1000, f.i64(0x1d27f0));
  f.function.appendUnreachable(f.entry, 0x1004);

  COptions options;
  options.names = namedAt(0x1d27f0, "__stack_chk_fail");
  const std::string text = f.emit(options);
  INFO(text);
  CHECK(contains(text, "__stack_chk_fail()"));
  CHECK(contains(text, "/* does not return */"));
}

// The idiom sub_199214 leaves behind: a call to the errno accessor whose
// result is read exactly once, by the store that negates the syscall's
// result into it.
TEST_CASE("an errno accessor call folds into its store when nothing else reads it",
          "[emit][import-call]") {
  Fixture f;
  const il::OpId call = f.function.appendCall(f.entry, 0x1000, f.i64(0x1d28a0), Type::integer(64));
  const ExprId resultRef = f.function.valueRef(f.function.resultOf(call));
  const ExprId negated = f.function.unary(ExprOp::Neg, f.entryReg("x1"));
  f.function.appendStore(f.entry, 0x1004, Type::integer(32), resultRef, negated);
  f.function.appendReturn(f.entry, 0x1008);

  COptions options;
  options.names = namedAt(0x1d28a0, "__errno_location");
  const std::string text = f.emit(options);
  INFO(text);
  CHECK(contains(text, "*__errno_location() = "));
  CHECK(!contains(text, "unnamed-value"));
}

// The shape a dispatcher merge takes: the call's result is x0, the ABI
// register a live argument can also be carried forward in, so a phi at the
// merge block's head reads it on the very edge the fold would otherwise stop
// naming. This must decline to fold -- and, before the fix this regresses,
// silently printed the phi's copy as `/*unnamed-value-%N*/0` instead.
TEST_CASE("an errno accessor call does not fold when its result also feeds a phi",
          "[emit][import-call]") {
  Fixture f;
  const BlockId callBlock = f.function.createBlock(0x2000);
  const BlockId otherBlock = f.function.createBlock(0x3000);
  const BlockId merge = f.function.createBlock(0x4000);

  const ExprId cond = f.function.binary(ExprOp::CmpNe, f.entryReg("x0"), f.i64(0));
  f.function.appendCondBranch(f.entry, 0x1000, cond, callBlock, otherBlock);

  const il::OpId call =
      f.function.appendCall(callBlock, 0x2000, f.i64(0x1d28a0), Type::integer(64));
  const ExprId resultRef = f.function.valueRef(f.function.resultOf(call));
  const ExprId negated = f.function.unary(ExprOp::Neg, f.entryReg("x1"));
  f.function.appendStore(callBlock, 0x2004, Type::integer(32), resultRef, negated);
  f.function.appendBranch(callBlock, 0x2008, merge);

  f.function.appendBranch(otherBlock, 0x3000, merge);

  const il::OpId phi = f.function.appendPhi(merge, 0x4000, Type::integer(64),
                                            std::vector<ExprId>{resultRef, f.i64(0)});
  const il::OpId ret = f.function.appendReturn(merge, 0x4004);
  f.function.setOperands(ret,
                         std::vector<ExprId>{f.function.valueRef(f.function.op(phi).result)});

  COptions options;
  options.names = namedAt(0x1d28a0, "__errno_location");
  const std::string text = f.emit(options);
  INFO(text);
  CHECK(contains(text, "__errno_location();"));
  CHECK(!contains(text, "*__errno_location() ="));
  CHECK(!contains(text, "unnamed-value"));
}
