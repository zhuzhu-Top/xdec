// StackFrame: address classification and the three-valued alias oracle.
#include <catch2/catch_test_macros.hpp>

#include "il/il_test_support.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::AddressKind;
using xdec::analysis::AliasResult;
using xdec::analysis::StackFrame;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::RegId;
using xdec::il::Type;

namespace {

struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {}

  RegId reg(std::string_view name) { return function.registers().find(name); }
  ExprId i64(uint64_t value) { return function.constant(Type::integer(64), value); }

  Function function;
};

TEST_CASE("sp-derived chains classify with their total displacement", "[analysis][stack]") {
  Fixture f;
  const ExprId entrySp = f.function.entryReg(f.reg("sp"));
  // sub(entry(sp), 0x80) + 0x18, the shape a prologue plus access builds.
  const ExprId based = f.function.binary(
      ExprOp::Add, f.function.binary(ExprOp::Sub, entrySp, f.i64(0x80)), f.i64(0x18));
  // The other association, as a scaled access would build it.
  const ExprId reassociated = f.function.binary(
      ExprOp::Sub, f.function.binary(ExprOp::Add, entrySp, f.i64(0x10)), f.i64(0x30));

  const StackFrame frame = StackFrame::compute(f.function);
  REQUIRE(frame.stackPointer() == f.reg("sp"));

  const auto first = frame.classify(based);
  REQUIRE(first.kind == AddressKind::StackSlot);
  CHECK(first.delta == -0x68);

  const auto second = frame.classify(reassociated);
  REQUIRE(second.kind == AddressKind::StackSlot);
  CHECK(second.delta == -0x20);

  // A constant address is a global; a loaded pointer is Other.
  const auto global = frame.classify(f.i64(0x9000));
  CHECK(global.kind == AddressKind::Global);
  CHECK(global.address == 0x9000);

  // Another register's entry leaf is not the stack pointer.
  const auto other = frame.classify(
      f.function.binary(ExprOp::Add, f.function.entryReg(f.reg("x0")), f.i64(8)));
  CHECK(other.kind == AddressKind::Other);
}

TEST_CASE("the alias oracle is three-valued and frame-image disjoint", "[analysis][stack]") {
  Fixture f;
  const ExprId entrySp = f.function.entryReg(f.reg("sp"));
  const ExprId slotA = f.function.binary(ExprOp::Add, entrySp, f.i64(0x10));
  const ExprId slotAgain = f.function.binary(ExprOp::Add, entrySp, f.i64(0x10));
  const ExprId slotNear = f.function.binary(ExprOp::Add, entrySp, f.i64(0x14));
  const ExprId slotFar = f.function.binary(ExprOp::Add, entrySp, f.i64(0x40));
  const ExprId global = f.i64(0x10);
  const ExprId pointer =
      f.function.binary(ExprOp::Add, f.function.entryReg(f.reg("x0")), f.i64(0));

  const StackFrame frame = StackFrame::compute(f.function);
  // Same expression, same size: Must. (slotA and slotAgain are one hash-consed
  // node; the sizes disambiguate the range question.)
  CHECK(frame.mayAlias(slotA, 8, slotAgain, 8) == AliasResult::Must);
  // Overlapping ranges without a shared base: May.
  CHECK(frame.mayAlias(slotA, 8, slotNear, 8) == AliasResult::May);
  // Disjoint slots: No.
  CHECK(frame.mayAlias(slotA, 8, slotFar, 8) == AliasResult::No);
  // Frame and image never meet, even at the same numeric address.
  CHECK(frame.mayAlias(slotA, 8, global, 8) == AliasResult::No);
  // An unclassified pointer may reach anything.
  CHECK(frame.mayAlias(slotA, 8, pointer, 8) == AliasResult::May);
  CHECK(frame.mayAlias(pointer, 8, global, 8) == AliasResult::May);
}

}  // namespace
