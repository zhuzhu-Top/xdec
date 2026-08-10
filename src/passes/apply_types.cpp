// makeApplyTypesPass: trims call argument lists to what an imported header
// says the callee takes (see the header for why arity is the one part of type
// import that cannot wait for the emitter).
#include "xdec/passes/apply_types.h"

#include <format>
#include <string>
#include <vector>

#include "xdec/analysis/import_callee.h"
#include "xdec/il/function.h"
#include "xdec/types/binder.h"
#include "xdec/types/database.h"

namespace xdec::passes {

namespace {

/// Operand zero of a Call is its target; the rest are the calling convention's
/// argument registers (see the Call case in il/verify.cpp).
constexpr std::size_t kCallTargetOperand = 0;
constexpr std::size_t kCallFirstArgOperand = 1;

class ApplyTypes final : public pass::FunctionPass {
 public:
  ApplyTypes()
      : FunctionPass([] {
          pass::PassInfo info;
          info.name = "apply-types";
          info.level = il::Maturity::Ssa;
          info.produces = il::Maturity::Ssa;
          // A direct call is only a constant target once propagation has run,
          // and resolve-call is what turns a proven indirect call into one, so
          // this runs after both and sees the most calls it can.
          info.requirements = {"ssa-optimize", "resolve-call"};
          return info;
        }()) {}

  Result<bool> run(pass::Context& context) override {
    const types::TypeDatabase* database = context.typeDatabase();
    if (database == nullptr) {
      return false;  // no header imported: the normal case
    }

    il::Function& function = context.function();
    const types::TypeBinder binder(*database, [&context](uint64_t va) {
      const pass::SymbolName symbol = context.nameAt(va);
      return types::BoundName{symbol.name, symbol.isFunction};
    });
    // This function's own prototype types its parameters, which is what makes
    // a call *through* one of them describable.
    const types::TypeEntry* self =
        binder.prototypeAt(function.block(function.entryBlock()).va);

    bool changed = false;
    for (const il::BlockId blockId : function.blockHandles()) {
      for (const il::OpId opId : function.block(blockId).ops) {
        if (function.op(opId).code != il::OpCode::Call) {
          continue;
        }
        changed |= applyOne(function, binder, self, context.memoryFacts(), opId);
      }
    }
    return changed;
  }

 private:
  static bool applyOne(il::Function& function, const types::TypeBinder& binder,
                       const types::TypeEntry* self, const MemoryFacts& memory, il::OpId opId) {
    const auto operands = function.operands(function.op(opId));
    if (operands.size() <= kCallFirstArgOperand) {
      return false;  // already trimmed, or lifted without the convention
    }

    const types::TypeEntry* callee =
        analysis::resolveCallee(function, binder, self, memory, operands[kCallTargetOperand]);
    if (callee == nullptr || callee->variadic) {
      return false;
    }

    const std::size_t keep = kCallFirstArgOperand + callee->params.size();
    if (operands.size() <= keep) {
      // The header declares at least as many parameters as the convention
      // attached. Nothing to remove, and nothing to add either -- see the
      // header on why a call is never extended.
      return false;
    }
    annotateOnce(function, opId,
                 std::format("the imported prototype takes {} argument(s); {} "
                             "further register(s) dropped",
                             callee->params.size(), operands.size() - keep));
    function.setOperands(opId,
                         std::vector<il::ExprId>(
                             operands.begin(),
                             operands.begin() + static_cast<std::ptrdiff_t>(keep)));
    return true;
  }

  /// Notes accumulate across passes, so a fixpoint re-run must not repeat one.
  static void annotateOnce(il::Function& function, il::OpId opId, std::string note) {
    if (function.noteOn(opId).find(note) != std::string_view::npos) {
      return;
    }
    function.appendNote(opId, note);
  }
};

}  // namespace

std::unique_ptr<pass::Pass> makeApplyTypesPass() {
  return std::make_unique<ApplyTypes>();
}

}  // namespace xdec::passes
