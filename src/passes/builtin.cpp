// registerBuiltinPasses: the stock pipeline every front-end gets.
#include "xdec/passes/builtin.h"

#include "xdec/passes/apply_types.h"
#include "xdec/passes/cfg_finalize.h"
#include "xdec/passes/const_fold_memory.h"
#include "xdec/passes/fold_resolved_branch.h"
#include "xdec/passes/local_simplify.h"
#include "xdec/passes/recover_syscall.h"
#include "xdec/passes/recover_tailcall.h"
#include "xdec/passes/resolve_call.h"
#include "xdec/passes/resolve_indirect.h"
#include "xdec/passes/ssa_construct.h"
#include "xdec/passes/ssa_optimize.h"
#include "xdec/passes/stack_prop.h"
#include "xdec/passes/trampoline_fold.h"
#include "xdec/passes/vars.h"

namespace xdec::passes {

void registerBuiltinPasses(pass::Registry& registry) {
  // Registration order is the pipeline tie-breaker for passes at the same
  // maturity, so this list reads bottom-up in pipeline order anyway.
  (void)registry.add(makeLocalSimplifyPass());
  (void)registry.add(makeCfgFinalizePass());
  // Between the two: still Cfg maturity, still no phis to keep in step with
  // an edge retarget, and the last point before ssa-construct where eliding
  // a block is this cheap.
  (void)registry.add(makeTrampolineFoldPass());
  (void)registry.add(makeSsaConstructPass());
  // Before the optimiser, because that is the ordering that pays: the loads
  // this folds are the leaves SCCP and the algebra rules need literal before
  // an obfuscated expression tree can collapse at all.
  (void)registry.add(makeConstFoldMemoryPass());
  (void)registry.add(makeSsaOptimizePass());
  // After the optimiser: stack-prop wants folded addresses and entry leaves,
  // and same-level passes run in registration order.
  (void)registry.add(makeStackPropPass());
  // Before resolve-call, because the call this creates out of a tail-calling
  // indirect branch is one resolve-call can then prove direct or name as an
  // import, exactly like a `blr` the lifter produced.
  (void)registry.add(makeRecoverTailCallPass());
  // Still Ssa, and after every simplification: a call target only converges
  // once the arithmetic over it has collapsed, so this is the last place it can
  // run and the first place it can succeed.
  (void)registry.add(makeResolveCallPass());
  // Same reasoning for the syscall number, one register over: it is only a
  // constant after propagation, and trimming the argument list here means the
  // registers a syscall does not read stop looking like function arguments
  // before `vars` counts them.
  (void)registry.add(makeRecoverSyscallPass());
  // And the same again for ordinary calls, where the arity comes from an
  // imported prototype rather than the syscall table. After resolve-call,
  // because a call it proves direct is one this can look a symbol up for.
  (void)registry.add(makeApplyTypesPass());
  // Resolution consumes everything the Ssa-level work simplified.
  (void)registry.add(makeResolveIndirectPass());
  // After resolve-indirect, because only once a branch's targets are known
  // can this tell a genuinely computed one from a single-target one that no
  // longer needs to look computed at all.
  (void)registry.add(makeFoldResolvedBranchPass());
  // Then the calling convention, over resolved control flow: Optimized has no
  // pass of its own yet, so this one bridges Resolved straight to Vars.
  (void)registry.add(makeVarsPass());
}

}  // namespace xdec::passes
