// How an `svc` prints once the syscall table can name it.
//
// The intrinsic is built by hand here rather than lifted, because what is
// under test is the emitter's half of the contract: given the operand layout
// the AArch64 spec produces (imm16, x8, x0..x5), which C does the reader see.
// tests/passes/test_recover_syscall.cpp tests the other half — that a lift
// really does produce that layout — so between them nothing is assumed.
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
#include "xdec/types/syscall_table.h"

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
using xdec::il::Function;
using xdec::il::Type;
using xdec::types::SyscallTable;

namespace {

const SyscallTable& table() {
  static const SyscallTable kTable = [] {
    const xdec::Result<std::string> path =
        SyscallTable::resolvePath(SyscallTable::defaultName());
    if (!path) {
      FAIL(path.error().format());
    }
    xdec::Result<SyscallTable> loaded = SyscallTable::loadFile(*path);
    if (!loaded) {
      FAIL(loaded.error().format());
    }
    return std::move(*loaded);
  }();
  return kTable;
}

struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {
    entry = function.createBlock(0x1000);
    function.setEntryBlock(entry);
  }

  ExprId entryReg(std::string_view name) {
    return function.entryReg(function.registers().find(name));
  }

  /// The svc intrinsic as the spec lifts it: `imm16`, the number, then as many
  /// of x0..x5 as the caller wants to leave attached.
  void syscall(ExprId number, std::vector<ExprId> args) {
    std::vector<ExprId> operands{function.constant(Type::integer(16), 0), number};
    operands.insert(operands.end(), args.begin(), args.end());
    function.appendIntrinsic(entry, 0x1000, xdec::passes::kSyscallIntrinsic,
                             Type::integer(64), operands);
  }

  std::string emit(const SyscallTable* syscalls) {
    function.appendReturn(entry, 0x1004);
    function.rebuildEdges();
    const StackFrame frame = StackFrame::compute(function);
    const VariableTable variables = VariableTable::recover(function, frame);
    const Dominators dominators = Dominators::compute(function);
    const PostDominators postDominators = PostDominators::compute(function);
    const std::vector<NaturalLoop> loops = naturalLoops(function, dominators);
    COptions options;
    options.syscalls = syscalls;
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

// The whole point of the track: a number and six registers become a call whose
// name says what the kernel was asked to do.
TEST_CASE("a known syscall prints as a named call with typed arguments",
          "[emit][syscall]") {
  Fixture f;
  f.syscall(f.function.constant(Type::integer(64), 64),
            {f.entryReg("x0"), f.entryReg("x1"), f.entryReg("x2")});
  const std::string text = f.emit(&table());
  INFO(text);
  CHECK(contains(text, "sys_write((int)"));
  CHECK(contains(text, "(const void*)"));
  CHECK(contains(text, "(size_t)"));
  CHECK(!contains(text, "__xdec_intrin_aarch64.svc"));
  // Declared, because nothing else in the output would introduce the name.
  CHECK(contains(text, "long sys_write(int, const void*, size_t); // syscall 64"));
}

// A signature naming a tag no header here defines still has to compile as a
// cast, so the tag is forward-declared.
TEST_CASE("a syscall taking a struct pointer forward-declares the tag",
          "[emit][syscall]") {
  Fixture f;
  f.syscall(f.function.constant(Type::integer(64), 169),
            {f.entryReg("x0"), f.entryReg("x1")});
  const std::string text = f.emit(&table());
  INFO(text);
  CHECK(contains(text, "struct timeval;\n"));
  CHECK(contains(text, "struct timezone;\n"));
  CHECK(contains(text, "sys_gettimeofday((struct timeval*)"));
}

// The table's silence is about the table. The number is still the most useful
// thing on the line, so it leads, and the arguments stay as they were found.
TEST_CASE("a number the table does not list keeps the number and the arguments",
          "[emit][syscall]") {
  Fixture f;
  f.syscall(f.function.constant(Type::integer(64), 9999),
            {f.entryReg("x0"), f.entryReg("x1")});
  const std::string text = f.emit(&table());
  INFO(text);
  CHECK(contains(text, "__xdec_syscall(9999,"));
  CHECK(contains(text, "long __xdec_syscall(long nr, ...);"));
}

// x8 came from somewhere the analyses could not follow. Printing the expression
// that computed it is what keeps the output honest and still readable.
TEST_CASE("a non-constant number prints as the expression that produced it",
          "[emit][syscall]") {
  Fixture f;
  f.syscall(f.entryReg("x7"), {f.entryReg("x0")});
  const std::string text = f.emit(&table());
  INFO(text);
  CHECK(contains(text, "__xdec_syscall("));
  CHECK(contains(text, "/* x8 */"));
}

// No table is the pipeline running without the data file, and it must degrade
// to the same fallback rather than to the raw intrinsic: the number is known
// either way, and printing it is strictly more than the instruction said.
TEST_CASE("without a table a syscall still prints as a syscall", "[emit][syscall]") {
  Fixture f;
  f.syscall(f.function.constant(Type::integer(64), 64), {f.entryReg("x0")});
  const std::string text = f.emit(nullptr);
  INFO(text);
  CHECK(contains(text, "__xdec_syscall(64,"));
  CHECK(!contains(text, "__xdec_intrin_aarch64.svc"));
}
