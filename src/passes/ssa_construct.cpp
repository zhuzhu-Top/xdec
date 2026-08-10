// makeSsaConstructPass: Cytron phi placement plus a dominator-tree renaming
// walk (see the header for the design stance).
#include "xdec/passes/ssa_construct.h"

#include <algorithm>
#include <charconv>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

#include "xdec/analysis/dominators.h"
#include "xdec/il/function.h"

#include "transform.h"

namespace xdec::passes {

namespace {

/// Whether a register class participates in register SSA. Float, vector and
/// special registers keep their op form: the IL does not model their view
/// structure well enough yet, and an honest op beats an invented version.
[[nodiscard]] bool tracked(il::RegClass regClass) {
  return regClass == il::RegClass::General || regClass == il::RegClass::Flags ||
         regClass == il::RegClass::StackPointer;
}

/// What the renaming walk needs about one phi site: which register it merges
/// and which op is the phi.
struct PhiSite {
  il::RegId root;
  il::OpId op;
};

class SsaBuilder {
 public:
  SsaBuilder(il::Function& function, const analysis::Dominators& dominators)
      : function_(function), dominators_(dominators) {
    for (il::RegId root{0}; root.asSize() < function_.registers().size();
         root = il::RegId{root.index() + 1}) {
      const il::RegisterInfo& info = function_.registers()[root];
      if (info.isSubRegister() || !tracked(info.regClass)) {
        continue;
      }
      roots_.push_back(root);
      versions_.emplace(root, std::vector<il::ExprId>{});
    }
    // Calling-convention registers by name (AAPCS64 today): x0..x7 pass
    // arguments, x0 carries the result. Absent those names, no annotation.
    for (unsigned index = 0; index < 8; ++index) {
      const il::RegId reg =
          function_.registers().find(std::format("x{}", index));
      if (reg.valid()) {
        argRoots_.push_back(function_.registers().rootOf(reg));
      }
    }
    if (const il::RegId x0 = function_.registers().find("x0"); x0.valid()) {
      resultRoots_.push_back(function_.registers().rootOf(x0));
    }
  }

  void build() {
    collectDefSites();
    placePhis();
    // Every version stack starts with the register's entry leaf: distinct per
    // register, so the stack pointer's origin never blurs into the unknown
    // inputs the way a shared undef would blur it.
    for (const il::RegId root : roots_) {
      versions_[root].push_back(function_.entryReg(root));
    }
    renameBlock(function_.entryBlock());
    substituteOperands();
    removeRegisterOps();
  }

 private:
  /// Blocks defining each tracked root. A call is a definition of everything
  /// it clobbers.
  ///
  /// Skips blocks unreachable from the entry: a block reachable only through
  /// an indirect branch this pipeline stage has not resolved yet has no edge
  /// in the CFG, so renameBlock's dominator-tree walk will never visit it (see
  /// substituteOperands for why that must hold everywhere its register ops are
  /// touched).
  void collectDefSites() {
    for (const il::BlockId blockId : function_.blockHandles()) {
      if (!dominators_.reachable(blockId)) {
        continue;
      }
      for (const il::OpId opId : function_.block(blockId).ops) {
        const il::Op& op = function_.op(opId);
        if (op.code == il::OpCode::WriteReg) {
          const il::RegId root = function_.registers().rootOf(op.reg());
          if (versions_.contains(root)) {
            defBlocks_[root].insert(blockId);
          }
        } else if (op.code == il::OpCode::Call) {
          for (const il::RegId root : roots_) {
            if (clobbers(root)) {
              defBlocks_[root].insert(blockId);
            }
          }
        }
      }
    }
    // The value a register arrives with is a definition of it, in the entry
    // block. Leaving it out makes a register written in exactly one block look
    // like it needs no merge anywhere, which is wrong the moment that block is
    // its own successor: `subs x1, x1, #1` at a loop latch would then read the
    // register's entry value on every iteration, and a countdown loop comes out
    // testing a constant.
    for (auto& [root, blocks] : defBlocks_) {
      blocks.insert(function_.entryBlock());
    }
  }

  /// Whether a call destroys the register's contents, by the usual calling
  /// convention: caller-saved registers die, callee-saved ones survive. The
  /// sets are name-pattern based (x0..x18 and x30 clobbered on AArch64,
  /// vector argument registers q0..q7, anything unrecognised conservatively
  /// clobbered); an exotic register file simply falls back to clobbering,
  /// never to pretending a value survives.
  [[nodiscard]] bool clobbers(il::RegId root) const {
    const il::RegisterInfo& info = function_.registers()[root];
    if (info.regClass == il::RegClass::StackPointer ||
        info.regClass == il::RegClass::Zero) {
      return false;
    }
    const std::string_view name = function_.registers().nameOf(root);
    if (name.size() >= 2 && (name[0] == 'x' || name[0] == 'q' || name[0] == 'v')) {
      unsigned number = 0;
      const char* begin = name.data() + 1;
      const char* end = name.data() + name.size();
      if (const auto [ptr, ec] = std::from_chars(begin, end, number);
          ec == std::errc{} && ptr == end) {
        if (name[0] == 'x') {
          return number <= 18 || number == 30;
        }
        return number <= 7;
      }
    }
    return true;
  }

  /// Cytron over the dominance frontier, per register with more than one
  /// def-site.
  void placePhis() {
    for (const il::RegId root : roots_) {
      const auto found = defBlocks_.find(root);
      if (found == defBlocks_.end() || found->second.size() < 2) {
        continue;
      }
      std::vector<il::BlockId> worklist(found->second.begin(), found->second.end());
      std::set<il::BlockId> placed;
      // Def-sites grow as phis count as definitions; track the closure.
      std::set<il::BlockId> everOnWorklist(found->second.begin(), found->second.end());
      while (!worklist.empty()) {
        const il::BlockId defsite = worklist.back();
        worklist.pop_back();
        for (const il::BlockId join : dominators_.frontier(defsite)) {
          if (!placed.insert(join).second) {
            continue;
          }
          const il::Type type = function_.registers()[root].type();
          const il::ValueId merged = function_.prependPhi(join, function_.block(join).va, type);
          const il::OpId definition = function_.value(merged).definition;
          phiSites_[join].push_back({root, definition});
          phiValues_[{join, root}] = merged;
          // Which register this phi merges, so a later analysis (see
          // analysis::matchLiveRegisterFrame) can find a specific one at a
          // specific block directly instead of re-deriving it from variable
          // naming order, which carries no such guarantee.
          function_.annotate(definition, std::format("reg:{}", function_.registers().nameOf(root)));
          // The phi is itself a definition of the register in that block.
          if (everOnWorklist.insert(join).second) {
            worklist.push_back(join);
          }
        }
      }
    }
  }

  /// Dominator-tree preorder: at each block, phis define first, ops rewrite
  /// the version state, successors receive phi inputs, then children recurse.
  void renameBlock(il::BlockId blockId) {
    std::map<il::RegId, std::size_t> pushed;

    const auto pushVersion = [&](il::RegId root, il::ExprId version) {
      versions_[root].push_back(version);
      ++pushed[root];
    };
    const auto current = [&](il::RegId root) { return versions_[root].back(); };

    for (const PhiSite& site : phiSites_[blockId]) {
      pushVersion(site.root, function_.valueRef(phiValues_[{blockId, site.root}]));
    }

    for (const il::OpId opId : function_.block(blockId).ops) {
      const il::Op& op = function_.op(opId);
      switch (op.code) {
        case il::OpCode::ReadReg: {
          const il::RegId root = function_.registers().rootOf(op.reg());
          if (!versions_.contains(root)) {
            break;  // untracked class: the op stays
          }
          subst_.set(op.result, readAs(function_.registers()[op.reg()], current(root)));
          break;
        }
        case il::OpCode::WriteReg: {
          const il::RegId root = function_.registers().rootOf(op.reg());
          if (!versions_.contains(root)) {
            break;
          }
          // The written expression becomes the register's new version, merged
          // into the root's shape when the access is a view.
          const auto operands = function_.operands(op);
          const il::ExprId written = subst_.apply(function_, operands[0]);
          pushVersion(root, writeAs(root, function_.registers()[op.reg()], written));
          break;
        }
        case il::OpCode::Call: {
          // The calling-convention arguments are exactly the argument
          // registers' current versions; record them before the clobber
          // below, so later stages (emission) see what was passed without
          // re-deriving SSA.
          std::vector<il::ExprId> args;
          for (const il::RegId root : argRoots_) {
            if (versions_.contains(root)) {
              args.push_back(current(root));
            }
          }
          callArgs_[opId] = std::move(args);
          for (const il::RegId root : roots_) {
            if (!clobbers(root)) {
              continue;
            }
            // The result register's new version is the call's defined value:
            // "whatever the callee returned", one distinct unknown per call,
            // which is precisely what consumers of the return value need.
            if (!resultRoots_.empty() && root == resultRoots_.front() &&
                op.result.valid()) {
              pushVersion(root, function_.valueRef(op.result));
              continue;
            }
            pushVersion(root, function_.undefined(function_.registers()[root].type()));
          }
          break;
        }
        case il::OpCode::IndirectBranch: {
          // A computed branch that nothing has resolved yet might be a jump
          // within this function or a tail call out of it, and which one is not
          // knowable here. So the argument registers' versions are recorded the
          // same way a call's are: if the branch turns out to be a tail call,
          // those values are its arguments, and by the time anything can tell,
          // the instructions that set them are long since folded away or dead.
          //
          // Nothing is claimed by recording them. resolve-indirect drops them
          // again the moment the branch resolves to blocks in this function.
          if (!function_.targets(op).empty()) {
            break;
          }
          std::vector<il::ExprId> args;
          for (const il::RegId root : argRoots_) {
            if (versions_.contains(root)) {
              args.push_back(current(root));
            }
          }
          callArgs_[opId] = std::move(args);
          break;
        }
        case il::OpCode::Return: {
          // The return value is the first result register's version.
          if (!resultRoots_.empty() && versions_.contains(resultRoots_.front())) {
            returnValues_[opId] = current(resultRoots_.front());
          }
          break;
        }
        default:
          break;
      }
    }

    // Phi inputs for successors, in each successor's predecessor order.
    for (const il::BlockId succ : function_.block(blockId).successors) {
      const auto sites = phiSites_.find(succ);
      if (sites == phiSites_.end()) {
        continue;
      }
      const auto& predecessors = function_.block(succ).predecessors;
      // A conditional branch with both arms here lists this block twice;
      // every matching slot gets the same version.
      for (uint32_t index = 0; index < predecessors.size(); ++index) {
        if (predecessors[index] != blockId) {
          continue;
        }
        for (const PhiSite& site : sites->second) {
          phiInputs_[site.op].emplace(index, current(site.root));
        }
      }
    }

    for (const il::BlockId child : dominators_.children(blockId)) {
      renameBlock(child);
    }

    // Pop everything this block pushed.
    for (const auto& [root, count] : pushed) {
      std::vector<il::ExprId>& stack = versions_[root];
      stack.resize(stack.size() - count);
    }
  }

  /// The expression a read of `reg` denotes given the root's current version.
  [[nodiscard]] il::ExprId readAs(const il::RegisterInfo& info, il::ExprId rootVersion) {
    if (!info.isSubRegister()) {
      return rootVersion;
    }
    return function_.extract(info.type(), rootVersion, info.offsetInParent);
  }

  /// The root's new version after writing `written` through `info`'s view.
  [[nodiscard]] il::ExprId writeAs(il::RegId root, const il::RegisterInfo& info,
                                   il::ExprId written) {
    if (!info.isSubRegister()) {
      return written;
    }
    const il::Type rootType = function_.registers()[root].type();
    if (info.zeroExtendsParent) {
      return function_.cast(il::ExprOp::ZExt, rootType, written);
    }
    // A merging partial write: the honest version is "unknown".
    return function_.undefined(rootType);
  }

  /// Every surviving op's operands get the value substitution. Register ops
  /// are about to vanish, but rewriting them too is harmless and uniform.
  ///
  /// Skips blocks the rename walk never visited (unreachable at this pipeline
  /// stage: their only path in is an indirect branch nothing has resolved
  /// yet). `subst_` has no entries for a value such a block's own ReadReg
  /// defines, so rewriting here would be a no-op at best; the real hazard is
  /// removeRegisterOps deleting that same ReadReg right after, which would
  /// leave any use of its value dangling. Leaving the block's register ops
  /// alone keeps it self-consistent until a later round makes it reachable
  /// and this pass runs again.
  void substituteOperands() {
    for (const il::BlockId blockId : function_.blockHandles()) {
      if (!dominators_.reachable(blockId)) {
        continue;
      }
      for (const il::OpId opId : function_.block(blockId).ops) {
        const il::Op& op = function_.op(opId);
        if (op.code == il::OpCode::Phi) {
          const auto found = phiInputs_.find(opId);
          if (found == phiInputs_.end()) {
            continue;
          }
          const auto& predecessors = function_.block(blockId).predecessors;
          std::vector<il::ExprId> incoming(predecessors.size(), il::ExprId{});
          for (const auto& [index, expr] : found->second) {
            incoming[index] = expr;
          }
          // Unreachable predecessors contribute undef: the edge exists but
          // the renaming never visited across it.
          for (std::size_t i = 0; i < incoming.size(); ++i) {
            if (!incoming[i].valid()) {
              incoming[i] = function_.undefined(op.type);
            }
          }
          function_.setOperands(opId, incoming);
          continue;
        }
        // No early exit on an empty operand list: `ret` has none, and the
        // annotation below is the whole point of visiting it. Skipping it here
        // is what made every function return void.
        const auto operands = function_.operands(op);
        std::vector<il::ExprId> rewritten(operands.begin(), operands.end());
        bool changed = false;
        for (il::ExprId& operand : rewritten) {
          const il::ExprId next = subst_.apply(function_, operand);
          changed |= next != operand;
          operand = next;
        }
        // Calling-convention annotations, captured mid-rename (see the Call
        // and Return cases there): the versions are final expressions, so
        // they append after substitution untouched.
        if (const auto found = callArgs_.find(opId); found != callArgs_.end()) {
          rewritten.insert(rewritten.end(), found->second.begin(), found->second.end());
          changed = true;
        }
        if (const auto found = returnValues_.find(opId); found != returnValues_.end()) {
          rewritten.push_back(found->second);
          changed = true;
        }
        if (changed) {
          function_.setOperands(opId, rewritten);
        }
      }
    }
  }

  /// ReadReg/WriteReg of tracked registers have said everything they had to
  /// say; the values carry the flow now.
  ///
  /// Unreachable-at-this-stage blocks are exempt, same reason as
  /// substituteOperands: their register ops were never renamed, so removing
  /// them would tombstone a definition some op in the very same block still
  /// reads.
  void removeRegisterOps() {
    for (const il::BlockId blockId : function_.blockHandles()) {
      if (!dominators_.reachable(blockId)) {
        continue;
      }
      std::vector<il::OpId> removable;
      for (const il::OpId opId : function_.block(blockId).ops) {
        const il::Op& op = function_.op(opId);
        if ((op.code == il::OpCode::ReadReg || op.code == il::OpCode::WriteReg) &&
            versions_.contains(function_.registers().rootOf(op.reg()))) {
          removable.push_back(opId);
        }
      }
      for (const il::OpId opId : removable) {
        function_.removeOp(blockId, opId);
      }
    }
  }

  il::Function& function_;
  const analysis::Dominators& dominators_;
  std::vector<il::RegId> roots_;
  /// Calling-convention registers, looked up by the usual names; an exotic
  /// register file simply records no arguments (emission degrades, never
  /// guesses).
  std::vector<il::RegId> argRoots_;
  std::vector<il::RegId> resultRoots_;
  /// Call op -> argument register versions at the call, appended to the op's
  /// operands in substituteOperands. Return op -> result register version.
  std::map<il::OpId, std::vector<il::ExprId>> callArgs_;
  std::map<il::OpId, il::ExprId> returnValues_;
  std::map<il::RegId, std::vector<il::ExprId>> versions_;
  std::map<il::RegId, std::set<il::BlockId>> defBlocks_;
  std::map<il::BlockId, std::vector<PhiSite>> phiSites_;
  std::map<std::pair<il::BlockId, il::RegId>, il::ValueId> phiValues_;
  /// Phi op -> (predecessor index -> incoming expression), assembled per edge.
  std::map<il::OpId, std::map<uint32_t, il::ExprId>> phiInputs_;
  ValueSubst subst_;
};

class SsaConstruct final : public pass::FunctionPass {
 public:
  SsaConstruct()
      : FunctionPass([] {
          pass::PassInfo info;
          info.name = "ssa-construct";
          info.level = il::Maturity::Cfg;
          info.produces = il::Maturity::Ssa;
          info.invalidates = {"dominators", "scc"};
          return info;
        }()) {}

  Result<bool> run(pass::Context& context) override {
    il::Function& function = context.function();
    const analysis::Dominators dominators = analysis::Dominators::compute(function);
    SsaBuilder(function, dominators).build();
    return true;
  }
};

}  // namespace

std::unique_ptr<pass::Pass> makeSsaConstructPass() {
  return std::make_unique<SsaConstruct>();
}

}  // namespace xdec::passes
