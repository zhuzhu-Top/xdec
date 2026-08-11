// findFoldableStackLoads (see the header for the safety rules).
#include "xdec/analysis/stack_load_fold.h"

#include "xdec/analysis/value_uses.h"

namespace xdec::analysis {

namespace {

/// Whether some op strictly between `loadIndex` and `useIndex` in `ops`
/// could have overwritten `loadAddress`/`loadWidth`.
[[nodiscard]] bool clobberedBetween(const il::Function& function, const StackFrame& frame,
                                    const std::vector<il::OpId>& ops, std::size_t loadIndex,
                                    std::size_t useIndex, il::ExprId loadAddress,
                                    unsigned loadWidth) {
  for (std::size_t index = loadIndex + 1; index < useIndex; ++index) {
    const il::Op& op = function.op(ops[index]);
    switch (op.code) {
      case il::OpCode::Store: {
        const auto operands = function.operands(op);
        if (frame.mayAlias(loadAddress, loadWidth, operands[0], op.type.bits() / 8) !=
            AliasResult::No) {
          return true;
        }
        break;
      }
      case il::OpCode::Call:
      case il::OpCode::Intrinsic:
      case il::OpCode::Unimplemented:
        // Opaque effects: any of these may write through a pointer this
        // analysis has no way to reason about.
        return true;
      default:
        break;
    }
  }
  return false;
}

/// Whether `useOp`'s address operand (index 0, the shape both Load and Store
/// share) is exactly `result` -- the shape a spilled pointer takes, and what
/// lets variables.cpp treat the slot itself as a pointer instead of a plain
/// integer.
[[nodiscard]] bool isAddressOperand(const il::Function& function, const il::Op& useOp,
                                    il::ValueId result) {
  if (useOp.code != il::OpCode::Load && useOp.code != il::OpCode::Store) {
    return false;
  }
  const auto operands = function.operands(useOp);
  if (operands.empty()) {
    return false;
  }
  const il::Expr& addressExpr = function.expr(operands[0]);
  return addressExpr.op == il::ExprOp::Value &&
         static_cast<uint32_t>(addressExpr.immediate) == result.index();
}

}  // namespace

std::unordered_map<uint32_t, FoldableStackLoad> findFoldableStackLoads(
    const il::Function& function, const StackFrame& frame,
    const std::unordered_set<uint32_t>& deadOps) {
  std::unordered_map<uint32_t, FoldableStackLoad> foldable;
  const FunctionValueUses uses = collectValueUses(function);

  for (const il::BlockId blockId : function.blockHandles()) {
    const il::Block& block = function.block(blockId);
    std::unordered_map<uint32_t, std::size_t> indexOf;
    indexOf.reserve(block.ops.size());
    for (std::size_t index = 0; index < block.ops.size(); ++index) {
      indexOf.emplace(block.ops[index].index(), index);
    }

    for (std::size_t loadIndex = 0; loadIndex < block.ops.size(); ++loadIndex) {
      const il::OpId loadOpId = block.ops[loadIndex];
      const il::Op& load = function.op(loadOpId);
      if (load.code != il::OpCode::Load || !load.result.valid()) {
        continue;
      }
      const auto found = uses.sites.find(load.result.index());
      if (found == uses.sites.end()) {
        continue;  // unread: not this analysis's problem (see dce.cpp)
      }

      // Every *live* reader (one whose own op will actually print) must be
      // in this block, after the load, with nothing able to have clobbered
      // the slot in between. A reader elsewhere -- another block, most
      // often the far side of a merge a flattened dispatcher's loop-carried
      // state closes through -- is exactly the cross-block forwarding this
      // analysis does not attempt (see the header's non-goals).
      std::vector<std::size_t> liveUseIndices;
      bool eligible = true;
      for (const ValueUseSite& site : found->second) {
        if (deadOps.contains(site.op.index())) {
          continue;  // never prints; not a real reader
        }
        if (site.block != blockId) {
          eligible = false;
          break;
        }
        const auto useAt = indexOf.find(site.op.index());
        if (useAt == indexOf.end() || useAt->second <= loadIndex) {
          eligible = false;  // SSA disagreement: untrusted, not acted on
          break;
        }
        liveUseIndices.push_back(useAt->second);
      }
      if (!eligible || liveUseIndices.empty()) {
        continue;
      }

      const il::ExprId address = function.operands(load)[0];
      const AddressInfo info = frame.classify(address);
      if (info.kind != AddressKind::StackSlot) {
        continue;
      }
      const unsigned width = load.type.bits() / 8;
      bool fresh = true;
      for (const std::size_t useIndex : liveUseIndices) {
        if (clobberedBetween(function, frame, block.ops, loadIndex, useIndex, address, width)) {
          fresh = false;
          break;
        }
      }
      if (!fresh) {
        continue;
      }

      bool usedAsAddress = true;
      for (const ValueUseSite& site : found->second) {
        if (deadOps.contains(site.op.index())) {
          continue;
        }
        if (!isAddressOperand(function, function.op(site.op), load.result)) {
          usedAsAddress = false;
          break;
        }
      }

      foldable.emplace(loadOpId.index(),
                       FoldableStackLoad{info.delta, load.type.bits(), usedAsAddress});
    }
  }
  return foldable;
}

}  // namespace xdec::analysis
