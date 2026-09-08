// makeAnnotateStackCanaryPass (see the header).
#include "xdec/passes/annotate_stack_canary.h"

#include <format>

#include "xdec/analysis/stack_canary.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/il/function.h"

namespace xdec::passes {

namespace {

class AnnotateStackCanary final : public pass::FunctionPass {
 public:
  AnnotateStackCanary()
      : FunctionPass([] {
          pass::PassInfo info;
          info.name = "annotate-stack-canary";
          info.level = il::Maturity::Vars;
          info.produces = il::Maturity::Vars;
          info.requirements = {"vars"};
          return info;
        }()) {}

  Result<bool> run(pass::Context& context) override {
    il::Function& function = context.function();
    const analysis::StackFrame frame = analysis::StackFrame::compute(function);
    for (const analysis::StackCanarySave& save : analysis::findStackCanarySaves(function, frame)) {
      annotateOnce(function, save.save,
                   std::format("stack canary saved from 0x{:x}; re-read and compared against "
                               "this value at 0x{:x} before returning",
                               save.guardAddress, function.op(save.check).va));
    }
    // Purely a note: nothing here ever changes the IL, so the fixpoint
    // scheduler never needs to see this pass as having "changed" anything
    // (recover-syscall's own annotateOnce follows the same convention).
    return false;
  }

 private:
  static void annotateOnce(il::Function& function, il::OpId opId, std::string note) {
    if (function.noteOn(opId).find(note) != std::string_view::npos) {
      return;
    }
    function.appendNote(opId, note);
  }
};

}  // namespace

std::unique_ptr<pass::Pass> makeAnnotateStackCanaryPass() {
  return std::make_unique<AnnotateStackCanary>();
}

}  // namespace xdec::passes
