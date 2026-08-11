// findExclusiveLoads/findExclusiveStores (see the header for the shape).
#include "xdec/analysis/atomic_exclusive.h"

namespace xdec::analysis {

namespace {

[[nodiscard]] bool isIntrinsicNamed(const il::Function& function, const il::Op& op,
                                    std::string_view name) {
  return op.code == il::OpCode::Intrinsic && function.nameOf(op.payload) == name;
}

}  // namespace

std::unordered_map<uint32_t, ExclusiveLoad> findExclusiveLoads(const il::Function& function) {
  std::unordered_map<uint32_t, ExclusiveLoad> result;
  for (const il::BlockId blockId : function.blockHandles()) {
    const std::vector<il::OpId>& ops = function.block(blockId).ops;
    for (std::size_t index = 0; index + 1 < ops.size(); ++index) {
      const il::Op& reserve = function.op(ops[index]);
      if (!isIntrinsicNamed(function, reserve, "aarch64.reserve")) {
        continue;
      }
      const il::Op& load = function.op(ops[index + 1]);
      if (load.code != il::OpCode::Load) {
        continue;
      }
      const auto reserveOperands = function.operands(reserve);
      const auto loadOperands = function.operands(load);
      if (reserveOperands.size() != 1 || loadOperands.empty() ||
          reserveOperands[0] != loadOperands[0]) {
        continue;
      }
      result.emplace(ops[index + 1].index(), ExclusiveLoad{ops[index]});
    }
  }
  return result;
}

std::unordered_map<uint32_t, ExclusiveStore> findExclusiveStores(const il::Function& function) {
  std::unordered_map<uint32_t, ExclusiveStore> result;
  for (const il::BlockId blockId : function.blockHandles()) {
    const std::vector<il::OpId>& ops = function.block(blockId).ops;
    for (std::size_t index = 0; index + 1 < ops.size(); ++index) {
      const il::Op& store = function.op(ops[index]);
      if (store.code != il::OpCode::Store) {
        continue;
      }
      const il::Op& status = function.op(ops[index + 1]);
      if (!isIntrinsicNamed(function, status, "aarch64.store_exclusive_status")) {
        continue;
      }
      result.emplace(ops[index].index(), ExclusiveStore{ops[index + 1]});
    }
  }
  return result;
}

}  // namespace xdec::analysis
