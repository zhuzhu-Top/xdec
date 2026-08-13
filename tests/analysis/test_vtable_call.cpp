// matchVtableCallTarget / findConfirmedVtableCalls: recognising `call
// load(obj + slotOffset)`, confirmed once the same object reads more than
// one distinct slot.
#include <catch2/catch_test_macros.hpp>

#include "il/il_test_support.h"
#include "xdec/analysis/vtable_call.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::findConfirmedVtableCalls;
using xdec::analysis::matchVtableCallTarget;
using xdec::analysis::VtableCallSite;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::OpId;
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

  Function function;
  BlockId entry;
};

}  // namespace

TEST_CASE("a call through a loaded slot at obj+offset matches, offset and object recovered",
          "[analysis][vtable-call]") {
  Fixture f;
  const ExprId object = f.entryReg("x0");
  const ExprId address = f.function.binary(ExprOp::Add, object, f.i64(0x18));
  const il::ValueId target = f.function.appendLoad(f.entry, 0x1000, Type::integer(64), address);
  const OpId call = f.function.appendCall(f.entry, 0x1004, f.function.valueRef(target));

  const auto result = matchVtableCallTarget(f.function, call);
  REQUIRE(result.has_value());
  CHECK(result->call == call);
  CHECK(result->object == object);
  CHECK(result->slotOffset == 0x18);
}

TEST_CASE("a call through a bare loaded pointer (no address offset) matches at slot 0",
          "[analysis][vtable-call]") {
  Fixture f;
  const ExprId object = f.entryReg("x0");
  const il::ValueId target = f.function.appendLoad(f.entry, 0x1000, Type::integer(64), object);
  const OpId call = f.function.appendCall(f.entry, 0x1004, f.function.valueRef(target));

  const auto result = matchVtableCallTarget(f.function, call);
  REQUIRE(result.has_value());
  CHECK(result->object == object);
  CHECK(result->slotOffset == 0);
}

TEST_CASE("a direct call, and a call through a non-load value, do not match",
          "[analysis][vtable-call]") {
  Fixture f;
  const OpId direct = f.function.appendCall(f.entry, 0x1000, f.i64(0x5000));
  CHECK(!matchVtableCallTarget(f.function, direct).has_value());

  const OpId throughArithmetic = f.function.appendCall(
      f.entry, 0x1004, f.function.binary(ExprOp::Add, f.entryReg("x0"), f.i64(8)));
  CHECK(!matchVtableCallTarget(f.function, throughArithmetic).has_value());
}

TEST_CASE("one object read at two distinct slots across two call sites is a confirmed "
          "vtable dispatch",
          "[analysis][vtable-call]") {
  Fixture f;
  const ExprId object = f.entryReg("x0");
  const ExprId firstAddress = f.function.binary(ExprOp::Add, object, f.i64(0x10));
  const il::ValueId firstTarget =
      f.function.appendLoad(f.entry, 0x1000, Type::integer(64), firstAddress);
  const OpId firstCall = f.function.appendCall(f.entry, 0x1004, f.function.valueRef(firstTarget));

  const ExprId secondAddress = f.function.binary(ExprOp::Add, object, f.i64(0x20));
  const il::ValueId secondTarget =
      f.function.appendLoad(f.entry, 0x1008, Type::integer(64), secondAddress);
  const OpId secondCall =
      f.function.appendCall(f.entry, 0x100c, f.function.valueRef(secondTarget));

  const std::vector<VtableCallSite> confirmed = findConfirmedVtableCalls(f.function);
  REQUIRE(confirmed.size() == 2);
  CHECK(confirmed[0].object == object);
  CHECK(confirmed[1].object == object);
  const bool sawFirst = confirmed[0].call == firstCall || confirmed[1].call == firstCall;
  const bool sawSecond = confirmed[0].call == secondCall || confirmed[1].call == secondCall;
  CHECK(sawFirst);
  CHECK(sawSecond);
}

TEST_CASE("an object read at only one distinct slot, however many times, is not confirmed",
          "[analysis][vtable-call]") {
  Fixture f;
  const ExprId object = f.entryReg("x0");
  const ExprId address = f.function.binary(ExprOp::Add, object, f.i64(0x10));
  const il::ValueId firstTarget =
      f.function.appendLoad(f.entry, 0x1000, Type::integer(64), address);
  f.function.appendCall(f.entry, 0x1004, f.function.valueRef(firstTarget));
  const il::ValueId secondTarget =
      f.function.appendLoad(f.entry, 0x1008, Type::integer(64), address);
  f.function.appendCall(f.entry, 0x100c, f.function.valueRef(secondTarget));

  CHECK(findConfirmedVtableCalls(f.function).empty());
}

TEST_CASE("two different objects each seen at only one slot contribute nothing, "
          "even though the function has two call sites",
          "[analysis][vtable-call]") {
  Fixture f;
  const ExprId firstObject = f.entryReg("x0");
  const ExprId secondObject = f.entryReg("x1");
  const il::ValueId firstTarget = f.function.appendLoad(
      f.entry, 0x1000, Type::integer(64),
      f.function.binary(ExprOp::Add, firstObject, f.i64(0x10)));
  f.function.appendCall(f.entry, 0x1004, f.function.valueRef(firstTarget));
  const il::ValueId secondTarget = f.function.appendLoad(
      f.entry, 0x1008, Type::integer(64),
      f.function.binary(ExprOp::Add, secondObject, f.i64(0x18)));
  f.function.appendCall(f.entry, 0x100c, f.function.valueRef(secondTarget));

  CHECK(findConfirmedVtableCalls(f.function).empty());
}
