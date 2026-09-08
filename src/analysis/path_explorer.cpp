// PathExplorer (see the header).
#include "xdec/analysis/path_explorer.h"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include "xdec/analysis/path_interpreter.h"
#include "xdec/analysis/path_state.h"
#include "xdec/analysis/stack_canary.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/analysis/sym_value.h"
#include "xdec/binary/cache_pointer.h"

namespace xdec::analysis {

namespace {

/// Whether `blockId` is "just a call": every op before the terminator is a
/// Call or a Nop, exactly one Call, and the terminator does not return
/// control anywhere this walk would otherwise follow. This is the
/// `-fstack-protector` mismatch arm's shape on every target this project
/// supports (`bl __stack_chk_fail` alone, falling into an Unreachable the
/// lifter's noreturn handling appends, or occasionally a bare Return) --
/// structural, so it needs no symbol name to recognise, which matters
/// exactly where docs/22-dyld-shared-cache.md notes this project cannot yet
/// confirm one (a dyld shared cache image with no export-trie parsing).
///
/// Deliberately narrow: a block that does anything else first (even a single
/// arithmetic op) does not match, because "narrow and sometimes silent" is
/// the safe direction to be wrong in here -- a missed mismatch arm is merely
/// explored like any other edge (see PathExploreOptions::exploreCanaryFailPaths),
/// while a wrongly suppressed real edge would hide a path that matters.
[[nodiscard]] bool looksLikeBareCallBlock(const il::Function& function, il::BlockId blockId) {
  const il::Block& block = function.block(blockId);
  if (block.ops.empty()) {
    return false;
  }
  unsigned calls = 0;
  for (std::size_t index = 0; index + 1 < block.ops.size(); ++index) {
    const il::Op& op = function.op(block.ops[index]);
    if (op.code == il::OpCode::Call) {
      ++calls;
      continue;
    }
    if (op.code == il::OpCode::Nop) {
      continue;
    }
    return false;
  }
  if (calls != 1) {
    return false;
  }
  const il::Op& terminator = function.op(block.ops.back());
  return terminator.code == il::OpCode::Unreachable || terminator.code == il::OpCode::Return;
}

/// One continuation of the exploration: a block to step, the edge it was
/// reached by, and everything one specific walk from the entry has resolved
/// so far.
struct Frontier {
  il::BlockId block;
  il::BlockId cameFrom;
  PathState state;
};

}  // namespace

PathExplorer::PathExplorer(const il::Function& function, ByteReader reader,
                           const EntryRegFacts* entryRegs, PathExploreOptions options)
    : function_(&function), reader_(std::move(reader)), entryRegs_(entryRegs),
      options_(options) {}

bool PathExplorer::readable(uint64_t va) const {
  std::array<std::byte, 4> word{};
  return static_cast<bool>(reader_(va, word));
}

void PathExplorer::recordIndirectTarget(uint64_t branchVa, const SymValueSet& targetSet) {
  if (targetSet.isTop() || targetSet.values().empty()) {
    return;
  }
  static constexpr binary::CachePointerDecoder kCachePointer;
  std::vector<uint64_t> filtered;
  for (const uint64_t va : targetSet.values()) {
    // Zero for the same reason resolve_indirect.cpp's valueSetCandidates
    // excludes it: an unrelocated pointer slot reads as zero, and zero is
    // not code.
    if (va == 0) {
      continue;
    }
    uint64_t candidate = va;
    if (!readable(candidate)) {
      const uint64_t decoded = kCachePointer.decode(candidate);
      if (decoded == candidate || !readable(decoded)) {
        continue;
      }
      candidate = decoded;
    }
    filtered.push_back(candidate);
  }
  if (filtered.empty()) {
    return;
  }
  std::vector<uint64_t>& out = results_[branchVa];
  for (const uint64_t candidate : filtered) {
    if (std::find(out.begin(), out.end(), candidate) == out.end()) {
      out.push_back(candidate);
    }
  }
}

const std::vector<uint64_t>* PathExplorer::targetsFor(uint64_t branchVa) const {
  const auto found = results_.find(branchVa);
  return found == results_.end() ? nullptr : &found->second;
}

void PathExplorer::explore() {
  if (explored_) {
    return;
  }
  explored_ = true;
  if (!options_.enabled || !function_->entryBlock().valid()) {
    return;
  }

  // One snapshot of every canary check's likely mismatch arm, taken once
  // up front -- same reasoning resolve-indirect's own Dominators/ImageEval
  // snapshot uses (see its run()): every candidate answer below should see
  // one consistent picture of the function, not one that shifts as the walk
  // proceeds.
  const StackFrame frame = StackFrame::compute(*function_);
  for (const StackCanarySave& save : findStackCanarySaves(*function_, frame)) {
    if (!function_->hasOp(save.check)) {
      continue;
    }
    const il::Op& check = function_->op(save.check);
    const auto targets = function_->targets(check);
    if (targets.size() != 2) {
      continue;
    }
    const bool firstBare = looksLikeBareCallBlock(*function_, targets[0]);
    const bool secondBare = looksLikeBareCallBlock(*function_, targets[1]);
    if (firstBare != secondBare) {
      canaryFailSuccessor_[save.check.index()] = firstBare ? targets[0] : targets[1];
    }
  }

  const PathInterpreter interp(*function_);
  std::vector<Frontier> worklist;
  worklist.push_back(Frontier{function_->entryBlock(), il::BlockId::invalid(),
                              PathState(*function_, reader_, entryRegs_)});

  while (!worklist.empty() && pathsExplored_ < options_.maxPaths) {
    Frontier item = std::move(worklist.back());
    worklist.pop_back();
    ++pathsExplored_;

    PathState& state = item.state;
    if (state.steps >= options_.maxStepsPerPath) {
      continue;
    }
    if (state.blockVisits(item.block) >= options_.maxBlockRevisits) {
      continue;
    }
    state.markVisited(item.block);
    ++state.steps;

    const PathOutcome outcome = interp.stepBlock(item.block, item.cameFrom, state);
    switch (outcome.stop) {
      case PathStop::Branch:
        worklist.push_back(Frontier{outcome.target, item.block, std::move(state)});
        break;

      case PathStop::CondBranch: {
        const SymValueSet condition = state.eval(outcome.condition);
        bool pushTrue = true;
        bool pushFalse = true;
        if (!condition.isTop() && condition.values().size() == 1) {
          if (condition.values()[0] != 0) {
            pushFalse = false;
          } else {
            pushTrue = false;
          }
        }
        if (!options_.exploreCanaryFailPaths) {
          if (const auto found = canaryFailSuccessor_.find(outcome.op.index());
              found != canaryFailSuccessor_.end()) {
            if (found->second == outcome.ifTrue) {
              pushTrue = false;
            }
            if (found->second == outcome.ifFalse) {
              pushFalse = false;
            }
          }
        }
        if (pushTrue && pushFalse) {
          PathState branch = state;
          worklist.push_back(Frontier{outcome.ifTrue, item.block, std::move(branch)});
          worklist.push_back(Frontier{outcome.ifFalse, item.block, std::move(state)});
        } else if (pushTrue) {
          worklist.push_back(Frontier{outcome.ifTrue, item.block, std::move(state)});
        } else if (pushFalse) {
          worklist.push_back(Frontier{outcome.ifFalse, item.block, std::move(state)});
        }
        break;
      }

      case PathStop::IndirectBranch: {
        if (!outcome.resolvedTargets.empty()) {
          // A branch a prior pass this same run already resolved: follow
          // every one of its targets like any other multi-way edge, rather
          // than treat it as this walk's own thing to solve.
          for (std::size_t index = 0; index < outcome.resolvedTargets.size(); ++index) {
            const il::BlockId target = outcome.resolvedTargets[index];
            if (index + 1 == outcome.resolvedTargets.size()) {
              worklist.push_back(Frontier{target, item.block, std::move(state)});
            } else {
              PathState branch = state;
              worklist.push_back(Frontier{target, item.block, std::move(branch)});
            }
          }
        } else {
          recordIndirectTarget(outcome.va, state.eval(outcome.indirectTarget));
        }
        break;
      }

      case PathStop::Return:
      case PathStop::Unreachable:
      case PathStop::Stopped:
      default:
        break;
    }
  }
}

}  // namespace xdec::analysis
