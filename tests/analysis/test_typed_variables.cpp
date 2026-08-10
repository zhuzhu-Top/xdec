// TypedVariables: type evidence traced from Call/svc sites back to entry
// registers, stack slots and SSA values, plus the VariableTable integration
// that turns a typed stack slot into a promoted struct local (and its
// sibling accesses into named fields of it).
#include <catch2/catch_test_macros.hpp>

#include "il/il_test_support.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/analysis/typed_variables.h"
#include "xdec/analysis/variables.h"
#include "xdec/il/function.h"
#include "xdec/support/json.h"
#include "xdec/types/binder.h"
#include "xdec/types/parse.h"
#include "xdec/types/syscall_table.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::CalleeSummaries;
using xdec::analysis::CalleeSummary;
using xdec::analysis::StackFrame;
using xdec::analysis::TypedVariables;
using xdec::analysis::Variable;
using xdec::analysis::VariableTable;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::RegId;
using xdec::il::Type;
using xdec::types::NameAt;
using xdec::types::SyscallTable;
using xdec::types::TypeBinder;
using xdec::types::TypeDatabase;
using xdec::types::TypeId;

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
    return delta < 0
               ? function.binary(ExprOp::Sub, sp, i64(static_cast<uint64_t>(-delta)))
               : function.binary(ExprOp::Add, sp, i64(static_cast<uint64_t>(delta)));
  }

  Function function;
  BlockId entry;
};

[[nodiscard]] TypeDatabase parse(std::string_view header) {
  TypeDatabase database;
  const xdec::Result<xdec::types::ParseReport> report = xdec::types::parseHeader(header, database);
  INFO((report ? report->format("<test>") : report.error().format()));
  REQUIRE(report);
  return database;
}

/// A syscall table with one entry: `number`, `argTypes` resolved against
/// `database`.
[[nodiscard]] SyscallTable syscallTableOf(const TypeDatabase& database, uint32_t number,
                                          std::string_view name,
                                          std::span<const std::string_view> argTypes) {
  std::string args;
  for (const std::string_view type : argTypes) {
    args += args.empty() ? "\"" : ",\"";
    args += type;
    args += '"';
  }
  const std::string text = std::format(
      R"({{"arch":"aarch64","syscalls":{{"{}":{{"name":"{}","argc":{},"args":[{}]}}}}}})", number,
      name, argTypes.size(), args);
  const xdec::Result<xdec::json::Value> document = xdec::json::parse(text);
  REQUIRE(document.hasValue());
  xdec::Result<SyscallTable> table = SyscallTable::fromJson(*document);
  REQUIRE(table.hasValue());
  table->resolveTypes(database);
  return std::move(*table);
}

/// `entry` is the only address `names` gives a symbol; it is a function under
/// `symbolName` so a call to it binds to whatever prototype was declared for
/// that name.
[[nodiscard]] NameAt namesFor(uint64_t address, std::string_view symbolName) {
  return [address, name = std::string{symbolName}](uint64_t va) {
    return va == address ? xdec::types::BoundName{name, true} : xdec::types::BoundName{};
  };
}

}  // namespace

TEST_CASE("a syscall argument types the stack slot its address points at",
          "[analysis][typed-variables]") {
  Fixture f;
  // The struct alone is not enough: nothing interns "struct timeval*" (see
  // TypeDatabase::findPointerTo) until some declaration actually takes one,
  // the same way the real android-ndk.hdecl's own gettimeofday prototype is
  // what makes SyscallTable::resolveTypes able to resolve the syscall
  // table's identical spelling.
  const TypeDatabase database =
      parse("struct timeval { long tv_sec; long tv_usec; };\n"
            "void gettimeofday_proto(struct timeval *tv, void *tz);");
  const SyscallTable syscalls =
      syscallTableOf(database, 169, "gettimeofday",
                    std::array<std::string_view, 2>{"struct timeval*", "void*"});

  const std::vector<ExprId> args{
      f.i64(0),                        // imm
      f.i64(169),                      // x8: syscall number
      f.slot(-0x50),                   // x0: &tv
      f.i64(0),                        // x1: tz
  };
  f.function.appendIntrinsic(f.entry, 0x1000, "aarch64.svc", Type::integer(64), args);
  f.function.appendReturn(f.entry, 0x1004);

  const StackFrame frame = StackFrame::compute(f.function);
  const TypedVariables typed =
      TypedVariables::recover(f.function, frame, &database, &syscalls, NameAt{});
  const std::optional<TypeId> found = typed.forStackSlot(-0x50);
  REQUIRE(found.has_value());
  CHECK(*found == database.lookup("timeval", xdec::types::NameSpace::Tag));
}

TEST_CASE("a direct call types the entry register its argument reads",
          "[analysis][typed-variables]") {
  Fixture f;
  const TypeDatabase database =
      parse("struct Foo { int x; }; void callee(struct Foo *p);");
  const NameAt names = namesFor(0x2000, "callee");

  const il::OpId call =
      f.function.appendCall(f.entry, 0x1000, f.i64(0x2000), Type::voidType());
  const std::vector<ExprId> operands{f.i64(0x2000), f.function.entryReg(f.reg("x0"))};
  f.function.setOperands(call, operands);
  f.function.appendReturn(f.entry, 0x1004);

  const StackFrame frame = StackFrame::compute(f.function);
  const TypedVariables typed =
      TypedVariables::recover(f.function, frame, /*database=*/&database,
                              /*syscalls=*/nullptr, names);
  const std::optional<TypeId> found = typed.forArgument(f.reg("x0"));
  REQUIRE(found.has_value());
  const std::optional<TypeId> pointerToFoo =
      database.findPointerTo(database.lookup("Foo", xdec::types::NameSpace::Tag));
  REQUIRE(pointerToFoo.has_value());
  CHECK(*found == *pointerToFoo);
}

TEST_CASE("a call through a GOT/import slot types its argument the same as a "
          "direct call",
          "[analysis][typed-variables]") {
  Fixture f;
  const TypeDatabase database =
      parse("struct Foo { int x; }; void callee(struct Foo *p);");

  // x8 = *(slot), then the call target is that loaded value -- the shape an
  // `adrp`+`ldr`+`blr` sequence lifts to once the slot's address folds to a
  // constant. No symbol names the slot itself; only the loader's relocation
  // does, which is what MemoryFacts::loader stands in for here.
  const il::ValueId slotValue = f.function.appendLoad(f.entry, 0x1000, Type::integer(64), f.i64(0x3000));
  const il::OpId call =
      f.function.appendCall(f.entry, 0x1004, f.function.valueRef(slotValue), Type::voidType());
  f.function.setOperands(
      call, std::vector<ExprId>{f.function.valueRef(slotValue), f.function.entryReg(f.reg("x0"))});
  f.function.appendReturn(f.entry, 0x1008);

  xdec::MemoryFacts memory;
  memory.loader = [](uint64_t va) {
    xdec::LoaderValue value;
    if (va == 0x3000) {
      value.importName = "callee";
    }
    return value;
  };

  const StackFrame frame = StackFrame::compute(f.function);
  const TypedVariables typed = TypedVariables::recover(f.function, frame, &database, nullptr,
                                                        NameAt{}, {}, memory);
  const std::optional<TypeId> found = typed.forArgument(f.reg("x0"));
  REQUIRE(found.has_value());
  const std::optional<TypeId> pointerToFoo =
      database.findPointerTo(database.lookup("Foo", xdec::types::NameSpace::Tag));
  REQUIRE(pointerToFoo.has_value());
  CHECK(*found == *pointerToFoo);
}

TEST_CASE("a value loaded from a GOT/import slot naming a global is typed as "
          "a pointer to it",
          "[analysis][typed-variables]") {
  Fixture f;
  // `useFoo` is never called; it exists only so some declaration spells
  // `struct Foo*`, which is what interns the pointer type findPointerTo
  // looks up (see globalPointerType's own comment on why it may not intern
  // one itself).
  const TypeDatabase database =
      parse("struct Foo { int x; }; extern struct Foo g_foo; void useFoo(struct Foo *p);");

  const il::ValueId slotValue = f.function.appendLoad(f.entry, 0x1000, Type::integer(64), f.i64(0x3000));
  f.function.appendReturn(f.entry, 0x1004);

  xdec::MemoryFacts memory;
  memory.loader = [](uint64_t va) {
    xdec::LoaderValue value;
    if (va == 0x3000) {
      value.importName = "g_foo";
    }
    return value;
  };

  const StackFrame frame = StackFrame::compute(f.function);
  const TypedVariables typed =
      TypedVariables::recover(f.function, frame, &database, nullptr, NameAt{}, {}, memory);
  const std::optional<TypeId> found = typed.forValue(slotValue);
  REQUIRE(found.has_value());
  const std::optional<TypeId> pointerToFoo =
      database.findPointerTo(database.lookup("Foo", xdec::types::NameSpace::Tag));
  REQUIRE(pointerToFoo.has_value());
  CHECK(*found == *pointerToFoo);
}

TEST_CASE("a value loaded from a GOT slot naming a function is not typed as "
          "a global pointer",
          "[analysis][typed-variables]") {
  Fixture f;
  const TypeDatabase database = parse("struct Foo { int x; }; void callee(struct Foo *p);");

  const il::ValueId slotValue = f.function.appendLoad(f.entry, 0x1000, Type::integer(64), f.i64(0x3000));
  f.function.appendReturn(f.entry, 0x1004);

  xdec::MemoryFacts memory;
  memory.loader = [](uint64_t va) {
    xdec::LoaderValue value;
    if (va == 0x3000) {
      value.importName = "callee";
    }
    return value;
  };

  const StackFrame frame = StackFrame::compute(f.function);
  const TypedVariables typed =
      TypedVariables::recover(f.function, frame, &database, nullptr, NameAt{}, {}, memory);
  CHECK_FALSE(typed.forValue(slotValue).has_value());
}

TEST_CASE("evidence reaches every phi operand a typed argument merges through",
          "[analysis][typed-variables]") {
  Fixture f;
  const TypeDatabase database = parse("struct Foo { int x; }; void callee(struct Foo *p);");
  const NameAt names = namesFor(0x2000, "callee");

  const BlockId header = f.function.createBlock(0x2000);
  f.function.appendBranch(f.entry, 0x1000, header);
  const il::OpId phiOp = f.function.appendPhi(
      header, 0x2000, Type::integer(64),
      std::vector<ExprId>{f.function.entryReg(f.reg("x0")), f.function.entryReg(f.reg("x1"))});
  const ExprId merged = f.function.valueRef(f.function.op(phiOp).result);
  const il::OpId call = f.function.appendCall(header, 0x2004, f.i64(0x2000), Type::voidType());
  f.function.setOperands(call, std::vector<ExprId>{f.i64(0x2000), merged});
  f.function.appendReturn(header, 0x2008);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const TypedVariables typed = TypedVariables::recover(f.function, frame, &database, nullptr, names);
  CHECK(typed.forArgument(f.reg("x0")).has_value());
  CHECK(typed.forArgument(f.reg("x1")).has_value());
}

TEST_CASE("a call's result is typed by the callee's return", "[analysis][typed-variables]") {
  Fixture f;
  const TypeDatabase database = parse("struct Foo { int x; }; struct Foo *callee(void);");
  const NameAt names = namesFor(0x2000, "callee");

  const il::OpId call =
      f.function.appendCall(f.entry, 0x1000, f.i64(0x2000), Type::integer(64));
  f.function.appendReturn(f.entry, 0x1004);

  const StackFrame frame = StackFrame::compute(f.function);
  const TypedVariables typed = TypedVariables::recover(f.function, frame, &database, nullptr, names);
  const std::optional<TypeId> found = typed.forValue(f.function.op(call).result);
  REQUIRE(found.has_value());
  const std::optional<TypeId> pointerToFoo =
      database.findPointerTo(database.lookup("Foo", xdec::types::NameSpace::Tag));
  REQUIRE(pointerToFoo.has_value());
  CHECK(*found == *pointerToFoo);
}

TEST_CASE("the function's own return type follows an agreeing typed call result",
          "[analysis][typed-variables]") {
  Fixture f;
  const TypeDatabase database = parse("int callee(void);");
  const NameAt names = namesFor(0x2000, "callee");

  const il::OpId call =
      f.function.appendCall(f.entry, 0x1000, f.i64(0x2000), Type::integer(32));
  const ExprId result = f.function.valueRef(f.function.op(call).result);
  const ExprId widened = f.function.cast(ExprOp::ZExt, Type::integer(64), result);
  const il::OpId ret = f.function.appendReturn(f.entry, 0x1004);
  f.function.setOperands(ret, std::vector<ExprId>{widened});

  const StackFrame frame = StackFrame::compute(f.function);
  const TypedVariables typed = TypedVariables::recover(f.function, frame, &database, nullptr, names);
  REQUIRE(typed.returnType().has_value());
  CHECK(*typed.returnType() == database.lookup("int"));
}

TEST_CASE("disagreeing return paths report no evidence", "[analysis][typed-variables]") {
  Fixture f;
  const TypeDatabase database = parse("int callee(void);");
  const NameAt names = namesFor(0x2000, "callee");

  const BlockId untyped = f.function.createBlock(0x3000);
  const BlockId typedBlock = f.function.createBlock(0x4000);
  const ExprId cond =
      f.function.binary(ExprOp::CmpEq, f.function.entryReg(f.reg("x0")), f.i64(0));
  f.function.appendCondBranch(f.entry, 0x1000, cond, untyped, typedBlock);

  const il::OpId call =
      f.function.appendCall(typedBlock, 0x4000, f.i64(0x2000), Type::integer(32));
  const ExprId widened =
      f.function.cast(ExprOp::ZExt, Type::integer(64), f.function.valueRef(f.function.op(call).result));
  const il::OpId typedRet = f.function.appendReturn(typedBlock, 0x4004);
  f.function.setOperands(typedRet, std::vector<ExprId>{widened});

  // A different path returns whatever it was handed -- not the callee's
  // result, so nothing here says its type agrees.
  const il::OpId untypedRet = f.function.appendReturn(untyped, 0x3000);
  f.function.setOperands(untypedRet, std::vector<ExprId>{f.function.entryReg(f.reg("x1"))});
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const TypedVariables typed = TypedVariables::recover(f.function, frame, &database, nullptr, names);
  CHECK_FALSE(typed.returnType().has_value());
}

TEST_CASE("summaryOf exports a decompiled function's own parameter and "
          "return evidence",
          "[analysis][typed-variables]") {
  Fixture f;
  const TypeDatabase database = parse("struct Foo { int x; }; void callee(struct Foo *p);");
  const NameAt names = namesFor(0x2000, "callee");

  // x0 is passed straight into `callee`, which types it; x1 is read by
  // nothing a header describes, so its slot in the summary stays empty.
  const il::OpId call = f.function.appendCall(f.entry, 0x1000, f.i64(0x2000), Type::voidType());
  f.function.setOperands(call, std::vector<ExprId>{f.i64(0x2000), f.function.entryReg(f.reg("x0")),
                                                    f.function.entryReg(f.reg("x1"))});
  f.function.appendReturn(f.entry, 0x1004);

  const StackFrame frame = StackFrame::compute(f.function);
  VariableTable variables = VariableTable::recover(f.function, frame);
  const TypedVariables typed = TypedVariables::recover(f.function, frame, &database, nullptr, names);
  const TypeBinder binder(database, names);
  variables.applyImportedTypes(typed, binder);

  const CalleeSummary summary = xdec::analysis::summaryOf(variables, typed);
  const std::optional<TypeId> pointerToFoo =
      database.findPointerTo(database.lookup("Foo", xdec::types::NameSpace::Tag));
  REQUIRE(pointerToFoo.has_value());
  REQUIRE(summary.paramTypes.size() >= 2);
  CHECK(summary.paramTypes[0] == *pointerToFoo);
  CHECK_FALSE(summary.paramTypes[1].valid());
}

TEST_CASE("a cached summary from an already-decompiled helper types a call "
          "to it in another function",
          "[analysis][typed-variables]") {
  const TypeDatabase database = parse("struct Foo { int x; }; void callee(struct Foo *p);");
  const NameAt names = namesFor(0x2000, "callee");

  // `helper`, decompiled first: it just forwards its own x0 into `callee`,
  // so summaryOf reports its first parameter as `struct Foo*` too.
  Fixture helper;
  const il::OpId innerCall =
      helper.function.appendCall(helper.entry, 0x2000, helper.i64(0x3000), Type::voidType());
  helper.function.setOperands(
      innerCall, std::vector<ExprId>{helper.i64(0x3000), helper.function.entryReg(helper.reg("x0"))});
  helper.function.appendReturn(helper.entry, 0x2004);
  const NameAt namesWithCallee = [](uint64_t va) {
    return va == 0x3000 ? xdec::types::BoundName{"callee", true} : xdec::types::BoundName{};
  };
  const StackFrame helperFrame = StackFrame::compute(helper.function);
  VariableTable helperVars = VariableTable::recover(helper.function, helperFrame);
  const TypedVariables helperTyped =
      TypedVariables::recover(helper.function, helperFrame, &database, nullptr, namesWithCallee);
  const TypeBinder binder(database, namesWithCallee);
  helperVars.applyImportedTypes(helperTyped, binder);

  CalleeSummaries summaries;
  summaries[0x2000] = xdec::analysis::summaryOf(helperVars, helperTyped);

  // `caller`, decompiled next in the same run: it calls `helper` directly, at
  // an address no header names -- only the summary knows what it takes.
  Fixture caller;
  const il::OpId outerCall =
      caller.function.appendCall(caller.entry, 0x1000, caller.i64(0x2000), Type::voidType());
  caller.function.setOperands(
      outerCall, std::vector<ExprId>{caller.i64(0x2000), caller.function.entryReg(caller.reg("x0"))});
  caller.function.appendReturn(caller.entry, 0x1004);

  const StackFrame callerFrame = StackFrame::compute(caller.function);
  const TypedVariables callerTyped = TypedVariables::recover(
      caller.function, callerFrame, &database, nullptr, NameAt{}, summaries);
  const std::optional<TypeId> found = callerTyped.forArgument(caller.reg("x0"));
  REQUIRE(found.has_value());
  const std::optional<TypeId> pointerToFoo =
      database.findPointerTo(database.lookup("Foo", xdec::types::NameSpace::Tag));
  REQUIRE(pointerToFoo.has_value());
  CHECK(*found == *pointerToFoo);
}

TEST_CASE("a typed stack slot promotes to a struct local, and a sibling load "
          "becomes a named field",
          "[analysis][typed-variables][vars]") {
  Fixture f;
  const TypeDatabase database =
      parse("struct timeval { long tv_sec; long tv_usec; };\n"
            "void gettimeofday_proto(struct timeval *tv, void *tz);");
  const SyscallTable syscalls =
      syscallTableOf(database, 169, "gettimeofday",
                    std::array<std::string_view, 2>{"struct timeval*", "void*"});

  const std::vector<ExprId> args{f.i64(0), f.i64(169), f.slot(-0x50), f.i64(0)};
  f.function.appendIntrinsic(f.entry, 0x1000, "aarch64.svc", Type::integer(64), args);
  // tv_sec and tv_usec, each read back out of the buffer the syscall filled.
  f.function.appendLoad(f.entry, 0x1004, Type::integer(64), f.slot(-0x50));
  f.function.appendLoad(f.entry, 0x1008, Type::integer(64), f.slot(-0x48));
  f.function.appendReturn(f.entry, 0x100c);

  const StackFrame frame = StackFrame::compute(f.function);
  VariableTable table = VariableTable::recover(f.function, frame);
  const TypedVariables typed =
      TypedVariables::recover(f.function, frame, &database, &syscalls, NameAt{});
  const TypeBinder binder(database, NameAt{});
  table.applyImportedTypes(typed, binder);

  const Variable* base = table.localAt(-0x50);
  REQUIRE(base != nullptr);
  REQUIRE(base->importedType.has_value());
  CHECK(database.format(*base->importedType) == "struct timeval");
  CHECK_FALSE(base->aliasBase.has_value());

  const Variable* usec = table.localAt(-0x48);
  REQUIRE(usec != nullptr);
  REQUIRE(usec->aliasBase.has_value());
  CHECK(*usec->aliasBase == -0x50);
  CHECK(usec->aliasField == "tv_usec");
  CHECK_FALSE(usec->importedType.has_value());
}
