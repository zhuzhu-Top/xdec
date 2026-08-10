// Which blocks the entry can actually reach, read straight off the CFG's own
// edges rather than off the address-space proofs that put those edges there.
//
// Dominators already answers this (Dominators::reachable, backed by the same
// DFS Function::reversePostOrder does) and every emitter already keys off
// that rpo -- a block outside it never enters the structurizer's regions, so
// it never prints. This module exists anyway, as the second half of the
// jump-table over-enumeration defence (see resolve_indirect.cpp and
// index_bound.cpp's boundOnIndex/localBound for the first half): those fixes
// keep a bad candidate from ever becoming an edge, but nothing about that is
// a law of the pass, and a caller here does not want to trust it silently.
// Naming "reachable from entry" as its own small, dependency-free check --
// no dominator tree, just a walk -- gives the driver something to verify
// against after the fact and gives a future regression a place to fail loudly
// instead of just costing an unused local variable in the emitted function.
#pragma once

#include <unordered_set>

#include "xdec/il/function.h"

namespace xdec::analysis {

/// Every block reached from `function.entryBlock()` by a walk over
/// `Block::successors` -- which for an IndirectBranch are only the targets a
/// resolve pass actually set (see passes::resolve-indirect), so a table entry
/// that was never claimed as a candidate never appears here either. Empty
/// when the function has no entry block.
[[nodiscard]] std::unordered_set<il::BlockId> reachableBlocks(const il::Function& function);

}  // namespace xdec::analysis
