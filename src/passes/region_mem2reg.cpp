#include "xdec/passes/region_mem2reg.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "xdec/analysis/dominators.h"
#include "xdec/devirt/memory.h"
#include "xdec/il/function.h"

#include "transform.h"

namespace xdec::passes {

namespace {

struct SlotKey {
  std::size_t regionIndex = 0;
  int64_t offset = 0;

  friend bool operator<(const SlotKey& a, const SlotKey& b) noexcept {
    if (a.regionIndex != b.regionIndex) {
      return a.regionIndex < b.regionIndex;
    }
    return a.offset < b.offset;
  }
  friend bool operator==(const SlotKey&, const SlotKey&) = default;
};

struct MatchedSlot {
  SlotKey key;
  uint32_t widthBits = 0;
};

[[nodiscard]] bool isPrivateRegion(devirt::RegionKind kind) noexcept {
  return kind == devirt::RegionKind::PrivateRegFile ||
         kind == devirt::RegionKind::PrivateStack;
}

[[nodiscard]] bool exprReferencesEntryReg(const il::Function& function, il::ExprId root,
                                          il::RegId targetReg) {
  if (!root.valid() || !targetReg.valid()) {
    return false;
  }
  std::vector<il::ExprId> work{root};
  std::unordered_set<uint32_t> seen;
  while (!work.empty()) {
    const il::ExprId id = work.back();
    work.pop_back();
    if (!id.valid() || !seen.insert(id.index()).second) {
      continue;
    }
    const il::Expr& e = function.expr(id);
    if (e.op == il::ExprOp::EntryReg &&
        il::RegId{static_cast<uint32_t>(e.immediate)} == targetReg) {
      return true;
    }
    for (unsigned i = 0; i < e.operandCount; ++i) {
      work.push_back(e.operands[i]);
    }
  }
  return false;
}

[[nodiscard]] std::optional<std::pair<il::RegId, int64_t>> matchRegPlusConst(
    const il::Function& function, il::ExprId addr, unsigned depth = 0) {
  if (!addr.valid() || depth > 16) {
    return std::nullopt;
  }
  const il::Expr& e = function.expr(addr);
  if (e.op == il::ExprOp::EntryReg) {
    return std::pair{il::RegId{static_cast<uint32_t>(e.immediate)}, int64_t{0}};
  }
  if ((e.op == il::ExprOp::ZExt || e.op == il::ExprOp::Bitcast) &&
      e.operandCount == 1) {
    return matchRegPlusConst(function, e.operands[0], depth + 1);
  }
  if ((e.op == il::ExprOp::Add || e.op == il::ExprOp::Sub) && e.operandCount == 2) {
    uint64_t c = 0;
    if (function.asConstant(e.operands[1], c)) {
      if (const auto base = matchRegPlusConst(function, e.operands[0], depth + 1)) {
        const int64_t delta = static_cast<int64_t>(c);
        return std::pair{base->first,
                         e.op == il::ExprOp::Add ? (base->second + delta)
                                                 : (base->second - delta)};
      }
    }
    if (e.op == il::ExprOp::Add && function.asConstant(e.operands[0], c)) {
      if (const auto base = matchRegPlusConst(function, e.operands[1], depth + 1)) {
        return std::pair{base->first, base->second + static_cast<int64_t>(c)};
      }
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<SlotKey> matchPrivateSlot(
    const il::Function& function, const devirt::MemoryRegionMap& regions,
    il::ExprId addr, uint32_t accessBits) {
  const auto regionList = regions.regions();
  uint64_t constAddr = 0;
  if (function.asConstantThroughCasts(addr, constAddr)) {
    for (std::size_t idx = 0; idx < regionList.size(); ++idx) {
      const auto& r = regionList[idx];
      if (!isPrivateRegion(r.kind) || !r.fixedBase.has_value()) {
        continue;
      }
      if (constAddr >= *r.fixedBase &&
          constAddr - *r.fixedBase + (accessBits / 8) <= r.size) {
        return SlotKey{idx, static_cast<int64_t>(constAddr - *r.fixedBase)};
      }
    }
    return std::nullopt;
  }

  if (const auto baseOff = matchRegPlusConst(function, addr)) {
    for (std::size_t idx = 0; idx < regionList.size(); ++idx) {
      const auto& r = regionList[idx];
      if (!isPrivateRegion(r.kind) || !r.baseReg.has_value()) {
        continue;
      }
      if (*r.baseReg == baseOff->first) {
        return SlotKey{idx, baseOff->second};
      }
    }
  }
  return std::nullopt;
}

struct SlotInfo {
  il::Type type;
  bool conflict = false;
  std::set<il::BlockId> storeBlocks;
  std::vector<std::pair<il::BlockId, il::OpId>> loads;
  std::vector<std::pair<il::BlockId, il::OpId>> stores;
};

struct RegionAnalysis {
  std::vector<bool> escapedRegion;
  std::map<SlotKey, SlotInfo> slots;
};

[[nodiscard]] RegionAnalysis analyzePrivateRegions(
    const il::Function& function, const devirt::MemoryRegionMap& regions,
    const analysis::Dominators& dominators) {
  const auto regionList = regions.regions();
  RegionAnalysis out;
  out.escapedRegion.assign(regionList.size(), false);

  for (const il::BlockId bid : function.blockHandles()) {
    if (!dominators.reachable(bid)) {
      continue;
    }
    for (const il::OpId oid : function.block(bid).ops) {
      const il::Op& op = function.op(oid);
      const auto operands = function.operands(op);
      if (op.code == il::OpCode::Load && !operands.empty()) {
        if (const auto slot = matchPrivateSlot(function, regions, operands[0], op.type.bits())) {
          SlotInfo& info = out.slots[*slot];
          if (info.type.isVoid()) {
            info.type = op.type;
          } else if (info.type != op.type) {
            info.conflict = true;
          }
          info.loads.emplace_back(bid, oid);
          continue;
        }
        for (std::size_t idx = 0; idx < regionList.size(); ++idx) {
          if (regionList[idx].baseReg.has_value() &&
              exprReferencesEntryReg(function, operands[0], *regionList[idx].baseReg)) {
            out.escapedRegion[idx] = true;
          }
        }
        continue;
      }
      if (op.code == il::OpCode::Store && operands.size() >= 2) {
        if (const auto slot = matchPrivateSlot(function, regions, operands[0], op.type.bits())) {
          SlotInfo& info = out.slots[*slot];
          if (info.type.isVoid()) {
            info.type = op.type;
          } else if (info.type != op.type) {
            info.conflict = true;
          }
          info.storeBlocks.insert(bid);
          info.stores.emplace_back(bid, oid);
          // Storing a region base pointer into memory leaks/aliases it.
          for (std::size_t idx = 0; idx < regionList.size(); ++idx) {
            if (regionList[idx].baseReg.has_value() &&
                exprReferencesEntryReg(function, operands[1], *regionList[idx].baseReg)) {
              out.escapedRegion[idx] = true;
            }
          }
          continue;
        }
        for (std::size_t idx = 0; idx < regionList.size(); ++idx) {
          if (regionList[idx].baseReg.has_value() &&
              (exprReferencesEntryReg(function, operands[0], *regionList[idx].baseReg) ||
               exprReferencesEntryReg(function, operands[1], *regionList[idx].baseReg))) {
            out.escapedRegion[idx] = true;
          }
        }
        continue;
      }
      for (const il::ExprId arg : operands) {
        for (std::size_t idx = 0; idx < regionList.size(); ++idx) {
          if (regionList[idx].baseReg.has_value() &&
              exprReferencesEntryReg(function, arg, *regionList[idx].baseReg)) {
            out.escapedRegion[idx] = true;
          }
        }
      }
    }
  }

  // Mark overlapping slots inside the same region as conflicting.
  std::map<std::size_t, std::vector<std::pair<int64_t, uint32_t>>> spansByRegion;
  for (const auto& [key, info] : out.slots) {
    spansByRegion[key.regionIndex].emplace_back(key.offset, info.type.bits() / 8);
  }
  for (const auto& [rIdx, spans] : spansByRegion) {
    for (std::size_t i = 0; i < spans.size(); ++i) {
      for (std::size_t j = i + 1; j < spans.size(); ++j) {
        const int64_t a0 = spans[i].first;
        const int64_t a1 = a0 + static_cast<int64_t>(spans[i].second);
        const int64_t b0 = spans[j].first;
        const int64_t b1 = b0 + static_cast<int64_t>(spans[j].second);
        if (a0 < b1 && b0 < a1) {
          out.slots[SlotKey{rIdx, a0}].conflict = true;
          out.slots[SlotKey{rIdx, b0}].conflict = true;
        }
      }
    }
  }
  return out;
}

class RegionMem2Reg final : public pass::FunctionPass {
 public:
  RegionMem2Reg()
      : FunctionPass([] {
          pass::PassInfo info;
          info.name = "region-mem2reg";
          info.level = il::Maturity::Ssa;
          info.produces = il::Maturity::Ssa;
          info.requirements = {"ssa-construct"};
          return info;
        }()) {}

  Result<bool> run(pass::Context& context) override {
    const devirt::MemoryRegionMap* regions = context.regions();
    if (regions == nullptr || regions->empty()) {
      return false;
    }

    il::Function& function = context.function();
    const analysis::Dominators dominators = analysis::Dominators::compute(function);
    RegionAnalysis analysis = analyzePrivateRegions(function, *regions, dominators);

    std::vector<SlotKey> promotable;
    for (const auto& [key, info] : analysis.slots) {
      if (analysis.escapedRegion[key.regionIndex] || info.conflict ||
          info.stores.empty() || info.loads.empty()) {
        continue;
      }
      promotable.push_back(key);
    }
    if (promotable.empty()) {
      return false;
    }

    // Place Phis via Cytron dominance frontiers. Seed entry with a sentinel
    // `Undef`; if any Load actually observes that sentinel, skip promoting that
    // slot so live-in region loads remain sound!
    std::map<SlotKey, std::vector<il::ExprId>> versions;
    std::map<il::BlockId, std::vector<std::pair<SlotKey, il::OpId>>> phiSites;
    std::map<std::pair<il::BlockId, SlotKey>, il::ValueId> phiValues;
    std::unordered_map<uint32_t, SlotKey> opToSlot;
    std::unordered_set<uint32_t> isStoreOp;

    for (const SlotKey& slot : promotable) {
      const SlotInfo& info = analysis.slots.at(slot);
      for (const auto& [bid, oid] : info.loads) {
        opToSlot.emplace(oid.index(), slot);
      }
      for (const auto& [bid, oid] : info.stores) {
        opToSlot.emplace(oid.index(), slot);
        isStoreOp.insert(oid.index());
      }
      versions[slot].push_back(function.undefined(info.type));

      std::set<il::BlockId> defBlocks = info.storeBlocks;
      defBlocks.insert(function.entryBlock());
      std::vector<il::BlockId> worklist(defBlocks.begin(), defBlocks.end());
      std::set<il::BlockId> placed;
      std::set<il::BlockId> everOnWorklist = defBlocks;
      while (!worklist.empty()) {
        const il::BlockId defsite = worklist.back();
        worklist.pop_back();
        for (const il::BlockId join : dominators.frontier(defsite)) {
          if (!placed.insert(join).second) {
            continue;
          }
          const il::ValueId merged =
              function.prependPhi(join, function.block(join).va, info.type);
          const il::OpId phiOp = function.value(merged).definition;
          phiSites[join].emplace_back(slot, phiOp);
          phiValues[{join, slot}] = merged;
          if (everOnWorklist.insert(join).second) {
            worklist.push_back(join);
          }
        }
      }
    }

    // Rename over dominator tree, tracking which slots read an uninitialized
    // entry sentinel (making them ineligible for pure store-backed promotion).
    std::map<il::OpId, std::map<uint32_t, il::ExprId>> phiInputs;
    std::map<il::ValueId, il::ExprId> loadReplacements;
    std::set<SlotKey> liveInReads;

    auto rename = [&](auto& self, il::BlockId bid) -> void {
      std::map<SlotKey, std::size_t> pushed;
      const auto pushVer = [&](const SlotKey& slot, il::ExprId val) {
        versions[slot].push_back(val);
        ++pushed[slot];
      };
      for (const auto& [slot, phiOp] : phiSites[bid]) {
        (void)phiOp;
        pushVer(slot, function.valueRef(phiValues[{bid, slot}]));
      }
      for (const il::OpId oid : function.block(bid).ops) {
        const auto found = opToSlot.find(oid.index());
        if (found == opToSlot.end()) {
          continue;
        }
        const SlotKey& slot = found->second;
        const il::Op& op = function.op(oid);
        if (isStoreOp.contains(oid.index())) {
          pushVer(slot, function.operands(op)[1]);
        } else {
          const il::ExprId cur = versions[slot].back();
          if (function.expr(cur).op == il::ExprOp::Undef) {
            liveInReads.insert(slot);
          }
          loadReplacements.emplace(op.result, cur);
        }
      }
      for (const il::BlockId succ : function.block(bid).successors) {
        const auto sitesIt = phiSites.find(succ);
        if (sitesIt == phiSites.end()) {
          continue;
        }
        const auto& preds = function.block(succ).predecessors;
        for (uint32_t idx = 0; idx < preds.size(); ++idx) {
          if (preds[idx] != bid) {
            continue;
          }
          for (const auto& [slot, phiOp] : sitesIt->second) {
            phiInputs[phiOp].emplace(idx, versions[slot].back());
          }
        }
      }
      for (const il::BlockId child : dominators.children(bid)) {
        self(self, child);
      }
      for (const auto& [slot, count] : pushed) {
        auto& stack = versions[slot];
        stack.resize(stack.size() - count);
      }
    };
    rename(rename, function.entryBlock());

    // Finalize Phi operands, excluding any slot with live-in reads (remove its
    // newly created Phis and keep its memory ops intact).
    std::set<SlotKey> promotedSet(promotable.begin(), promotable.end());
    for (const SlotKey& bad : liveInReads) {
      promotedSet.erase(bad);
    }
    if (promotedSet.empty()) {
      for (const auto& [bid, sites] : phiSites) {
        for (const auto& [slot, phiOp] : sites) {
          (void)slot;
          function.removeOp(bid, phiOp);
        }
      }
      return false;
    }

    ValueSubst subst;
    for (const auto& [val, repl] : loadReplacements) {
      const il::OpId defId = function.value(val).definition;
      const auto slotIt = opToSlot.find(defId.index());
      if (slotIt != opToSlot.end() && promotedSet.contains(slotIt->second)) {
        subst.set(val, repl);
      }
    }

    for (const auto& [bid, sites] : phiSites) {
      for (const auto& [slot, phiOp] : sites) {
        if (!promotedSet.contains(slot)) {
          function.removeOp(bid, phiOp);
          continue;
        }
        const auto& preds = function.block(bid).predecessors;
        std::vector<il::ExprId> incoming(preds.size(),
                                         function.undefined(analysis.slots.at(slot).type));
        if (const auto it = phiInputs.find(phiOp); it != phiInputs.end()) {
          for (const auto& [idx, expr] : it->second) {
            incoming[idx] = expr;
          }
        }
        function.setOperands(phiOp, incoming);
      }
    }

    for (const il::BlockId bid : function.blockHandles()) {
      if (!dominators.reachable(bid)) {
        continue;
      }
      for (const il::OpId oid : function.block(bid).ops) {
        const auto operands = function.operands(function.op(oid));
        if (operands.empty()) {
          continue;
        }
        std::vector<il::ExprId> rewritten(operands.begin(), operands.end());
        bool changed = false;
        for (il::ExprId& arg : rewritten) {
          const il::ExprId next = subst.apply(function, arg);
          changed |= next != arg;
          arg = next;
        }
        if (changed) {
          function.setOperands(oid, rewritten);
        }
      }
    }

    for (const SlotKey& slot : promotedSet) {
      const SlotInfo& info = analysis.slots.at(slot);
      for (const auto& [bid, oid] : info.loads) {
        function.removeOp(bid, oid);
      }
      for (const auto& [bid, oid] : info.stores) {
        function.removeOp(bid, oid);
      }
    }
    return true;
  }
};

class RegionDse final : public pass::FunctionPass {
 public:
  RegionDse()
      : FunctionPass([] {
          pass::PassInfo info;
          info.name = "region-dse";
          info.level = il::Maturity::Ssa;
          info.produces = il::Maturity::Ssa;
          info.requirements = {"ssa-optimize"};
          return info;
        }()) {}

  Result<bool> run(pass::Context& context) override {
    const devirt::MemoryRegionMap* regions = context.regions();
    if (regions == nullptr || regions->empty()) {
      return false;
    }
    il::Function& function = context.function();
    const analysis::Dominators dominators = analysis::Dominators::compute(function);
    const RegionAnalysis analysis = analyzePrivateRegions(function, *regions, dominators);
    const auto regionList = regions->regions();

    std::vector<std::pair<il::BlockId, il::OpId>> deadStores;
    for (const auto& [key, info] : analysis.slots) {
      if (analysis.escapedRegion[key.regionIndex] || info.conflict || !info.loads.empty()) {
        continue;
      }
      // PrivateStack slots die at function exit; un-loaded stores are dead.
      if (regionList[key.regionIndex].kind == devirt::RegionKind::PrivateStack) {
        for (const auto& [bid, oid] : info.stores) {
          deadStores.emplace_back(bid, oid);
        }
      }
    }

    // Intra-block overwritten PrivateRegFile / PrivateStack stores before any
    // intervening Load on the same slot are also dead.
    for (const il::BlockId bid : function.blockHandles()) {
      std::map<SlotKey, il::OpId> pending;
      for (const il::OpId oid : function.block(bid).ops) {
        const il::Op& op = function.op(oid);
        const auto operands = function.operands(op);
        if (op.code == il::OpCode::Load && !operands.empty()) {
          if (const auto slot = matchPrivateSlot(function, *regions, operands[0], op.type.bits())) {
            pending.erase(*slot);
          } else {
            pending.clear();
          }
          continue;
        }
        if (op.code == il::OpCode::Store && operands.size() >= 2) {
          if (const auto slot = matchPrivateSlot(function, *regions, operands[0], op.type.bits())) {
            if (!analysis.escapedRegion[slot->regionIndex] &&
                !analysis.slots.at(*slot).conflict) {
              if (const auto it = pending.find(*slot); it != pending.end()) {
                deadStores.emplace_back(bid, it->second);
              }
              pending[*slot] = oid;
            } else {
              pending.clear();
            }
          } else {
            pending.clear();
          }
          continue;
        }
        if (op.code == il::OpCode::Call || op.code == il::OpCode::Intrinsic) {
          pending.clear();
        }
      }
    }

    if (deadStores.empty()) {
      return false;
    }
    std::unordered_set<uint32_t> removed;
    for (const auto& [bid, oid] : deadStores) {
      if (removed.insert(oid.index()).second) {
        function.removeOp(bid, oid);
      }
    }
    return !removed.empty();
  }
};

}  // namespace

std::unique_ptr<pass::Pass> makeRegionMem2RegPass() {
  return std::make_unique<RegionMem2Reg>();
}

std::unique_ptr<pass::Pass> makeRegionDsePass() {
  return std::make_unique<RegionDse>();
}

}  // namespace xdec::passes
