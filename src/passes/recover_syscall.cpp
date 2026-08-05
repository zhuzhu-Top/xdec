// makeRecoverSyscallPass: names the syscall behind an `svc` and trims its
// argument list to that syscall's arity (see the header for why each step is
// safe and what it does when the evidence runs out).
#include "xdec/passes/recover_syscall.h"

#include <format>
#include <vector>

#include "xdec/il/function.h"
#include "xdec/types/syscall_table.h"

namespace xdec::passes {

namespace {

class RecoverSyscall final : public pass::FunctionPass {
 public:
  RecoverSyscall()
      : FunctionPass([] {
          pass::PassInfo info;
          info.name = "recover-syscall";
          info.level = il::Maturity::Ssa;
          info.produces = il::Maturity::Ssa;
          // The whole design rests on the number already being folded: this
          // pass reads an operand, it does not search for a definition.
          info.requirements = {"ssa-optimize"};
          return info;
        }()) {}

  Result<bool> run(pass::Context& context) override {
    const types::SyscallTable* table = context.syscallTable();
    il::Function& function = context.function();
    const uint32_t intrinsicName = function.internName(kSyscallIntrinsic);

    bool changed = false;
    for (const il::BlockId blockId : function.blockHandles()) {
      for (const il::OpId opId : function.block(blockId).ops) {
        const il::Op& op = function.op(opId);
        if (op.code != il::OpCode::Intrinsic || op.payload != intrinsicName) {
          continue;
        }
        changed |= recoverOne(function, opId, table);
      }
    }
    return changed;
  }

 private:
  static bool recoverOne(il::Function& function, il::OpId opId,
                         const types::SyscallTable* table) {
    const auto operands = function.operands(function.op(opId));
    if (operands.size() <= kSyscallFirstArgOperand) {
      // Already trimmed to no arguments, or lifted by something that did not
      // attach the ABI. Either way there is nothing left to do, and saying so
      // is what keeps the pass idempotent under the fixpoint scheduler.
      return false;
    }

    uint64_t number = 0;
    // A zero-extended `mov w8, #nr` or a thunk's truncated parameter is still
    // the same syscall, so the number is read through those adjustments.
    if (!function.asConstantThroughCasts(operands[kSyscallNumberOperand], number)) {
      annotateOnce(function, opId,
                   "syscall number (x8) is not a constant here; arguments are the raw "
                   "x0-x5 registers");
      return false;
    }

    const types::SyscallInfo* info =
        table == nullptr ? nullptr : table->find(static_cast<uint32_t>(number));
    if (info == nullptr) {
      annotateOnce(function, opId,
                   table == nullptr
                       ? std::format("syscall {} (no syscall table loaded)", number)
                       : std::format("syscall {} is not in the '{}' table", number,
                                     table->arch()));
      return false;
    }

    annotateOnce(function, opId, std::format("syscall {} ({})", info->name, number));

    const std::size_t keep = kSyscallFirstArgOperand + info->argCount;
    if (operands.size() <= keep) {
      return false;
    }
    const std::vector<il::ExprId> trimmed(
        operands.begin(), operands.begin() + static_cast<std::ptrdiff_t>(keep));
    function.setOperands(opId, trimmed);
    return true;
  }

  /// Notes accumulate across passes by design, so a fixpoint re-run must not
  /// append the same sentence again.
  static void annotateOnce(il::Function& function, il::OpId opId, std::string note) {
    const std::string_view existing = function.noteOn(opId);
    if (existing.find(note) != std::string_view::npos) {
      return;
    }
    function.appendNote(opId, note);
  }
};

}  // namespace

std::unique_ptr<pass::Pass> makeRecoverSyscallPass() {
  return std::make_unique<RecoverSyscall>();
}

}  // namespace xdec::passes
