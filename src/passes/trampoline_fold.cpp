// makeTrampolineFoldPass: retargets edges around empty forwarding blocks (see
// the header for why the CFG is full of these and why Cfg is the right level
// to remove them).
#include "xdec/passes/trampoline_fold.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "xdec/il/function.h"

namespace xdec::passes {

namespace {

/// A block is a trampoline when it does nothing but jump on: exactly one op,
/// and that op an unconditional branch. Nothing about it — no value, no
/// side effect — is observable between whoever reaches it and wherever it
/// goes, so every edge landing on it can point straight at its target
/// instead without changing what the function computes.
bool isTrampoline(const il::Function& function, il::BlockId block) {
  const il::Block& info = function.block(block);
  if (info.ops.size() != 1) {
    return false;
  }
  return function.op(info.ops[0]).code == il::OpCode::Branch;
}

/// Chases a chain of trampolines to the first block that is not one itself,
/// memoising every link so a long chain — or several trampolines that all
/// feed the same tail — is only ever walked once. `seen` turns a cycle of
/// nothing-but-trampolines into a harmless no-op instead of a hang: such a
/// cycle computes nothing and is unreachable from anywhere real, so leaving
/// it exactly as it was loses no information.
il::BlockId resolveTrampoline(const il::Function& function, il::BlockId start,
                              std::unordered_map<uint32_t, il::BlockId>& cache) {
  std::vector<il::BlockId> chain;
  std::unordered_set<uint32_t> seen;
  il::BlockId at = start;
  while (isTrampoline(function, at)) {
    if (const auto found = cache.find(at.index()); found != cache.end()) {
      at = found->second;
      break;
    }
    if (!seen.insert(at.index()).second) {
      break;  // back to a block already in this chain: a trampoline-only cycle
    }
    chain.push_back(at);
    at = function.targets(function.op(function.block(at).ops.front())).front();
  }
  for (const il::BlockId member : chain) {
    cache[member.index()] = at;
  }
  return at;
}

class TrampolineFold final : public pass::FunctionPass {
 public:
  TrampolineFold()
      : FunctionPass([] {
          pass::PassInfo info;
          info.name = "trampoline-fold";
          info.level = il::Maturity::Cfg;
          info.produces = il::Maturity::Cfg;
          info.requirements = {"cfg-finalize"};
          info.invalidates = {"cfg", "dominators", "scc"};
          return info;
        }()) {}

  Result<bool> run(pass::Context& context) override {
    il::Function& function = context.function();

    std::unordered_map<uint32_t, il::BlockId> cache;
    bool changed = false;
    std::vector<il::BlockId> rewritten;
    for (const il::BlockId blockId : function.blockHandles()) {
      const il::Block& block = function.block(blockId);
      if (block.ops.empty() || block.external) {
        continue;
      }
      const il::OpId terminatorId = block.ops.back();
      const il::Op& terminator = function.op(terminatorId);
      if (!terminator.isTerminator()) {
        continue;
      }
      const std::span<const il::BlockId> targets = function.targets(terminator);
      rewritten.assign(targets.begin(), targets.end());
      bool any = false;
      for (il::BlockId& target : rewritten) {
        if (!isTrampoline(function, target)) {
          continue;
        }
        const il::BlockId dest = resolveTrampoline(function, target, cache);
        if (dest != target) {
          target = dest;
          any = true;
        }
      }
      if (any) {
        function.setTargets(terminatorId, rewritten);
        changed = true;
      }
    }

    if (changed) {
      function.rebuildEdges();
    }
    return changed;
  }
};

}  // namespace

std::unique_ptr<pass::Pass> makeTrampolineFoldPass() {
  return std::make_unique<TrampolineFold>();
}

}  // namespace xdec::passes
