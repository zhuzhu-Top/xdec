// analyzeEmitRedundancy / prepareEmitRedundancy (see the header for what
// each computes and why they share one traversal).
#include "xdec/analysis/emit_redundancy.h"

#include <algorithm>
#include <format>

#include "xdec/analysis/dispatcher_shape.h"
#include "xdec/analysis/live_register_frame.h"

namespace xdec::analysis {

namespace {

/// Shape J's own count, best-effort: a flattened function has at most one
/// dispatch loop, so the first resolved `IndirectBranch` whose targets match
/// `DispatcherShape` is the whole function's relay, and every other block is
/// irrelevant to this count. `nullopt` covers both "not flattened" and "no
/// dispatcher shape resolves" -- this analysis does not distinguish the two,
/// same as `matchDispatcherShape` itself does not.
struct DispatcherRelayStats {
  std::size_t slots = 0;
  std::size_t unanimousPassthrough = 0;
};

[[nodiscard]] std::optional<DispatcherRelayStats> analyzeDispatcherRelay(
    const il::Function& function) {
  for (const il::BlockId blockId : function.blockHandles()) {
    const auto& ops = function.block(blockId).ops;
    if (ops.empty()) {
      continue;
    }
    const il::Op& term = function.op(ops.back());
    if (term.code != il::OpCode::IndirectBranch) {
      continue;
    }
    const auto targets = function.targets(term);
    const std::optional<DispatcherShape> shape = matchDispatcherShape(function, blockId, targets);
    if (!shape.has_value()) {
      continue;
    }
    const std::optional<LiveRegisterFrame> frame = matchLiveRegisterFrame(function, *shape);
    if (!frame.has_value()) {
      return DispatcherRelayStats{};  // a dispatcher, but nothing kept alive across it
    }
    const std::vector<bool> unanimous = unanimousPassthroughSlots(function, *shape, *frame);
    DispatcherRelayStats stats;
    stats.slots = frame->slots.size();
    stats.unanimousPassthrough =
        static_cast<std::size_t>(std::count(unanimous.begin(), unanimous.end(), true));
    return stats;
  }
  return std::nullopt;
}

}  // namespace

std::string EmitRedundancyReport::format() const {
  std::string text = std::format(
      "stack loads: {}/{} folded, memory loads: {}/{} folded, stack stores: {}/{} dead, "
      "{} write-only local(s)",
      stackLoadsFolded, stackLoads, memoryLoadsFolded, memoryLoads, stackStoresDead, stackStores,
      writeOnlyLocals);
  if (dispatcherRelaySlots.has_value()) {
    text += std::format(", dispatcher relay: {}/{} slot(s) unanimous passthrough",
                        dispatcherRelayUnneededSlots.value_or(0), *dispatcherRelaySlots);
  }
  return text;
}

EmitRedundancyPrep prepareEmitRedundancy(const il::Function& function, const StackFrame& frame,
                                         const VariableTable& variables,
                                         std::unordered_set<uint32_t> seedDeadOps,
                                         const StackLoadFilter& stackLoadFilter) {
  EmitRedundancyPrep prep;
  prep.deadOps = std::move(seedDeadOps);

  // The ldaxr/stlxr fold (see atomic_exclusive.h): joined first, before
  // anything else below decides what needs a declaration or counts as a
  // live reference, exactly like CContext's own constructor always did.
  prep.exclusiveLoads = findExclusiveLoads(function);
  for (const auto& [loadIndex, exclusive] : prep.exclusiveLoads) {
    prep.deadOps.insert(exclusive.reserveOp.index());
  }
  prep.exclusiveStores = findExclusiveStores(function);
  for (const auto& [storeIndex, exclusive] : prep.exclusiveStores) {
    prep.deadOps.insert(storeIndex);
  }

  // Stack-load-fold (shape F): needs the exclusive-op folds above already in
  // `deadOps` so a load whose only "reader" is one of those never blocks its
  // own fold on a reader nothing will ever print.
  prep.foldableStackLoads = findFoldableStackLoads(function, frame, prep.deadOps);
  for (const auto& [opIndex, load] : prep.foldableStackLoads) {
    if (stackLoadFilter && !stackLoadFilter(opIndex, load)) {
      continue;  // no local recovered at this slot; leave the ordinary temp path
    }
    prep.appliedStackLoads.insert(opIndex);
    prep.deadOps.insert(opIndex);
  }

  // Load-inline (shape G): the same fold, minus the StackSlot restriction --
  // run after the stack-load fold above for the same reason that fold ran
  // after the exclusive ops.
  prep.foldableMemoryLoads = findFoldableMemoryLoads(function, frame, prep.deadOps);
  for (const auto& [opIndex, load] : prep.foldableMemoryLoads) {
    prep.deadOps.insert(opIndex);
  }

  // Stack-store-fold (shape H1): function-wide rather than one-store-at-a-
  // time, and does not itself read `deadOps` (see stack_store_fold.h), so
  // its position here is about grouping, not a dependency on the folds
  // above.
  prep.deadStackStores = findDeadStackStores(function, frame, variables);
  for (const uint32_t opIndex : prep.deadStackStores) {
    prep.deadOps.insert(opIndex);
    const auto operands = function.operands(function.op(il::OpId{opIndex}));
    prep.deadLocalStackDeltas.insert(frame.classify(operands[0]).delta);
  }

  return prep;
}

EmitRedundancyReport analyzeEmitRedundancy(const il::Function& function, const StackFrame& frame,
                                           const VariableTable& variables) {
  EmitRedundancyReport report;

  std::unordered_set<int64_t> readDeltas;
  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      const auto operands = function.operands(op);
      if (op.code == il::OpCode::Load) {
        const AddressInfo info = frame.classify(operands[0]);
        if (info.kind == AddressKind::StackSlot) {
          ++report.stackLoads;
          readDeltas.insert(info.delta);
        } else {
          // Global or Other -- findFoldableMemoryLoads' own domain (shape
          // G), the counterpart of stackLoads/stackLoadsFolded above.
          ++report.memoryLoads;
        }
      } else if (op.code == il::OpCode::Store) {
        report.stackStores += frame.classify(operands[0]).kind == AddressKind::StackSlot;
      }
    }
  }

  // Sharing one aggregator with CContext (rather than each calling
  // findFoldableStackLoads/findFoldableMemoryLoads independently, the latter
  // with whatever `deadOps` it happened to have on hand) is what keeps this
  // report's counts from drifting out of step with what CContext actually
  // folds -- see the architecture plan's own note on this constructor.
  const EmitRedundancyPrep prep = prepareEmitRedundancy(function, frame, variables);
  report.stackLoadsFolded = prep.appliedStackLoads.size();
  report.memoryLoadsFolded = prep.foldableMemoryLoads.size();
  report.stackStoresDead = prep.deadStackStores.size();

  for (const analysis::Variable& local : variables.locals()) {
    if (!readDeltas.contains(local.stackDelta)) {
      ++report.writeOnlyLocals;
    }
  }

  if (const std::optional<DispatcherRelayStats> relay = analyzeDispatcherRelay(function);
      relay.has_value()) {
    report.dispatcherRelaySlots = relay->slots;
    report.dispatcherRelayUnneededSlots = relay->unanimousPassthrough;
  }

  return report;
}

}  // namespace xdec::analysis
