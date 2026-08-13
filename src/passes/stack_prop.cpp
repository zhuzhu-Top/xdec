// makeStackPropPass: frame-address canonicalisation and store-to-load
// forwarding (see the header for the rules and the deliberate omission).
#include "xdec/passes/stack_prop.h"

#include <algorithm>
#include <unordered_set>
#include <utility>
#include <vector>

#include "xdec/analysis/stack_frame.h"
#include "xdec/il/function.h"

#include "transform.h"

namespace xdec::passes {

namespace {

/// One remembered memory source: the expression a canonical address holds,
/// from either a store the block made or a load it already paid for. Load
/// results are sources too — a second load of an untouched address is the
/// first load's value, which is where the dispatcher's constant reloads go
/// to die.
struct MemSource {
  il::ExprId address;  // canonical: the normal-form slot or a constant
  il::ExprId data;
  unsigned size = 0;
};

class StackProp {
 public:
  explicit StackProp(il::Function& function)
      : function_(function), frame_(analysis::StackFrame::compute(function)) {
    spLeaf_ = frame_.stackPointer().valid() ? function_.entryReg(frame_.stackPointer())
                                            : il::ExprId{};
  }

  bool run() {
    if (spLeaf_.valid()) {
      for (const il::BlockId blockId : function_.blockHandles()) {
        walkBlock(blockId);
      }
    }
    const bool globalsForwarded = forwardStoreFreeGlobals();
    const bool forwarded = applyForwarding();
    return canonicalized_ || forwarded || globalsForwarded;
  }

 private:
  /// The normal form of a slot address: `entry(sp)` itself at zero, else a
  /// single add. Everything stack-derived collapses onto one of these per
  /// delta, so slot identity becomes an ExprId comparison.
  [[nodiscard]] il::ExprId canonical(int64_t delta) {
    if (delta == 0) {
      return spLeaf_;
    }
    return function_.binary(il::ExprOp::Add, spLeaf_,
                            function_.constant(spLeafType(), static_cast<uint64_t>(delta)));
  }

  [[nodiscard]] il::Type spLeafType() const { return function_.expr(spLeaf_).type; }

  /// The canonical key for a memory op's address: stack-derived addresses are
  /// rewritten to the normal form (and the op's operand with them), constants
  /// are already canonical, and anything else is invalid — untrackable.
  [[nodiscard]] il::ExprId canonicalAddress(il::OpId opId) {
    const il::ExprId address = function_.operands(function_.op(opId))[0];
    const analysis::AddressInfo info = frame_.classify(address);
    switch (info.kind) {
      case analysis::AddressKind::Global:
        return address;
      case analysis::AddressKind::StackSlot: {
        const il::ExprId normal = canonical(info.delta);
        if (address != normal) {
          const auto operands = function_.operands(function_.op(opId));
          std::vector<il::ExprId> rewritten(operands.begin(), operands.end());
          rewritten[0] = normal;
          function_.setOperands(opId, rewritten);
          canonicalized_ = true;
        }
        return normal;
      }
      case analysis::AddressKind::Other:
        return il::ExprId{};
    }
    return il::ExprId{};
  }

  void walkBlock(il::BlockId blockId) {
    /// What the block knows memory holds, since the last barrier.
    std::vector<MemSource> sources;
    for (const il::OpId opId : function_.block(blockId).ops) {
      const il::Op& op = function_.op(opId);
      switch (op.code) {
        case il::OpCode::Store: {
          const il::ExprId address = canonicalAddress(opId);
          if (!address.valid()) {
            // An unclassified store may write the frame through a pointer the
            // analysis cannot see. Global constants cannot (frame and image
            // are disjoint), so only Other clears.
            if (frame_.classify(function_.operands(op)[0]).kind ==
                analysis::AddressKind::Other) {
              sources.clear();
            }
            break;
          }
          const unsigned size = op.type.bits() / 8;
          // Forget whatever the new bytes may overlap, then remember the
          // write. The data operand is recorded pre-substituted, keeping the
          // replacement-closure invariant ValueSubst relies on.
          std::erase_if(sources, [&](const MemSource& source) {
            return frame_.mayAlias(address, size, source.address, source.size) !=
                   analysis::AliasResult::No;
          });
          const il::ExprId data = function_.operands(op)[1];
          sources.push_back({address, subst_.apply(function_, data), size});
          break;
        }
        case il::OpCode::Load: {
          const il::ExprId address = canonicalAddress(opId);
          if (!address.valid()) {
            break;
          }
          const unsigned size = op.type.bits() / 8;
          const auto found = std::find_if(sources.begin(), sources.end(),
                                          [&](const MemSource& source) {
                                            return source.address == address;
                                          });
          if (found == sources.end()) {
            // First touch of this address in the block: its value becomes the
            // source any reload draws on.
            sources.push_back({address, function_.valueRef(op.result), size});
            break;
          }
          const il::Type loadType = op.type;
          const il::Type sourceType = function_.expr(found->data).type;
          if (sourceType == loadType) {
            subst_.set(op.result, found->data);
            forwarded_.push_back({blockId, opId});
          } else if (sourceType.isScalarInteger() && loadType.isScalarInteger() &&
                     sourceType.bits() > loadType.bits()) {
            // Little-endian: a narrower read of a wider source is the low bits.
            subst_.set(op.result,
                       function_.cast(il::ExprOp::Trunc, loadType, found->data));
            forwarded_.push_back({blockId, opId});
          } else {
            sources.push_back({address, function_.valueRef(op.result), size});
          }
          break;
        }
        case il::OpCode::Call:
        case il::OpCode::Intrinsic:
        case il::OpCode::Unimplemented:
          // The callee reads its outgoing arguments off the stack and writes
          // through every pointer it holds; the opaque may do anything at all.
          sources.clear();
          break;
        default:
          break;
      }
    }
  }

  /// Whole-function reuse of a global load, for the case `walkBlock`'s
  /// per-block reset cannot reach: the same address reloaded from a
  /// different block. Sound whenever nothing anywhere in the function could
  /// have written it -- any store to the address, any store the analysis
  /// cannot classify (which may alias anything), or any opaque call
  /// disqualifies every global at once, the same clobber rule `walkBlock`
  /// already applies per block, just checked across the whole function
  /// instead of forgotten at each one. When it holds, the address denotes
  /// the same value for the life of the function: a caller-established
  /// constant the decompiler cannot name, but can still stop reloading,
  /// which is what lets an MBA idiom see two occurrences of the same operand
  /// instead of two different loads of it.
  bool forwardStoreFreeGlobals() {
    bool clobbered = false;
    std::unordered_set<il::ExprId> storedAddresses;
    std::unordered_set<il::OpId> alreadyForwarded;
    for (const auto& [blockId, opId] : forwarded_) {
      alreadyForwarded.insert(opId);
    }
    for (const il::BlockId blockId : function_.blockHandles()) {
      for (const il::OpId opId : function_.block(blockId).ops) {
        const il::Op& op = function_.op(opId);
        if (op.code == il::OpCode::Call || op.code == il::OpCode::Intrinsic ||
            op.code == il::OpCode::Unimplemented) {
          clobbered = true;
        } else if (op.code == il::OpCode::Store) {
          const il::ExprId address = function_.operands(op)[0];
          const analysis::AddressInfo info = frame_.classify(address);
          if (info.kind == analysis::AddressKind::Global) {
            storedAddresses.insert(address);
          } else if (info.kind == analysis::AddressKind::Other) {
            clobbered = true;
          }
        }
      }
    }
    if (clobbered) {
      return false;
    }

    struct Source {
      il::ExprId address;
      il::ExprId data;
    };
    std::vector<Source> sources;
    // A substitution of its own, not the shared `subst_`: that one's `apply`
    // memoizes per ExprId, and `walkBlock` above already asked it about some
    // of these operands (to pre-substitute store data) before this loop
    // could have known they were forwardable. Reusing it would return those
    // stale, unsubstituted answers forever. A fresh map has no such history.
    ValueSubst globalSubst;
    std::vector<std::pair<il::BlockId, il::OpId>> globallyForwarded;
    for (const il::BlockId blockId : function_.blockHandles()) {
      for (const il::OpId opId : function_.block(blockId).ops) {
        const il::Op& op = function_.op(opId);
        if (op.code != il::OpCode::Load || alreadyForwarded.contains(opId)) {
          continue;
        }
        const il::ExprId address = function_.operands(op)[0];
        if (frame_.classify(address).kind != analysis::AddressKind::Global ||
            storedAddresses.contains(address)) {
          continue;
        }
        const auto found = std::find_if(sources.begin(), sources.end(),
                                        [&](const Source& source) {
                                          return source.address == address;
                                        });
        if (found == sources.end()) {
          sources.push_back({address, function_.valueRef(op.result)});
          continue;
        }
        const il::Type loadType = op.type;
        const il::Type sourceType = function_.expr(found->data).type;
        if (sourceType == loadType) {
          globalSubst.set(op.result, found->data);
        } else if (sourceType.isScalarInteger() && loadType.isScalarInteger() &&
                   sourceType.bits() > loadType.bits()) {
          globalSubst.set(op.result, function_.cast(il::ExprOp::Trunc, loadType, found->data));
        } else {
          sources.push_back({address, function_.valueRef(op.result)});
          continue;
        }
        globallyForwarded.push_back({blockId, opId});
      }
    }
    if (globallyForwarded.empty()) {
      return false;
    }
    for (const il::BlockId blockId : function_.blockHandles()) {
      for (const il::OpId opId : function_.block(blockId).ops) {
        const auto operands = function_.operands(function_.op(opId));
        if (operands.empty()) {
          continue;
        }
        std::vector<il::ExprId> rewritten(operands.begin(), operands.end());
        bool touched = false;
        for (il::ExprId& operand : rewritten) {
          const il::ExprId next = globalSubst.apply(function_, operand);
          touched |= next != operand;
          operand = next;
        }
        if (touched) {
          function_.setOperands(opId, rewritten);
        }
      }
    }
    forwarded_.insert(forwarded_.end(), globallyForwarded.begin(), globallyForwarded.end());
    return true;
  }

  /// Rewrites every use of a forwarded load's value with the stored
  /// expression, then removes the loads themselves.
  bool applyForwarding() {
    if (forwarded_.empty()) {
      return false;
    }
    for (const il::BlockId blockId : function_.blockHandles()) {
      for (const il::OpId opId : function_.block(blockId).ops) {
        const auto operands = function_.operands(function_.op(opId));
        if (operands.empty()) {
          continue;
        }
        std::vector<il::ExprId> rewritten(operands.begin(), operands.end());
        bool touched = false;
        for (il::ExprId& operand : rewritten) {
          const il::ExprId next = subst_.apply(function_, operand);
          touched |= next != operand;
          operand = next;
        }
        if (touched) {
          function_.setOperands(opId, rewritten);
        }
      }
    }
    for (const auto& [blockId, opId] : forwarded_) {
      function_.removeOp(blockId, opId);
    }
    return true;
  }

  il::Function& function_;
  analysis::StackFrame frame_;
  il::ExprId spLeaf_;
  ValueSubst subst_;
  std::vector<std::pair<il::BlockId, il::OpId>> forwarded_;
  bool canonicalized_ = false;
};

class StackPropPass final : public pass::FunctionPass {
 public:
  StackPropPass()
      : FunctionPass([] {
          pass::PassInfo info;
          info.name = "stack-prop";
          info.level = il::Maturity::Ssa;
          info.produces = il::Maturity::Ssa;
          info.fixpoint = true;
          return info;
        }()) {}

  Result<bool> run(pass::Context& context) override {
    return StackProp(context.function()).run();
  }
};

}  // namespace

std::unique_ptr<pass::Pass> makeStackPropPass() {
  return std::make_unique<StackPropPass>();
}

}  // namespace xdec::passes
