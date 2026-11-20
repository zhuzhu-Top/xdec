// StackEscapeMap (see the header for what a region means and why this is
// its own analysis).
#include "xdec/analysis/stack_escape.h"

#include <algorithm>
#include <unordered_set>

namespace xdec::analysis {

namespace {

/// Same address-operand exemption stack_store_fold.cpp's own escape scan
/// uses: neither a Load's nor a Store's own address operand is itself an
/// escape of the delta it denotes.
[[nodiscard]] bool isOwnAddressOperand(il::OpCode code, std::size_t index) {
  return (code == il::OpCode::Load || code == il::OpCode::Store) && index == 0;
}

/// True when `value` is a raw callee-saved entry register (`x19..x30` or
/// `q8..q15` / `d8..d15` read) -- an ABI prologue register spill that cannot
/// be a user-populated aggregate field reached through an adjacent local's
/// pointer.
[[nodiscard]] bool isCalleeSavedEntrySpill(const il::Function& function, il::ExprId value) {
  il::ExprId cur = value;
  for (unsigned depth = 0; depth < 4 && cur.valid(); ++depth) {
    const il::Expr& expr = function.expr(cur);
    if ((expr.op == il::ExprOp::Trunc || expr.op == il::ExprOp::Bitcast ||
         expr.op == il::ExprOp::Extract) &&
        expr.operandCount == 1) {
      cur = expr.operands[0];
      continue;
    }
    break;
  }
  const auto isCalleeSavedVecName = [](std::string_view rname) {
    if ((rname.size() == 2 || rname.size() == 3) &&
        (rname[0] == 'q' || rname[0] == 'd' || rname[0] == 'v')) {
      const std::string_view num = rname.substr(1);
      return num == "8" || num == "9" || num == "10" || num == "11" ||
             num == "12" || num == "13" || num == "14" || num == "15";
    }
    return false;
  };
  const il::Expr& expr = function.expr(cur);
  if (expr.op == il::ExprOp::Value) {
    const il::ValueId val{static_cast<uint32_t>(expr.immediate)};
    if (function.hasValue(val)) {
      const il::OpId defId = function.value(val).definition;
      if (defId.valid() && function.hasOp(defId) &&
          function.op(defId).code == il::OpCode::ReadReg) {
        const il::RegId root = function.registers().rootOf(function.op(defId).reg());
        return isCalleeSavedVecName(function.registers().nameOf(root));
      }
    }
    return false;
  }
  if (expr.op != il::ExprOp::EntryReg) {
    return false;
  }
  const il::RegId reg{static_cast<uint32_t>(expr.immediate)};
  if (!function.registers().contains(reg)) {
    return false;
  }
  const std::string_view name = function.registers().nameOf(reg);
  if (isCalleeSavedVecName(name)) {
    return true;
  }
  return name.size() == 3 && name[0] == 'x' &&
         ((name[1] == '1' && name[2] == '9') ||
          (name[1] == '2' && name[2] >= '0' && name[2] <= '9') ||
          (name[1] == '3' && name[2] == '0'));
}

/// One Store's own footprint: the delta it writes through, and how many
/// bytes wide.
struct StoreSpan {
  int64_t delta;
  uint64_t widthBytes;
};

}  // namespace

StackEscapeMap StackEscapeMap::compute(const il::Function& function, const StackFrame& frame) {
  std::vector<int64_t> pointEscapes;
  std::unordered_set<int64_t> seenPoint;
  std::vector<StoreSpan> storeSpans;

  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      const auto operands = function.operands(op);
      for (std::size_t index = 0; index < operands.size(); ++index) {
        const AddressInfo info = frame.classify(operands[index]);
        if (info.kind != AddressKind::StackSlot) {
          continue;
        }
        if (op.code == il::OpCode::Store && index == 0) {
          if (operands.size() >= 2 && isCalleeSavedEntrySpill(function, operands[1])) {
            continue;
          }
          const uint64_t widthBytes = std::max<uint64_t>(op.type.bits() / 8, 1);
          storeSpans.push_back({info.delta, widthBytes});
          continue;
        }
        if (isOwnAddressOperand(op.code, index)) {
          continue;
        }
        if (seenPoint.insert(info.delta).second) {
          pointEscapes.push_back(info.delta);
        }
      }
    }
  }

  StackEscapeMap map;
  map.regions_.reserve(pointEscapes.size());
  for (const int64_t base : pointEscapes) {
    // Grown from [base, base) by repeatedly folding in any store span that
    // touches or overlaps the region so far -- a store whose delta leaves a
    // gap above the current end is left out, on this pass and every later
    // one, since nothing bridges it. Bounded by storeSpans.size() passes:
    // each pass that changes anything strictly grows `end` to some span's
    // own reach, and no span can do that twice.
    uint64_t end = 0;
    bool grew = false;
    bool changed = true;
    while (changed) {
      changed = false;
      for (const StoreSpan& span : storeSpans) {
        if (span.delta < base) {
          continue;
        }
        const uint64_t offset = static_cast<uint64_t>(span.delta - base);
        if (offset > end) {
          continue;
        }
        const uint64_t reach = offset + span.widthBytes;
        if (reach > end) {
          end = reach;
          grew = true;
          changed = true;
        }
      }
    }
    map.regions_.push_back({base, std::max<uint64_t>(end, 1),
                            grew ? StackEscapeReason::StoreFootprint : StackEscapeReason::Point});
  }
  return map;
}

bool StackEscapeMap::isEscaped(int64_t delta) const {
  for (const StackEscapeRegion& region : regions_) {
    if (delta >= region.baseDelta &&
        delta < region.baseDelta + static_cast<int64_t>(region.sizeBytes)) {
      return true;
    }
  }
  return false;
}

}  // namespace xdec::analysis
