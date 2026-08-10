// matchLiveRegisterFrame / classifyHandlerExit: the two-phi-site relay a
// flattening dispatcher's handlers leave a register in, and the three ways
// one handler can relate to it.
#include <catch2/catch_test_macros.hpp>

#include <format>
#include <vector>

#include "il/il_test_support.h"
#include "xdec/analysis/live_register_frame.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::classifyHandlerExit;
using xdec::analysis::DispatcherShape;
using xdec::analysis::HandlerExitKind;
using xdec::analysis::LiveRegisterFrame;
using xdec::analysis::matchLiveRegisterFrame;
using xdec::analysis::unanimousPassthroughSlots;
using xdec::il::BlockId;
using xdec::il::Function;
using xdec::il::OpId;
using xdec::il::RegId;
using xdec::il::Type;

namespace {

struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {
    entry = function.createBlock(0x1000);
    function.setEntryBlock(entry);
    hub = function.createBlock(0x2000);
    h1 = function.createBlock(0x3000);
    h2 = function.createBlock(0x3100);
    h3 = function.createBlock(0x3200);
    merge = function.createBlock(0x4000);
  }

  RegId reg(unsigned index) { return function.registers().find(std::format("x{}", index)); }

  /// Builds the relay for x0..x7: `hub` carries one phi per register (fed by
  /// `entry` and by `merge`'s own back edge), `merge` carries a second phi
  /// per register (fed by every handler in `mergeHandlers`, in order).
  /// `mergeOperand(reg, handlerIndex, hubPhi)` supplies each merge phi's
  /// operand for that handler; passing the hub phi's own result value back
  /// unmodified is exactly what a passthrough handler's exit looks like.
  template <class MergeOperandFn>
  void buildFrame(const std::vector<BlockId>& mergeHandlers, MergeOperandFn mergeOperand) {
    function.appendBranch(entry, 0x1000, hub);
    function.appendBranch(hub, 0x2000, h1);
    function.appendBranch(h1, 0x3000, merge);
    function.appendBranch(h2, 0x3100, merge);
    function.appendReturn(h3, 0x3200);
    function.appendBranch(merge, 0x4000, hub);
    function.rebuildEdges();

    std::vector<OpId> hubPhis;
    for (unsigned index = 0; index < 8; ++index) {
      const il::ValueId phiValue = function.prependPhi(hub, 0x2000, Type::integer(64));
      const OpId phiOp = function.value(phiValue).definition;
      function.annotate(phiOp, std::format("reg:{}", function.registers().nameOf(reg(index))));
      // hub.predecessors == [entry, merge] (ascending source BlockId, see
      // Function::rebuildEdges): the entry edge's value is never read by
      // anything under test, so any distinct placeholder does.
      function.setOperands(phiOp, std::vector<il::ExprId>{function.entryReg(reg(index)),
                                                          function.undefined(Type::integer(64))});
      hubPhis.push_back(phiOp);
    }

    for (unsigned index = 0; index < 8; ++index) {
      const il::ValueId phiValue = function.prependPhi(merge, 0x4000, Type::integer(64));
      const OpId phiOp = function.value(phiValue).definition;
      function.annotate(phiOp, std::format("reg:{}", function.registers().nameOf(reg(index))));
      std::vector<il::ExprId> operands;
      for (std::size_t handlerIndex = 0; handlerIndex < mergeHandlers.size(); ++handlerIndex) {
        operands.push_back(mergeOperand(index, handlerIndex, hubPhis[index]));
      }
      function.setOperands(phiOp, operands);
    }
  }

  Function function;
  BlockId entry;
  BlockId hub;
  BlockId h1;
  BlockId h2;
  BlockId h3;
  BlockId merge;
};

}  // namespace

TEST_CASE("a handler that copies every register straight through is Passthrough",
          "[analysis][live-register-frame]") {
  Fixture f;
  // merge.predecessors == [h1, h2] (both branch to it; h3 returns outright
  // and never reaches merge at all).
  f.buildFrame({f.h1, f.h2}, [&](unsigned, std::size_t, OpId hubPhi) {
    return f.function.valueRef(f.function.op(hubPhi).result);
  });
  f.function.rebuildEdges();

  const DispatcherShape shape{f.entry, f.merge, f.hub};
  const auto frame = matchLiveRegisterFrame(f.function, shape);
  REQUIRE(frame.has_value());
  REQUIRE(frame->slots.size() == 8);

  const auto exit = classifyHandlerExit(f.function, f.h1, shape, *frame);
  CHECK(exit.kind == HandlerExitKind::Passthrough);
  for (const bool unchanged : exit.unchanged) {
    CHECK(unchanged);
  }
}

TEST_CASE("a handler that changes one register and passes the rest through is Partial",
          "[analysis][live-register-frame]") {
  Fixture f;
  const il::ExprId changed = f.function.constant(Type::integer(64), 0x1234);
  // handlerIndex 0 is h1 (untouched, passthrough); handlerIndex 1 is h2,
  // which only overwrites x0 -- a call result landing there, say.
  f.buildFrame({f.h1, f.h2}, [&](unsigned index, std::size_t handlerIndex, OpId hubPhi) {
    if (handlerIndex == 1 && index == 0) {
      return changed;
    }
    return f.function.valueRef(f.function.op(hubPhi).result);
  });
  f.function.rebuildEdges();

  const DispatcherShape shape{f.entry, f.merge, f.hub};
  const auto frame = matchLiveRegisterFrame(f.function, shape);
  REQUIRE(frame.has_value());

  const auto exit = classifyHandlerExit(f.function, f.h2, shape, *frame);
  CHECK(exit.kind == HandlerExitKind::Partial);
  CHECK_FALSE(exit.unchanged[0]);
  for (std::size_t slot = 1; slot < frame->slots.size(); ++slot) {
    CHECK(exit.unchanged[slot]);
  }
}

TEST_CASE("a slot every handler leaves alone is unanimous; one that any handler changes is not",
          "[analysis][live-register-frame]") {
  Fixture f;
  const il::ExprId changed = f.function.constant(Type::integer(64), 0x1234);
  // Only h2 touches x0 (handlerIndex 1, index 0); every other register on
  // every handler passes straight through.
  f.buildFrame({f.h1, f.h2}, [&](unsigned index, std::size_t handlerIndex, OpId hubPhi) {
    if (handlerIndex == 1 && index == 0) {
      return changed;
    }
    return f.function.valueRef(f.function.op(hubPhi).result);
  });
  f.function.rebuildEdges();

  const DispatcherShape shape{f.entry, f.merge, f.hub};
  const auto frame = matchLiveRegisterFrame(f.function, shape);
  REQUIRE(frame.has_value());

  const std::vector<bool> unanimous = unanimousPassthroughSlots(f.function, shape, *frame);
  REQUIRE(unanimous.size() == 8);
  CHECK_FALSE(unanimous[0]);
  for (std::size_t slot = 1; slot < unanimous.size(); ++slot) {
    CHECK(unanimous[slot]);
  }
}

TEST_CASE("a handler that never reaches the shared tail is Return",
          "[analysis][live-register-frame]") {
  Fixture f;
  f.buildFrame({f.h1, f.h2}, [&](unsigned, std::size_t, OpId hubPhi) {
    return f.function.valueRef(f.function.op(hubPhi).result);
  });
  f.function.rebuildEdges();

  const DispatcherShape shape{f.entry, f.merge, f.hub};
  const auto frame = matchLiveRegisterFrame(f.function, shape);
  REQUIRE(frame.has_value());

  const auto exit = classifyHandlerExit(f.function, f.h3, shape, *frame);
  CHECK(exit.kind == HandlerExitKind::Return);
}

TEST_CASE("no register relayed through both hub and merge never matches the frame",
          "[analysis][live-register-frame]") {
  // hub and merge each carry ordinary, unrelated phis (not a shared
  // register): this is not the dispatcher relay, and nothing here should
  // guess that it is.
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId entry = function.createBlock(0x1000);
  function.setEntryBlock(entry);
  const BlockId hub = function.createBlock(0x2000);
  const BlockId merge = function.createBlock(0x3000);
  function.appendReturn(hub, 0x2000);
  function.appendReturn(merge, 0x3000);
  function.rebuildEdges();

  const DispatcherShape shape{entry, merge, hub};
  CHECK_FALSE(matchLiveRegisterFrame(function, shape).has_value());
}
