// makeVarsPass: recovers call argument arity from the caller's side (see the
// header for the rule and what it rests on).
#include "xdec/passes/vars.h"

#include <format>
#include <vector>

#include "xdec/il/function.h"

namespace xdec::passes {

namespace {

/// Whether the expression in an argument slot is evidence that the caller put
/// something there.
///
/// Only Undef says no. Undef is not "a value we failed to track" — SSA
/// construction writes it exactly where nothing on any path defines the
/// register, which after a call is the ABI's own statement that the callee may
/// have left anything in it. Every other expression, entry values included, is
/// something that reached the register by an explicit route.
[[nodiscard]] bool isSetUp(const il::Function& function, il::ExprId slot) {
  return function.expr(slot).op != il::ExprOp::Undef;
}

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
    bool changed = false;
    for (const il::BlockId blockId : function.blockHandles()) {
      for (const il::OpId opId : function.block(blockId).ops) {
        if (function.op(opId).code == il::OpCode::Call) {
          changed |= recoverArity(function, opId);
        }
      }
    }
    return changed;
  }

 private:
  /// Trims one call's argument list to what the caller set up, and reports the
  /// gaps it could not trim.
  static bool recoverArity(il::Function& function, il::OpId opId) {
    const auto operands = function.operands(function.op(opId));
    if (operands.size() < 2) {
      return false;  // target only: nothing was ever attached
    }
    // Operand zero is the target, not an argument.
    const std::size_t attached = operands.size() - 1;
    std::size_t arity = 0;
    std::size_t gaps = 0;
    for (std::size_t index = 1; index <= attached; ++index) {
      if (isSetUp(function, operands[index])) {
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
