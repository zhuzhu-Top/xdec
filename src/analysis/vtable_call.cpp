// matchVtableCallTarget / findConfirmedVtableCalls (see the header).
#include "xdec/analysis/vtable_call.h"

#include <map>
#include <unordered_set>

namespace xdec::analysis {

namespace {

[[nodiscard]] il::ExprId stripCasts(const il::Function& function, il::ExprId id) {
  for (;;) {
    const il::Expr& expr = function.expr(id);
    if (expr.op != il::ExprOp::ZExt && expr.op != il::ExprOp::SExt &&
        expr.op != il::ExprOp::Trunc && expr.op != il::ExprOp::Bitcast) {
      return id;
    }
    id = expr.operand(0);
  }
}

}  // namespace

std::optional<VtableCallSite> matchVtableCallTarget(const il::Function& function, il::OpId call) {
  if (!function.hasOp(call) || function.op(call).code != il::OpCode::Call) {
    return std::nullopt;
  }
  const auto callOperands = function.operands(function.op(call));
  if (callOperands.empty()) {
    return std::nullopt;
  }
  const il::Expr& target = function.expr(stripCasts(function, callOperands[0]));
  if (target.op != il::ExprOp::Value) {
    return std::nullopt;  // a direct call, or through something not a load's result
  }
  const il::ValueId loadedValue{static_cast<uint32_t>(target.immediate)};
  if (!function.hasValue(loadedValue)) {
    return std::nullopt;
  }
  const il::OpId definition = function.value(loadedValue).definition;
  if (!function.hasOp(definition) || function.op(definition).code != il::OpCode::Load) {
    return std::nullopt;
  }
  const il::ExprId address = function.operands(function.op(definition))[0];
  const il::Expr& addrExpr = function.expr(address);
  if (addrExpr.op != il::ExprOp::Add) {
    return VtableCallSite{call, address, 0};  // load(obj): a slot at offset 0
  }
  uint64_t offset = 0;
  if (function.asConstant(addrExpr.operand(1), offset)) {
    return VtableCallSite{call, addrExpr.operand(0), offset};
  }
  if (function.asConstant(addrExpr.operand(0), offset)) {
    return VtableCallSite{call, addrExpr.operand(1), offset};
  }
  return std::nullopt;  // neither side of the address is a constant offset
}

std::vector<VtableCallSite> findConfirmedVtableCalls(const il::Function& function) {
  std::vector<VtableCallSite> all;
  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      if (const std::optional<VtableCallSite> site = matchVtableCallTarget(function, opId)) {
        all.push_back(*site);
      }
    }
  }

  // Keyed by the object's ExprId (ordered, so the result does not depend on
  // hash iteration order) rather than grouped in a single unordered pass:
  // a function can read a real vtable through more than one object, and
  // each is judged only against its own other call sites.
  std::map<uint32_t, std::vector<std::size_t>> byObject;
  for (std::size_t i = 0; i < all.size(); ++i) {
    byObject[all[i].object.index()].push_back(i);
  }

  std::vector<VtableCallSite> confirmed;
  for (const auto& [objectIndex, indices] : byObject) {
    std::unordered_set<uint64_t> offsets;
    for (const std::size_t i : indices) {
      offsets.insert(all[i].slotOffset);
    }
    if (offsets.size() < 2) {
      continue;  // one slot seen on this object: not distinguishable from an
                 // ordinary function-pointer read
    }
    for (const std::size_t i : indices) {
      confirmed.push_back(all[i]);
    }
  }
  return confirmed;
}

}  // namespace xdec::analysis
