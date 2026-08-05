// Back edges, natural loops, and the SCC-based reducibility test.
#include "xdec/analysis/loops.h"

#include <map>

namespace xdec::analysis {

std::vector<BackEdge> backEdges(const il::Function& function,
                                const Dominators& dominators) {
  std::vector<BackEdge> edges;
  for (const il::BlockId from : dominators.rpo()) {
    for (const il::BlockId to : function.block(from).successors) {
      if (dominators.dominates(to, from)) {
        edges.push_back({from, to});
      }
    }
  }
  return edges;
}

NaturalLoop naturalLoop(const il::Function& function, il::BlockId header,
                        std::span<const il::BlockId> latches) {
  NaturalLoop loop;
  loop.header = header;
  loop.latches.assign(latches.begin(), latches.end());
  loop.blocks.insert(header);

  // Walk predecessors from each latch; everything reached before the header
  // is loop body. Classic definition, and deliberately cheap.
  std::vector<il::BlockId> worklist;
  for (const il::BlockId latch : latches) {
    if (latch != header && loop.blocks.insert(latch).second) {
      worklist.push_back(latch);
    }
  }
  while (!worklist.empty()) {
    const il::BlockId block = worklist.back();
    worklist.pop_back();
    for (const il::BlockId pred : function.block(block).predecessors) {
      if (loop.blocks.insert(pred).second) {
        worklist.push_back(pred);
      }
    }
  }
  return loop;
}

std::vector<NaturalLoop> naturalLoops(const il::Function& function,
                                      const Dominators& dominators) {
  std::map<il::BlockId, std::vector<il::BlockId>> byHeader;
  for (const BackEdge& edge : backEdges(function, dominators)) {
    byHeader[edge.header].push_back(edge.from);
  }
  std::vector<NaturalLoop> loops;
  for (const auto& [header, latches] : byHeader) {
    loops.push_back(naturalLoop(function, header, latches));
  }
  return loops;
}

bool isReducible(const il::Function& function, const Sccs& sccs) {
  for (const Sccs::Component& component : sccs.components()) {
    if (!component.cyclic) {
      continue;
    }
    // Entries from outside the component must all land on one block. With a
    // single-entry region the header dominates the body and standard loop
    // transforms apply; two or more means an irreducible region.
    il::BlockId entry{};
    for (const il::BlockId member : component.blocks) {
      for (const il::BlockId pred : function.block(member).predecessors) {
        if (sccs.componentOf(pred) == il::BlockId::kInvalidIndex) {
          continue;  // unreachable from the entry: not part of this judgement
        }
        if (sccs.componentOf(pred) == sccs.componentOf(member)) {
          continue;  // internal edge
        }
        if (!entry.valid()) {
          entry = member;
        } else if (entry != member) {
          return false;
        }
      }
    }
  }
  return true;
}

}  // namespace xdec::analysis
