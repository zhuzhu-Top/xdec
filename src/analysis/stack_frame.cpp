// StackFrame: address classification and the alias oracle (see the header).
#include "xdec/analysis/stack_frame.h"

#include <algorithm>
#include <limits>

namespace xdec::analysis {

StackFrame StackFrame::compute(const il::Function& function) {
  il::RegId spRoot;
  const il::RegisterFile& registers = function.registers();
  for (il::RegId id{0}; id.asSize() < registers.size(); id = il::RegId{id.index() + 1}) {
    const il::RegisterInfo& info = registers[id];
    if (info.regClass == il::RegClass::StackPointer && !info.isSubRegister()) {
      spRoot = id;
      break;
    }
  }
  StackFrame frame{function, spRoot};
  if (!spRoot.valid()) {
    return frame;
  }

  // Walk the memory ops once to learn the observed frame extent.
  int64_t low = std::numeric_limits<int64_t>::max();
  int64_t high = std::numeric_limits<int64_t>::min();
  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      if (op.code != il::OpCode::Load && op.code != il::OpCode::Store) {
        continue;
      }
      const auto operands = function.operands(op);
      const std::optional<int64_t> delta = frame.frameDelta(operands[0]);
      if (!delta.has_value()) {
        continue;
      }
      low = std::min(low, *delta);
      high = std::max(high, *delta + static_cast<int64_t>(op.type.bits() / 8));
    }
  }
  if (low <= high) {
    frame.frameLow_ = low;
    frame.frameHigh_ = high;
  }
  return frame;
}

std::optional<int64_t> StackFrame::frameDelta(il::ExprId address, unsigned depth) const {
  if (!spRoot_.valid() || depth > kMaxDepth) {
    return std::nullopt;
  }
  const il::Expr expr = function_->expr(address);  // by value: interning dangles
  switch (expr.op) {
    case il::ExprOp::EntryReg:
      if (il::RegId{static_cast<uint32_t>(expr.immediate)} == spRoot_) {
        return int64_t{0};
      }
      return std::nullopt;
    case il::ExprOp::Add: {
      // Exactly one side may be stack-derived; the other must be a constant.
      const std::optional<int64_t> fromLhs = frameDelta(expr.operands[0], depth + 1);
      uint64_t constant = 0;
      if (fromLhs.has_value() && function_->asConstant(expr.operands[1], constant)) {
        return *fromLhs + static_cast<int64_t>(constant);
      }
      const std::optional<int64_t> fromRhs = frameDelta(expr.operands[1], depth + 1);
      if (fromRhs.has_value() && function_->asConstant(expr.operands[0], constant)) {
        return *fromRhs + static_cast<int64_t>(constant);
      }
      return std::nullopt;
    }
    case il::ExprOp::Sub: {
      const std::optional<int64_t> fromLhs = frameDelta(expr.operands[0], depth + 1);
      uint64_t constant = 0;
      if (fromLhs.has_value() && function_->asConstant(expr.operands[1], constant)) {
        return *fromLhs - static_cast<int64_t>(constant);
      }
      return std::nullopt;
    }
    default:
      return std::nullopt;
  }
}

AddressInfo StackFrame::classify(il::ExprId address) const {
  AddressInfo info;
  if (const std::optional<int64_t> delta = frameDelta(address)) {
    info.kind = AddressKind::StackSlot;
    info.delta = *delta;
    return info;
  }
  uint64_t constant = 0;
  if (function_->asConstant(address, constant)) {
    info.kind = AddressKind::Global;
    info.address = constant;
  }
  return info;
}

AliasResult StackFrame::mayAlias(il::ExprId addressA, unsigned sizeA, il::ExprId addressB,
                                 unsigned sizeB) const {
  const AddressInfo a = classify(addressA);
  const AddressInfo b = classify(addressB);
  if (a.kind == AddressKind::Other || b.kind == AddressKind::Other) {
    return AliasResult::May;
  }
  if (a.kind != b.kind) {
    // Frame versus image: disjoint by target convention.
    return AliasResult::No;
  }
  const uint64_t baseA =
      a.kind == AddressKind::StackSlot ? static_cast<uint64_t>(a.delta) : a.address;
  const uint64_t baseB =
      b.kind == AddressKind::StackSlot ? static_cast<uint64_t>(b.delta) : b.address;
  const uint64_t endA = baseA + sizeA;
  const uint64_t endB = baseB + sizeB;
  if (endA <= baseB || endB <= baseA) {
    return AliasResult::No;
  }
  if (baseA == baseB && sizeA == sizeB) {
    return AliasResult::Must;
  }
  return AliasResult::May;
}

}  // namespace xdec::analysis
