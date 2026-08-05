// Loops and reducibility.
//
// A back edge is an edge whose head dominates its tail; the natural loop of a
// back edge is the header plus everything that can reach the latch without
// going through the header. These are the definitions the structurer and the
// obfuscation profile detector build on, and reducibility — whether every
// cyclic region has a single entry — is the property that decides whether
// pattern-free structuring (DREAM) can proceed directly or must first
// duplicate code.
#pragma once

#include <set>
#include <span>
#include <vector>

#include "xdec/analysis/dominators.h"
#include "xdec/analysis/scc.h"
#include "xdec/il/function.h"

namespace xdec::analysis {

struct BackEdge {
  /// The block the edge leaves: the loop latch.
  il::BlockId from;
  /// The block the edge enters: the loop header.
  il::BlockId header;
};

/// Every edge (a, b) where b dominates a, over the reachable graph. A
/// self-loop qualifies, which is what keeps the definition honest on
/// one-block loops.
[[nodiscard]] std::vector<BackEdge> backEdges(const il::Function& function,
                                              const Dominators& dominators);

struct NaturalLoop {
  il::BlockId header;
  /// Back-edge sources feeding this header.
  std::vector<il::BlockId> latches;
  /// Header plus every block that can reach a latch without passing the
  /// header. A std::set because membership tests dominate the uses.
  std::set<il::BlockId> blocks;
};

/// The natural loop shared by a set of latches of one header.
[[nodiscard]] NaturalLoop naturalLoop(const il::Function& function, il::BlockId header,
                                      std::span<const il::BlockId> latches);

/// All natural loops, grouped by header from the back edges.
[[nodiscard]] std::vector<NaturalLoop> naturalLoops(const il::Function& function,
                                                    const Dominators& dominators);

/// Whether the reachable graph is reducible: every cyclic SCC has a single
/// block receiving all entries from outside the SCC. When this is false the
/// structurer must either duplicate code or emit a dispatch loop, and the
/// obfuscation profile detector takes note, because irreducibility plus a
/// state variable is the fingerprint of control-flow flattening.
[[nodiscard]] bool isReducible(const il::Function& function, const Sccs& sccs);

}  // namespace xdec::analysis
