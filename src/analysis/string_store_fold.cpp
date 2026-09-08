// findFoldableStringStores (see the header for the safety rules).
#include "xdec/analysis/string_store_fold.h"

#include <cstddef>
#include <optional>

namespace xdec::analysis {

namespace {

struct RunEntry {
  il::OpId op;
  int64_t delta = 0;
  uint32_t width = 0;
  uint64_t value = 0;
};

/// Whether `bytes` is exactly one run's worth of a printable, NUL-terminated
/// C string: rule 4 of the header comment.
[[nodiscard]] bool isPrintableCString(const std::string& bytes) {
  if (bytes.size() < 2 || bytes.back() != '\0') {
    return false;
  }
  for (std::size_t i = 0; i + 1 < bytes.size(); ++i) {
    const auto byte = static_cast<unsigned char>(bytes[i]);
    if (byte < 0x20 || byte > 0x7e) {
      return false;
    }
  }
  return true;
}

/// Concatenates a finished run's stored constants into their little-endian
/// byte sequence and, if that sequence reads as a C string, records it.
/// Leaves `result` untouched otherwise -- a run that fails to decode simply
/// keeps its ordinary per-Store printing.
void tryEmit(const std::vector<RunEntry>& run,
            std::unordered_map<uint32_t, FoldableStringStore>& result) {
  if (run.size() < 2) {
    return;
  }
  std::string bytes;
  for (const RunEntry& entry : run) {
    uint64_t value = entry.value;
    for (uint32_t i = 0; i < entry.width; ++i) {
      bytes.push_back(static_cast<char>(static_cast<unsigned char>(value & 0xff)));
      value >>= 8;
    }
  }
  if (!isPrintableCString(bytes)) {
    return;
  }
  FoldableStringStore fold;
  fold.delta = run.front().delta;
  fold.text.assign(bytes, 0, bytes.size() - 1);  // drop the trailing NUL
  fold.continuationOps.reserve(run.size() - 1);
  for (std::size_t i = 1; i < run.size(); ++i) {
    fold.continuationOps.push_back(run[i].op.index());
  }
  result.emplace(run.front().op.index(), std::move(fold));
}

}  // namespace

std::unordered_map<uint32_t, FoldableStringStore> findFoldableStringStores(
    const il::Function& function, const StackFrame& frame) {
  std::unordered_map<uint32_t, FoldableStringStore> result;

  for (const il::BlockId blockId : function.blockHandles()) {
    std::vector<RunEntry> run;
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      std::optional<RunEntry> entry;
      if (op.code == il::OpCode::Store) {
        const auto operands = function.operands(op);
        if (operands.size() >= 2) {
          const AddressInfo info = frame.classify(operands[0]);
          const uint32_t width = op.type.bits() / 8;
          uint64_t constant = 0;
          if (info.kind == AddressKind::StackSlot && width >= 1 && width <= 8 &&
              function.asConstantThroughCasts(operands[1], constant)) {
            entry = RunEntry{opId, info.delta, width, constant};
          }
        }
      }
      if (!entry.has_value()) {
        tryEmit(run, result);
        run.clear();
        continue;
      }
      if (!run.empty() &&
          entry->delta != run.back().delta + static_cast<int64_t>(run.back().width)) {
        tryEmit(run, result);
        run.clear();
      }
      run.push_back(*entry);
    }
    tryEmit(run, result);
  }

  return result;
}

}  // namespace xdec::analysis
