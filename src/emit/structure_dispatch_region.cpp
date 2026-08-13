// J2 (docs/architecture-optimization-eval-prompt.md §3 Phase 3): flattening
// a nested dispatch tree within one analysis::DispatchRegion into fewer,
// wider switches.
//
// J1 already keeps a region member's own table-mode `switch` instead of
// collapsing it to `if`/`else` (see structure.cpp's `isMemberOfLargeDispatch
// Region`); when one site's own case target is itself another resolved
// dispatch reached privately, `claimCaseBody`'s ordinary recursion already
// prints that as a second, nested `switch` right inside the first one's
// case body -- correct, but exactly the "234 嵌套 2-case switch 树" shape
// the plan's own diagnosis names as the real cost. Two of those nested
// switches can be printed as one flat `switch` statement when -- and only
// when -- they are provably testing the exact same already-evaluated value:
// `Stmt::cond` for both is the identical `il::ExprId`, not merely a
// structurally similar re-computation. That equality is what makes the
// splice sound rather than a guess: a flattening dispatcher's own state
// handlers routinely reassign the state variable and dispatch on the *new*
// value one level down, and folding a re-read like that into one `switch`
// evaluated exactly once would print case labels for values the outer
// expression never actually takes at that point -- a wrong answer, not a
// legible one. The exact-`ExprId` requirement rules that shape out
// entirely: it only ever fires when nothing was reassigned in between (the
// same value-narrowing IL construction analysis::DispatchRegion itself is
// built to recognise, just tested more than once).
#include "structurizer.h"

#include <algorithm>

namespace xdec::emit {

std::size_t Structurizer::collapseRegionDispatchTree(Stmt& stmt,
                                                      const analysis::DispatchRegion& region) {
  std::size_t absorbed = 0;
  std::size_t index = 0;
  while (index < stmt.caseBodies.size()) {
    StmtPtr body = std::move(stmt.caseBodies[index]);
    // The exact shape switchFor's IndirectBranch case in emitRegion leaves
    // behind when a case's own handler is itself another resolved dispatch
    // reached privately: the handler's own block, then that block's switch.
    // Anything else claimed this case (real handler code before or after
    // the nested dispatch, a shared-tail Break, ...) is not this shape, and
    // is left exactly as claimCaseBody already built it.
    if (!body || body->kind != StmtKind::Sequence || body->items.size() != 2 ||
        body->items[0]->kind != StmtKind::Block || body->items[1]->kind != StmtKind::Switch) {
      stmt.caseBodies[index] = std::move(body);
      ++index;
      continue;
    }
    Stmt& inner = *body->items[1];
    // The soundness condition (see this file's own comment): the inner
    // switch must read the identical already-evaluated expression, and must
    // not itself have claimed a shared-tail epilogue of its own -- absorbing
    // its cases without also carrying that epilogue along would orphan
    // whichever of them still end in a `Break` meant for it.
    const bool sameDiscriminant = inner.tableMode && stmt.tableMode && inner.cond == stmt.cond;
    const bool sameCaseShape = stmt.caseValues.empty() == inner.caseValues.empty();
    const il::BlockId target = body->items[0]->block;
    const bool memberOfRegion =
        std::any_of(region.sites.begin(), region.sites.end(),
                    [&](const analysis::DispatchSite& site) { return site.dispatchBlock == target; });
    if (!sameDiscriminant || !sameCaseShape || inner.epilogue || !memberOfRegion) {
      stmt.caseBodies[index] = std::move(body);
      ++index;
      continue;
    }
    const std::size_t innerCount = inner.cases.size();
    stmt.cases[index] = inner.cases[0];
    stmt.casePreds[index] = inner.casePreds.empty() ? target : inner.casePreds[0];
    stmt.caseBodies[index] = std::move(inner.caseBodies[0]);
    if (!stmt.caseValues.empty()) {
      stmt.caseValues[index] = inner.caseValues[0];
    }
    for (std::size_t offset = 1; offset < innerCount; ++offset) {
      const std::size_t at = index + offset;
      stmt.cases.insert(stmt.cases.begin() + static_cast<std::ptrdiff_t>(at), inner.cases[offset]);
      stmt.casePreds.insert(stmt.casePreds.begin() + static_cast<std::ptrdiff_t>(at),
                            inner.casePreds.size() > offset ? inner.casePreds[offset] : target);
      stmt.caseBodies.insert(stmt.caseBodies.begin() + static_cast<std::ptrdiff_t>(at),
                             std::move(inner.caseBodies[offset]));
      if (!stmt.caseValues.empty()) {
        stmt.caseValues.insert(stmt.caseValues.begin() + static_cast<std::ptrdiff_t>(at),
                               inner.caseValues[offset]);
      }
    }
    absorbed += innerCount;
    // Recheck the slots just spliced in (not `++index`): a chain three or
    // more sites deep must flatten in one call, not one switchFor call per
    // level.
  }
  return absorbed;
}

}  // namespace xdec::emit
