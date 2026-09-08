// annotate-stack-canary: the note a save/reload round trip earns once the
// whole stock pipeline has settled the IL down to Vars (see
// analysis/test_stack_canary.cpp for the shape itself, tested in isolation;
// this file is about the pass wiring it into the pipeline and the emitter's
// note).
#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>

#include "il/il_test_support.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/il/function.h"
#include "xdec/il/verify.h"
#include "xdec/pass/manager.h"
#include "xdec/pass/registry.h"
#include "xdec/passes/builtin.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::Maturity;
using xdec::il::OpCode;
using xdec::il::OpId;
using xdec::il::Type;

namespace {

struct Builder {
  Builder() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {}

  BlockId block(uint64_t va) {
    const BlockId id = function.createBlock(va);
    if (!function.entryBlock().valid()) {
      function.setEntryBlock(id);
    }
    return id;
  }

  ExprId i64(uint64_t value) { return function.constant(Type::integer(64), value); }
  ExprId sp() { return function.entryReg(function.registers().find("sp")); }
  ExprId slot(int64_t delta) {
    return delta < 0 ? function.binary(ExprOp::Sub, sp(), i64(static_cast<uint64_t>(-delta)))
                     : function.binary(ExprOp::Add, sp(), i64(static_cast<uint64_t>(delta)));
  }
  ExprId load(BlockId at, uint64_t va, ExprId address) {
    return function.valueRef(function.appendLoad(at, va, Type::integer(64), address));
  }

  void atCfg() {
    function.rebuildEdges();
    function.setMaturity(Maturity::Cfg);
  }

  Function function;
};

/// The whole stock pipeline, up to and including vars. No image is wired: the
/// only addresses these tests touch are plain constants, so nothing needs
/// reading.
void runToVars(Function& function) {
  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);
  xdec::pass::Manager manager;
  manager.setImage([](uint64_t, std::span<std::byte>) -> xdec::Result<void> {
    return xdec::err(xdec::DiagCode::Internal, "no image in this test");
  });
  auto ran = manager.runTo(function, registry, Maturity::Vars);
  const std::string error = ran ? std::string{} : ran.error().format();
  INFO(error);
  REQUIRE(ran);
  const il::VerifyReport report = il::verify(function, Maturity::Vars);
  for (const xdec::Diag& diag : report.errors) {
    INFO(diag.format());
  }
  REQUIRE(report.ok());
}

/// The one Store left writing to `delta` -- the canary save, once vars has
/// settled the stack layout.
[[nodiscard]] OpId storeAt(const Function& function, int64_t delta) {
  const xdec::analysis::StackFrame frame = xdec::analysis::StackFrame::compute(function);
  for (const BlockId blockId : function.blockHandles()) {
    for (const OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      if (op.code != OpCode::Store) {
        continue;
      }
      const auto operands = function.operands(op);
      if (frame.classify(operands[0]).kind == xdec::analysis::AddressKind::StackSlot &&
          frame.classify(operands[0]).delta == delta) {
        return opId;
      }
    }
  }
  return OpId::invalid();
}

}  // namespace

TEST_CASE("a canary save/reload round trip is annotated once the pipeline settles",
          "[passes][annotate-stack-canary]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  const OpId save = b.function.appendStore(entry, 0x1000, Type::integer(64), b.slot(-0x10),
                                           b.load(entry, 0x1000, b.i64(0x2000)));

  // A call in between, exactly like the real shape's own body: with nothing
  // opaque between the save and the reload, forwarding the store straight
  // through the load would (rightly) prove the compare always false and fold
  // it away before this pass ever runs -- a call is what makes "did this
  // change" a real question again.
  b.function.appendCall(entry, 0x1004, b.i64(0x4000), Type::integer(64));

  const BlockId ok = b.block(0x1100);
  const BlockId fail = b.block(0x1200);
  const ExprId reload = b.load(entry, 0x1008, b.i64(0x2000));
  const ExprId saved = b.load(entry, 0x100c, b.slot(-0x10));
  const ExprId cond = b.function.binary(ExprOp::CmpNe, reload, saved);
  b.function.appendCondBranch(entry, 0x1010, cond, fail, ok);
  b.function.appendReturn(ok, 0x1100);
  b.function.appendReturn(fail, 0x1200);
  b.atCfg();

  runToVars(b.function);

  const OpId settledSave = storeAt(b.function, -0x10);
  REQUIRE(settledSave.valid());
  const std::string_view note = b.function.noteOn(settledSave);
  CHECK(note.find("stack canary") != std::string_view::npos);
  CHECK(note.find("0x2000") != std::string_view::npos);
  (void)save;
}

TEST_CASE("an ordinary saved local with no matching reload is left alone",
          "[passes][annotate-stack-canary]") {
  Builder b;
  const BlockId entry = b.block(0x1000);
  b.function.appendStore(entry, 0x1000, Type::integer(64), b.slot(-0x10),
                         b.load(entry, 0x1000, b.i64(0x2000)));
  b.function.appendReturn(entry, 0x1004);
  b.atCfg();

  runToVars(b.function);

  const OpId settledSave = storeAt(b.function, -0x10);
  REQUIRE(settledSave.valid());
  CHECK(b.function.noteOn(settledSave).empty());
}
