// See the header for the shape this clusters and why table identity, not
// control flow, is the clustering key.
#include "xdec/analysis/dispatch_region.h"

#include <algorithm>
#include <map>
#include <set>

#include "xdec/analysis/dispatch_values.h"
#include "xdec/analysis/jump_table.h"

namespace xdec::analysis {

namespace {

// Same bar analysis::matchDispatcherShape holds its own single-block vote
// to: a merge candidate has to carry most of the region's pooled targets,
// not just a plurality, before printing it as the state machine's shared
// epilogue would be honest.
constexpr double kRegionMergeSupportThreshold = 0.8;

/// Two table reads are the same physical table only if every one of these
/// agrees -- base, stride, entry width, the offset-table shape and, when
/// present, the clamp constants. `std::optional<uint64_t>` for the clamp
/// fields so "no site here clamps" (both nullopt) is its own distinct key,
/// never accidentally equal to a clamp whose bound happens to be zero.
struct TableKey {
  uint64_t base = 0;
  uint32_t stride = 0;
  uint32_t entryBits = 0;
  bool relative = false;
  uint64_t anchor = 0;
  bool signedOffsets = false;
  std::optional<uint64_t> clampBound;
  std::optional<uint64_t> clampReplacement;

  [[nodiscard]] bool operator==(const TableKey& other) const noexcept {
    return base == other.base && stride == other.stride && entryBits == other.entryBits &&
           relative == other.relative && anchor == other.anchor &&
           signedOffsets == other.signedOffsets && clampBound == other.clampBound &&
           clampReplacement == other.clampReplacement;
  }
};

/// The region-level generalisation of matchDispatcherShape's own vote: pools
/// every target from every site in the region instead of one dispatch
/// block's own target list, and requires a handler's sole predecessor to be
/// *some* site in the region rather than one fixed dispatch block. Below
/// three pooled targets there is nothing worth searching for, exactly like
/// the single-block version.
[[nodiscard]] std::optional<DispatchRegionTail> matchRegionSharedTail(
    const il::Function& function, const std::vector<DispatchSite>& sites) {
  std::set<il::BlockId> dispatchBlocks;
  for (const DispatchSite& site : sites) {
    dispatchBlocks.insert(site.dispatchBlock);
  }

  std::size_t totalTargets = 0;
  std::map<il::BlockId, std::size_t> votes;
  for (const DispatchSite& site : sites) {
    for (const il::BlockId handler : site.targets) {
      ++totalTargets;
      const il::Block& block = function.block(handler);
      if (block.predecessors.size() != 1 || !dispatchBlocks.contains(block.predecessors.front())) {
        continue;  // reached from elsewhere too, or from a block outside this region
      }
      if (block.successors.size() != 1) {
        continue;  // returns outright, or branches on its own -- not this shape
      }
      ++votes[block.successors.front()];
    }
  }
  if (totalTargets < 3 || votes.empty()) {
    return std::nullopt;
  }
  const auto best = std::max_element(
      votes.begin(), votes.end(),
      [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });
  if (static_cast<double>(best->second) <
      kRegionMergeSupportThreshold * static_cast<double>(totalTargets)) {
    return std::nullopt;
  }
  const il::BlockId merge = best->first;
  if (dispatchBlocks.contains(merge)) {
    return std::nullopt;
  }
  const il::Block& mergeBlock = function.block(merge);
  if (mergeBlock.successors.size() != 1) {
    return std::nullopt;
  }
  const il::BlockId hub = mergeBlock.successors.front();
  if (hub == merge || dispatchBlocks.contains(hub)) {
    return std::nullopt;
  }
  return DispatchRegionTail{merge, hub};
}

}  // namespace

std::vector<DispatchJoin> findDispatchJoins(const il::Function& function,
                                            const DispatchRegion& region) {
  std::set<il::BlockId> dispatchBlocks;
  for (const DispatchSite& site : region.sites) {
    dispatchBlocks.insert(site.dispatchBlock);
  }

  // One vote per qualifying target, keyed by the hub it falls into --
  // matchRegionSharedTail's own per-target check, just tallied per
  // candidate hub instead of pooled into one region-wide winner.
  std::map<il::BlockId, std::vector<il::BlockId>> tailsByHub;
  for (const DispatchSite& site : region.sites) {
    for (const il::BlockId handler : site.targets) {
      const il::Block& block = function.block(handler);
      if (block.predecessors.size() != 1 || block.predecessors.front() != site.dispatchBlock) {
        continue;  // shared, or reached from elsewhere -- not a private tail
      }
      if (block.successors.size() != 1) {
        continue;  // returns outright, or branches on its own -- not this shape
      }
      const il::BlockId hub = block.successors.front();
      if (hub == handler || dispatchBlocks.contains(hub)) {
        continue;  // a self-loop, or the hub is itself another site's own dispatch
      }
      tailsByHub[hub].push_back(handler);
    }
  }

  std::vector<DispatchJoin> joins;
  for (auto& [hub, tails] : tailsByHub) {
    if (tails.size() < 2) {
      continue;  // one feeding tail is a private handler already, not a join
    }
    const std::set<il::BlockId> tailSet(tails.begin(), tails.end());
    const auto& predecessors = function.block(hub).predecessors;
    const std::set<il::BlockId> predecessorSet(predecessors.begin(), predecessors.end());
    if (predecessorSet != tailSet) {
      continue;  // a predecessor from outside this region's own tails
    }
    joins.push_back(DispatchJoin{hub, std::move(tails)});
  }
  return joins;
}

std::optional<DispatchClamp> matchDispatchClamp(const il::Function& function, il::ExprId select) {
  const il::Expr& expr = function.expr(select);
  if (expr.op != il::ExprOp::Select) {
    return std::nullopt;
  }
  const uint32_t width = expr.type.bits();
  if (width != 32 && width != 64) {
    return std::nullopt;
  }
  const il::Expr& cond = function.expr(expr.operands[0]);
  bool isSigned = false;
  if (cond.op == il::ExprOp::CmpLtS) {
    isSigned = true;
  } else if (cond.op != il::ExprOp::CmpLtU) {
    return std::nullopt;
  }
  const il::ExprId index = expr.operands[2];
  if (cond.operands[1] != index) {
    return std::nullopt;
  }
  uint64_t bound = 0;
  uint64_t replacement = 0;
  if (!function.asConstant(cond.operands[0], bound) ||
      !function.asConstant(expr.operands[1], replacement)) {
    // A computed bound or replacement only bounds this one branch -- it is
    // not evidence that any other site reads the same table the same way,
    // which is the only thing clustering needs a clamp for.
    return std::nullopt;
  }
  return DispatchClamp{index, bound, replacement, isSigned};
}

std::optional<DispatcherShape> confirmDispatcherShapeFromRegion(const il::Function& function,
                                                                 il::BlockId dispatch,
                                                                 std::span<const il::BlockId> targets,
                                                                 const DispatchRegion& region) {
  if (!region.sharedTail.has_value() || targets.empty()) {
    return std::nullopt;
  }
  const il::BlockId merge = region.sharedTail->merge;
  for (const il::BlockId handler : targets) {
    const il::Block& block = function.block(handler);
    if (handler == merge || block.predecessors.size() != 1 || block.predecessors.front() != dispatch ||
        block.successors.size() != 1 || block.successors.front() != merge) {
      return std::nullopt;
    }
  }
  return DispatcherShape{dispatch, merge, region.sharedTail->hub};
}

std::vector<DispatchRegion> findDispatchRegions(const il::Function& function) {
  std::vector<TableKey> keys;
  std::vector<DispatchRegion> regions;

  for (const il::BlockId blockId : function.blockHandles()) {
    const il::Block& block = function.block(blockId);
    if (block.empty()) {
      continue;
    }
    const il::Op& terminator = function.op(block.terminator());
    if (terminator.code != il::OpCode::IndirectBranch) {
      continue;
    }
    const auto operands = function.operands(terminator);
    const auto targets = function.targets(terminator);
    if (operands.empty() || targets.empty()) {
      continue;  // still unresolved: nothing to cluster yet
    }
    const std::optional<JumpTable> table = matchJumpTable(function, operands[0]);
    if (!table.has_value() || !table->index.valid()) {
      continue;
    }

    const std::optional<DispatchClamp> clamp = matchDispatchClamp(function, table->index);
    TableKey key{table->base,
                table->stride,
                table->entryBits,
                table->relative,
                table->anchor,
                table->signedOffsets,
                clamp ? std::optional<uint64_t>(clamp->bound) : std::nullopt,
                clamp ? std::optional<uint64_t>(clamp->replacement) : std::nullopt};

    DispatchSite site;
    site.dispatchBlock = blockId;
    site.targets.assign(targets.begin(), targets.end());
    site.indexExpr = table->index;
    if (const auto values = matchDispatchValues(function, table->index, targets.size())) {
      site.caseValues = values->values;
    }

    const auto found = std::find(keys.begin(), keys.end(), key);
    if (found == keys.end()) {
      DispatchRegion region;
      region.tableBase = table->base;
      region.tableStride = table->stride;
      region.tableEntryBits = table->entryBits;
      region.tableRelative = table->relative;
      region.tableAnchor = table->anchor;
      region.tableSignedOffsets = table->signedOffsets;
      region.clampBound = key.clampBound;
      region.clampReplacement = key.clampReplacement;
      region.sites.push_back(std::move(site));
      keys.push_back(key);
      regions.push_back(std::move(region));
    } else {
      const std::size_t index = static_cast<std::size_t>(found - keys.begin());
      regions[index].sites.push_back(std::move(site));
    }
  }

  for (DispatchRegion& region : regions) {
    region.sharedTail = matchRegionSharedTail(function, region.sites);
  }
  return regions;
}

}  // namespace xdec::analysis
