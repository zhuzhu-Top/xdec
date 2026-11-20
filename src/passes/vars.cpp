// makeVarsPass: recovers call argument arity from the caller's side (see the
// header for the rule and what it rests on).
#include "xdec/passes/vars.h"

#include <format>
#include <optional>
#include <set>
#include <unordered_set>
#include <vector>

#include "xdec/analysis/import_callee.h"
#include "xdec/analysis/stack_escape.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/il/function.h"
#include "xdec/types/binder.h"
#include "xdec/types/database.h"

#include "call_names.h"

namespace xdec::passes {

namespace {

/// Operand zero of a Call (and of an unresolved IndirectBranch) is its target;
/// the rest are the argument registers' versions SSA construction attached.
constexpr std::size_t kFirstArgOperand = 1;

/// Every (op, operand index) whose argument slot still holds its own register's
/// entry value, untouched: no instruction in this function wrote it and no
/// call on any path before this one could have destroyed it.
using SlotSet = std::set<std::pair<uint32_t, std::size_t>>;

class Vars final : public pass::FunctionPass {
 public:
  Vars()
      : FunctionPass([] {
          pass::PassInfo info;
          info.name = "vars";
          info.level = il::Maturity::Resolved;
          info.produces = il::Maturity::Vars;
          // Not ssa-construct, though that is what attaches the operands: what
          // this pass reads is which slots survived optimisation as real
          // values, and a slot can only be trusted to be Undef once constant
          // and copy propagation have finished putting values where they go.
          info.requirements = {"ssa-optimize"};
          return info;
        }()) {}

  Result<bool> run(pass::Context& context) override {
    il::Function& function = context.function();
    const analysis::CallNameSources names = callNameSources(context);
    const SlotSet untouched = untouchedSlots(function, names);
    const std::unordered_set<uint32_t> readElsewhere = entryRegsReadOutside(function, untouched);

    std::optional<types::TypeBinder> binder;
    const types::TypeEntry* self = nullptr;
    if (const types::TypeDatabase* database = context.typeDatabase()) {
      binder.emplace(*database, [&context](uint64_t va) {
        const pass::SymbolName symbol = context.nameAt(va);
        return types::BoundName{symbol.name, symbol.isFunction};
      });
      self = binder->prototypeAt(function.block(function.entryBlock()).va);
    }

    bool changed = false;
    for (const il::BlockId blockId : function.blockHandles()) {
      for (const il::OpId opId : function.block(blockId).ops) {
        if (function.op(opId).code != il::OpCode::Call) {
          continue;
        }
        std::size_t declared = 0;
        if (binder) {
          const auto operands = function.operands(function.op(opId));
          if (const types::TypeEntry* callee = analysis::resolveCallee(
                  function, *binder, self, context.memoryFacts(), operands[0])) {
            declared = callee->params.size();
          }
        }
        changed |= recoverArity(function, opId, declared, untouched, readElsewhere);
      }
    }
    changed |= attachOutgoingStackArgs(function, context.memoryFacts(),
                                       binder ? &*binder : nullptr, self);
    return changed;
  }

 private:
  [[nodiscard]] static bool isCalleeSavedEntrySpill(const il::Function& function,
                                                    il::ExprId value) {
    const il::Expr& expr = function.expr(value);
    if (expr.op != il::ExprOp::EntryReg) {
      return false;
    }
    const il::RegId reg{static_cast<uint32_t>(expr.immediate)};
    if (!function.registers().contains(reg)) {
      return false;
    }
    const std::string_view name = function.registers().nameOf(reg);
    return name.size() == 3 && name[0] == 'x' &&
           ((name[1] == '1' && name[2] == '9') ||
            (name[1] == '2' && name[2] >= '0' && name[2] <= '9') ||
            (name[1] == '3' && name[2] == '0'));
  }

  /// Attaches contiguous outgoing stack arguments `[sp+0], [sp+8], ...` when
  /// a call has all 8 register slots active and unread, unescaped stores sit
  /// immediately at the call's SP offset in the same basic block.
  static bool attachOutgoingStackArgs(il::Function& function, const MemoryFacts& memory,
                                      const types::TypeBinder* binder,
                                      const types::TypeEntry* self) {
    const analysis::StackFrame frame = analysis::StackFrame::compute(function);
    std::unordered_set<int64_t> readDeltas;
    for (const il::BlockId blockId : function.blockHandles()) {
      for (const il::OpId opId : function.block(blockId).ops) {
        const il::Op& op = function.op(opId);
        if (op.code != il::OpCode::Load) {
          continue;
        }
        const auto ops = function.operands(op);
        if (!ops.empty()) {
          const analysis::AddressInfo info = frame.classify(ops[0]);
          if (info.kind == analysis::AddressKind::StackSlot) {
            readDeltas.insert(info.delta);
          }
        }
      }
    }
    const analysis::StackEscapeMap escapes = analysis::StackEscapeMap::compute(function, frame);

    bool changed = false;
    for (const il::BlockId blockId : function.blockHandles()) {
      std::unordered_map<int64_t, il::ExprId> recentStores;
      for (const il::OpId opId : function.block(blockId).ops) {
        const il::Op& op = function.op(opId);
        if (op.code == il::OpCode::Store) {
          const auto ops = function.operands(op);
          if (ops.size() >= 2) {
            const analysis::AddressInfo info = frame.classify(ops[0]);
            if (info.kind == analysis::AddressKind::StackSlot) {
              if (!isCalleeSavedEntrySpill(function, ops[1]) &&
                  !readDeltas.contains(info.delta) && !escapes.isEscaped(info.delta)) {
                recentStores[info.delta] = ops[1];
              } else {
                recentStores.erase(info.delta);
              }
            }
          }
          continue;
        }
        if (op.code == il::OpCode::Intrinsic || op.code == il::OpCode::Unimplemented) {
          recentStores.clear();
          continue;
        }
        if (op.code != il::OpCode::Call) {
          continue;
        }
        const std::optional<int64_t> spDelta = function.callSpDelta(opId);
        const auto operands = function.operands(op);
        if (spDelta.has_value() && operands.size() == 9) {
          bool allRegsSet = true;
          for (std::size_t i = kFirstArgOperand; i < operands.size(); ++i) {
            if (function.expr(operands[i]).op == il::ExprOp::Undef) {
              allRegsSet = false;
              break;
            }
          }
          const types::TypeEntry* callee =
              binder != nullptr
                  ? analysis::resolveCallee(function, *binder, self, memory, operands[0])
                  : nullptr;
          const bool allowedByType =
              callee == nullptr || callee->variadic || callee->params.size() > 8;
          if (allRegsSet && allowedByType && recentStores.contains(*spDelta)) {
            std::vector<il::ExprId> extended(operands.begin(), operands.end());
            for (int64_t offset = 0; offset < 256; offset += 8) {
              const auto it = recentStores.find(*spDelta + offset);
              if (it == recentStores.end()) {
                break;
              }
              extended.push_back(it->second);
            }
            if (extended.size() > operands.size()) {
              function.setOperands(opId, extended);
              changed = true;
            }
          }
        }
        recentStores.clear();
      }
    }
    return changed;
  }
  /// Whether a call can destroy the argument registers, which is every call
  /// but the helpers analysis/call_abi.h lists.
  static bool clobbersArguments(const il::Function& function, const il::Op& call,
                                const analysis::CallNameSources& names) {
    const auto operands = function.operands(call);
    if (operands.empty()) {
      return true;
    }
    const std::optional<std::string> name =
        analysis::callTargetName(function, operands[0], names);
    return !name.has_value() || !analysis::isRegisterPreservingHelper(*name);
  }

  /// A slot holding its own register's entry value is ambiguous only where
  /// nothing could have replaced that value first. After a clobbering call the
  /// register is dead by the ABI, so the same entry value arriving there was
  /// saved and put back on purpose: that is a set-up argument.
  static SlotSet untouchedSlots(const il::Function& function,
                                const analysis::CallNameSources& names) {
    std::set<il::BlockId> callBlocks;
    for (const il::BlockId blockId : function.blockHandles()) {
      for (const il::OpId opId : function.block(blockId).ops) {
        const il::Op& op = function.op(opId);
        if (op.code == il::OpCode::Call && clobbersArguments(function, op, names)) {
          callBlocks.insert(blockId);
          break;
        }
      }
    }
    // Blocks some clobbering call reaches along at least one edge. A call in a
    // loop reaches its own block, which is right: its second iteration runs
    // after its first.
    std::set<il::BlockId> afterCall;
    std::vector<il::BlockId> worklist;
    for (const il::BlockId blockId : callBlocks) {
      for (const il::BlockId succ : function.block(blockId).successors) {
        if (afterCall.insert(succ).second) {
          worklist.push_back(succ);
        }
      }
    }
    while (!worklist.empty()) {
      const il::BlockId blockId = worklist.back();
      worklist.pop_back();
      for (const il::BlockId succ : function.block(blockId).successors) {
        if (afterCall.insert(succ).second) {
          worklist.push_back(succ);
        }
      }
    }

    SlotSet out;
    for (const il::BlockId blockId : function.blockHandles()) {
      bool preceded = afterCall.contains(blockId);
      for (const il::OpId opId : function.block(blockId).ops) {
        const il::Op& op = function.op(opId);
        const bool isCall = op.code == il::OpCode::Call;
        if (!preceded && (isCall || op.code == il::OpCode::IndirectBranch)) {
          const auto operands = function.operands(op);
          for (std::size_t index = kFirstArgOperand; index < operands.size(); ++index) {
            const il::Expr& expr = function.expr(operands[index]);
            if (expr.op != il::ExprOp::EntryReg) {
              continue;
            }
            const il::RegId root{static_cast<uint32_t>(expr.immediate)};
            if (analysis::calleeArgumentIndex(function, root) ==
                static_cast<int>(index - kFirstArgOperand)) {
              out.emplace(opId.index(), index);
            }
          }
        }
        if (isCall && clobbersArguments(function, op, names)) {
          preceded = true;
        }
      }
    }
    return out;
  }

  /// The entry registers this function reads anywhere other than an untouched
  /// argument slot: its own parameters, by the only evidence the body has.
  static std::unordered_set<uint32_t> entryRegsReadOutside(const il::Function& function,
                                                           const SlotSet& untouched) {
    std::unordered_set<uint32_t> roots;
    std::unordered_set<uint32_t> seen;
    std::vector<il::ExprId> stack;
    for (const il::BlockId blockId : function.blockHandles()) {
      for (const il::OpId opId : function.block(blockId).ops) {
        const auto operands = function.operands(function.op(opId));
        for (std::size_t index = 0; index < operands.size(); ++index) {
          if (!untouched.contains({opId.index(), index})) {
            stack.push_back(operands[index]);
          }
        }
      }
    }
    while (!stack.empty()) {
      const il::ExprId id = stack.back();
      stack.pop_back();
      if (!id.valid() || !seen.insert(id.index()).second) {
        continue;
      }
      const il::Expr& expr = function.expr(id);
      if (expr.op == il::ExprOp::EntryReg) {
        roots.insert(static_cast<uint32_t>(expr.immediate));
        continue;
      }
      for (unsigned index = 0; index < expr.operandCount; ++index) {
        stack.push_back(expr.operand(index));
      }
    }
    return roots;
  }

  /// Whether the expression in an argument slot is evidence that the caller put
  /// something there.
  ///
  /// Undef says no. Undef is not "a value we failed to track" — SSA
  /// construction writes it exactly where nothing on any path defines the
  /// register, which after a call is the ABI's own statement that the callee may
  /// have left anything in it.
  ///
  /// An untouched slot says no too, unless the register is one of this
  /// function's own parameters. Before any call, x7 still holding whatever this
  /// function was entered with is what every call in a two-argument function
  /// sees; counting it would give the callee an eighth argument, and then give
  /// this function one as well, since the slot is also a read of x7.
  ///
  /// A prototype's declared parameters outrank that: forwarding one's own
  /// arguments untouched is exactly what a header describes a call doing.
  static bool isSetUp(const il::Function& function, il::OpId opId, std::size_t index,
                      il::ExprId slot, std::size_t declared, const SlotSet& untouched,
                      const std::unordered_set<uint32_t>& readElsewhere) {
    const il::Expr& expr = function.expr(slot);
    if (expr.op == il::ExprOp::Undef) {
      return false;
    }
    if (index - kFirstArgOperand >= declared) {
      if (untouched.contains({opId.index(), index})) {
        return readElsewhere.contains(static_cast<uint32_t>(expr.immediate));
      }
      // `mov x3, x0; ...; br x3`: the register holding the call's own target
      // is where the branch reads it from, not something passed to it.
      if (slot == function.operands(function.op(opId))[0]) {
        return false;
      }
    }
    return true;
  }

  /// Trims one call's argument list to what the caller set up, and reports the
  /// gaps it could not trim.
  static bool recoverArity(il::Function& function, il::OpId opId, std::size_t declared,
                           const SlotSet& untouched,
                           const std::unordered_set<uint32_t>& readElsewhere) {
    const auto operands = function.operands(function.op(opId));
    if (operands.size() <= kFirstArgOperand) {
      return false;  // target only: nothing was ever attached
    }
    const std::size_t attached = operands.size() - 1;
    // Untouched slots are only told apart from forwarded ones where the caller
    // is visibly building an argument list of its own: above the last slot it
    // wrote. A call that writes none is a pure forwarder as far as this side
    // can see (`(*env)->FindClass(env, name)`), and cutting it would lose
    // arguments the callee really reads.
    std::size_t lastWritten = 0;
    for (std::size_t index = kFirstArgOperand; index <= attached; ++index) {
      if (!untouched.contains({opId.index(), index}) &&
          isSetUp(function, opId, index, operands[index], declared, untouched, readElsewhere)) {
        lastWritten = index;
      }
    }
    const SlotSet noUntouched;
    const SlotSet& ambiguous = lastWritten == 0 ? noUntouched : untouched;
    std::size_t arity = 0;
    std::size_t gaps = 0;
    for (std::size_t index = kFirstArgOperand; index <= attached; ++index) {
      if (isSetUp(function, opId, index, operands[index], declared, ambiguous, readElsewhere)) {
        // Every unset slot before this one is a gap, now that something after
        // it turns out to be an argument.
        gaps += index - 1 - arity;
        arity = index;
      }
    }

    if (gaps > 0) {
      // The count, not the positions: which slot is which is already visible in
      // the emitted argument list, and a reader who cares can see it there.
      function.appendNote(
          opId, std::format("{} argument slot(s) hold values this function never wrote: "
                            "either the callee reads a stale register or a definition was "
                            "lost here",
                            gaps));
    }
    if (arity == attached) {
      return false;
    }
    const std::vector<il::ExprId> trimmed(
        operands.begin(),
        operands.begin() + static_cast<std::ptrdiff_t>(arity + 1));
    function.setOperands(opId, trimmed);
    return true;
  }
};

}  // namespace

std::unique_ptr<pass::Pass> makeVarsPass() { return std::make_unique<Vars>(); }

}  // namespace xdec::passes
