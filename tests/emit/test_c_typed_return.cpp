// Phase 4 of the type-propagation plan: a call or `svc` result declares at
// the callee's typed return, not the raw register width (see
// CContext::typeOfValue), and the enclosing function's own return follows an
// agreeing typed result (see CContext::functionReturnType) -- but only when
// that agrees with what the body itself proved, per TypeBinder::consistent.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "il/il_test_support.h"
#include "xdec/analysis/dominators.h"
#include "xdec/analysis/loops.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/analysis/typed_variables.h"
#include "xdec/analysis/variables.h"
#include "xdec/emit/c_printer.h"
#include "xdec/emit/structure.h"
#include "xdec/il/function.h"
#include "xdec/passes/recover_syscall.h"
#include "xdec/support/json.h"
#include "xdec/types/binder.h"
#include "xdec/types/parse.h"
#include "xdec/types/syscall_table.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::Dominators;
using xdec::analysis::NaturalLoop;
using xdec::analysis::PostDominators;
using xdec::analysis::StackFrame;
using xdec::analysis::TypedVariables;
using xdec::analysis::VariableTable;
using xdec::emit::COptions;
using xdec::emit::printFunction;
using xdec::emit::structureFunction;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::Function;
using xdec::il::Type;
using xdec::types::NameAt;
using xdec::types::SyscallTable;
using xdec::types::TypeBinder;
using xdec::types::TypeDatabase;

namespace {

struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {
    entry = function.createBlock(0x1000);
    function.setEntryBlock(entry);
  }

  ExprId entryReg(std::string_view name) {
    return function.entryReg(function.registers().find(name));
  }

  /// The svc intrinsic as the spec lifts it, returning gettimeofday's raw
  /// (64-bit register) result -- the same shape tests/emit/test_c_syscall.cpp
  /// builds by hand.
  il::OpId gettimeofday() {
    std::vector<ExprId> operands{function.constant(Type::integer(16), 0),
                                 function.constant(Type::integer(64), 169),
                                 entryReg("x0"), entryReg("x1")};
    return function.appendIntrinsic(entry, 0x1000, xdec::passes::kSyscallIntrinsic,
                                    Type::integer(64), operands);
  }

  std::string emit(const TypeDatabase* database, const SyscallTable* syscalls,
                   const TypedVariables& typed) {
    function.rebuildEdges();
    const StackFrame frame = StackFrame::compute(function);
    VariableTable variables = VariableTable::recover(function, frame);
    if (database != nullptr) {
      const TypeBinder binder(*database, NameAt{});
      variables.applyImportedTypes(typed, binder);
    }
    const Dominators dominators = Dominators::compute(function);
    const PostDominators postDominators = PostDominators::compute(function);
    const std::vector<NaturalLoop> loops = naturalLoops(function, dominators);
    COptions options;
    options.types = database;
    options.syscalls = syscalls;
    options.typedVariables = &typed;
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

/// A syscall table with one entry: `number`'s `ret`, resolved against
/// `database` -- mirrors tests/analysis/test_typed_variables.cpp's helper of
/// the same shape.
[[nodiscard]] SyscallTable gettimeofdayTable(const TypeDatabase& database) {
  const xdec::Result<xdec::json::Value> document = xdec::json::parse(
      R"({"arch":"aarch64","syscalls":{"169":{"name":"gettimeofday","argc":2,"ret":"int"}}})");
  REQUIRE(document.hasValue());
  xdec::Result<SyscallTable> table = SyscallTable::fromJson(*document);
  REQUIRE(table.hasValue());
  table->resolveTypes(database);
  return std::move(*table);
}

}  // namespace

TEST_CASE("a syscall result declares at the callee's typed return, not the raw "
          "register width",
          "[emit][typed-variables]") {
  Fixture f;
  const TypeDatabase database;  // "int" is a builtin; no header needed.
  const SyscallTable syscalls = gettimeofdayTable(database);
  f.gettimeofday();
  // Returns something unrelated to the syscall result, so only the temp
  // declaration is under test here -- the function's own return type is
  // Phase 4.2's concern, covered separately below.
  const il::OpId ret = f.function.appendReturn(f.entry, 0x1004);
  f.function.setOperands(ret, std::vector<ExprId>{f.entryReg("x2")});
  const StackFrame frame = StackFrame::compute(f.function);
  const TypedVariables typed =
      TypedVariables::recover(f.function, frame, &database, &syscalls, NameAt{});
  const std::string text = f.emit(&database, &syscalls, typed);
  INFO(text);
  CHECK(contains(text, "int32_t t0;"));
  CHECK_FALSE(contains(text, "uint64_t t0;"));
}

TEST_CASE("a function's own return type follows an agreeing typed call result",
          "[emit][typed-variables]") {
  Fixture f;
  TypeDatabase database;
  const xdec::Result<xdec::types::ParseReport> report =
      xdec::types::parseHeader("struct Foo { int x; }; struct Foo* callee(void);", database);
  REQUIRE(report.hasValue());
  const NameAt names = [](uint64_t va) {
    return va == 0x2000 ? xdec::types::BoundName{"callee", true} : xdec::types::BoundName{};
  };

  const il::OpId call =
      f.function.appendCall(f.entry, 0x1000, f.function.constant(Type::integer(64), 0x2000),
                            Type::integer(64));
  const il::OpId ret = f.function.appendReturn(f.entry, 0x1004);
  f.function.setOperands(ret, std::vector<ExprId>{f.function.valueRef(f.function.op(call).result)});

  const StackFrame frame = StackFrame::compute(f.function);
  const TypedVariables typed = TypedVariables::recover(f.function, frame, &database, nullptr, names);
  const std::string text = f.emit(&database, nullptr, typed);
  INFO(text);
  CHECK(contains(text, "struct Foo* sub_1000("));
}

TEST_CASE("a disagreeing width leaves the function's own return type as inferred",
          "[emit][typed-variables]") {
  Fixture f;
  const TypeDatabase database;
  const SyscallTable syscalls = gettimeofdayTable(database);
  const il::OpId svc = f.gettimeofday();
  const il::OpId ret = f.function.appendReturn(f.entry, 0x1004);
  // The syscall's raw 64-bit result, returned whole -- the same shape
  // eval_svc_gettimeofday exercises, where the body's own use of the value
  // outranks the syscall table's narrower "success type".
  f.function.setOperands(ret,
                         std::vector<ExprId>{f.function.valueRef(f.function.op(svc).result)});
  const StackFrame frame = StackFrame::compute(f.function);
  const TypedVariables typed =
      TypedVariables::recover(f.function, frame, &database, &syscalls, NameAt{});
  const std::string text = f.emit(&database, &syscalls, typed);
  INFO(text);
  CHECK(contains(text, "uint64_t sub_1000("));
}
