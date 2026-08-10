// See the header for the protocol this looks for and why.
#include "xdec/analysis/live_register_frame.h"

#include <algorithm>
#include <format>
#include <string_view>

namespace xdec::analysis {

namespace {

constexpr std::string_view kRegisterNotePrefix = "reg:";

/// The register a phi's own "reg:xN" note names, or an invalid handle when
/// it has none (an untracked class such as flags, or a phi some other pass
/// placed without this annotation).
[[nodiscard]] il::RegId notedRegister(const il::Function& function, il::OpId phi) {
  const std::string_view note = function.noteOn(phi);
  if (!note.starts_with(kRegisterNotePrefix)) {
    return il::RegId::invalid();
  }
  return function.registers().find(note.substr(kRegisterNotePrefix.size()));
}

/// The leading phi (if any) at `block` noted as merging `reg`. Phis lead a
/// block (every other op comes after them), so this stops at the first
/// non-phi rather than scanning the whole block.
[[nodiscard]] il::OpId leadingPhiFor(const il::Function& function, il::BlockId block,
                                    il::RegId reg) {
  for (const il::OpId opId : function.block(block).ops) {
    const il::Op& op = function.op(opId);
    if (op.code != il::OpCode::Phi) {
      break;
    }
    if (notedRegister(function, opId) == reg) {
      return opId;
    }
  }
  return il::OpId::invalid();
}

}  // namespace

std::optional<LiveRegisterFrame> matchLiveRegisterFrame(const il::Function& function,
                                                         const DispatcherShape& shape) {
  LiveRegisterFrame frame;
  for (const il::OpId hubPhi : function.block(shape.hub).ops) {
    if (function.op(hubPhi).code != il::OpCode::Phi) {
      break;  // phis lead the block
    }
    const il::RegId reg = notedRegister(function, hubPhi);
    if (!reg.valid()) {
      continue;
    }
    const il::OpId mergePhi = leadingPhiFor(function, shape.merge, reg);
    if (!mergePhi.valid()) {
      continue;  // this register is not relayed through the dispatcher's merge
    }
    frame.slots.push_back(LiveRegisterSlot{reg, hubPhi, mergePhi});
  }
  if (frame.slots.empty()) {
    return std::nullopt;
  }
  return frame;
}

HandlerFrameExit classifyHandlerExit(const il::Function& function, il::BlockId handler,
                                     const DispatcherShape& shape,
                                     const LiveRegisterFrame& frame) {
  HandlerFrameExit exit;
  exit.unchanged.assign(frame.slots.size(), false);
  const il::Block& mergeBlock = function.block(shape.merge);
  const auto predecessor =
      std::find(mergeBlock.predecessors.begin(), mergeBlock.predecessors.end(), handler);
  if (predecessor == mergeBlock.predecessors.end()) {
    exit.kind = HandlerExitKind::Return;
    return exit;
  }
  const auto index = static_cast<std::size_t>(predecessor - mergeBlock.predecessors.begin());
  unsigned unchangedCount = 0;
  for (std::size_t slot = 0; slot < frame.slots.size(); ++slot) {
    const auto operands = function.operands(function.op(frame.slots[slot].shadowPhiAtMerge));
    if (index >= operands.size()) {
      continue;
    }
    const il::Expr& incoming = function.expr(operands[index]);
    // Passthrough looks exactly like the identity it is: the merge phi's
    // operand on this edge is the very same `Value` expression the frame's
    // hub phi defines, not merely one that happens to equal it (comparing
    // ExprIds this way costs nothing extra -- the pool is hash-consed, so a
    // structurally identical expression built any other way would already
    // be this same ExprId).
    const bool unchanged =
        incoming.op == il::ExprOp::Value &&
        il::ValueId{static_cast<uint32_t>(incoming.immediate)} ==
            function.op(frame.slots[slot].livePhiAtHub).result;
    exit.unchanged[slot] = unchanged;
    unchangedCount += unchanged ? 1 : 0;
  }
  exit.kind = unchangedCount == frame.slots.size() ? HandlerExitKind::Passthrough
                                                   : HandlerExitKind::Partial;
  return exit;
}

std::vector<bool> unanimousPassthroughSlots(const il::Function& function,
                                            const DispatcherShape& shape,
                                            const LiveRegisterFrame& frame) {
  std::vector<bool> unanimous(frame.slots.size(), true);
  for (const il::BlockId handler : function.block(shape.merge).predecessors) {
    const HandlerFrameExit exit = classifyHandlerExit(function, handler, shape, frame);
    for (std::size_t slot = 0; slot < frame.slots.size(); ++slot) {
      if (!exit.unchanged[slot]) {
        unanimous[slot] = false;
      }
    }
  }
  return unanimous;
}

}  // namespace xdec::analysis
