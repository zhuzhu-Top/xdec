// findFoldableMemoryLoads (see the header for the safety rules).
#include "xdec/analysis/load_inline.h"

#include "xdec/analysis/value_uses.h"

namespace xdec::analysis {

namespace {

/// Whether some op strictly between `loadIndex` and `useIndex` in `ops`
/// could have overwritten `loadAddress`/`loadWidth`. Mirrors
/// stack_load_fold.cpp's own `clobberedBetween`: the same reasoning applies
/// unchanged regardless of what kind of address this is.
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
        return true;
      default:
        break;
    }
  }
  return false;
}

/// Whether `useOp`'s address operand (index 0, the shape both Load and Store
/// share) is exactly `result`. Mirrors stack_load_fold.cpp's own
/// `isAddressOperand`, but used here as an exclusion rather than a pointer
/// hint: unlike a stack slot, a folded address's substitution text has no
/// name of its own, and a load consumed this way is exactly the shape
/// c_context.cpp's `fieldAccess` chains through a *named* base to recognise
/// `n->next->value` -- folding it away would replace that field name with
/// raw pointer arithmetic, trading a strictly more readable spelling for a
/// less readable one. Leaving such a load its ordinary temporary is what
/// gives `fieldAccess` a name to chain through.
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

std::unordered_map<uint32_t, FoldableMemoryLoad> findFoldableMemoryLoads(
    const il::Function& function, const StackFrame& frame,
    const std::unordered_set<uint32_t>& deadOps) {
  std::unordered_map<uint32_t, FoldableMemoryLoad> foldable;
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
      const il::ExprId address = function.operands(load)[0];
      if (frame.classify(address).kind == AddressKind::StackSlot) {
        continue;  // stack_load_fold.cpp's own territory
      }

      const auto found = uses.sites.find(load.result.index());
      if (found == uses.sites.end()) {
        continue;  // unread: not this analysis's problem (see dce.cpp)
      }

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

      bool usedAsAddress = false;
      for (const ValueUseSite& site : found->second) {
        if (!deadOps.contains(site.op.index()) &&
            isAddressOperand(function, function.op(site.op), load.result)) {
          usedAsAddress = true;
          break;
        }
      }
      if (usedAsAddress) {
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

      foldable.emplace(loadOpId.index(), FoldableMemoryLoad{address, load.type.bits()});
    }
  }
  return foldable;
}

}  // namespace xdec::analysis
