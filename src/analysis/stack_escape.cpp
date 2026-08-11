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
