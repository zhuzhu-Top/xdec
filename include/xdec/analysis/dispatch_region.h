// DispatchRegion: clustering many small dispatch sites into the one physical
// table they all read through.
//
// A flattening obfuscator does not always emit one N-way dispatch block that
// every state reaches (the shape analysis::matchDispatcherShape and
// structure.cpp's `tryDispatcherLoop` already recover). It can equally well
// spend hundreds of small, otherwise-unrelated two-way branches, each
// computing its own next-state pair and reading through the exact same
// physical jump table with the exact same out-of-range clamp -- the shape a
// heavily flattened real-world binary leaves once value-set analysis has
// already narrowed each individual branch down to just the two states its
// own predecessor path can reach (see docs/00-core-vs-plugin-prompt.md and
// eval/FINDINGS.md's libscplugin notes: ~700 physically identical dispatch
// epilogues, none of which individually looks like more than an `if`).
//
// No single site's own target list says the two are related -- only the
// table identity they both read through does. This header is that identity,
// gathered once per function so a caller (a future structurizer pass, an
// emit-quality metric, a diagnostic) can ask "how much of this function is
// one flattened state machine" without re-deriving table identity itself.
//
// Deliberately analysis-only: nothing here prints C, renames a variable, or
// changes control flow. Clustering is purely structural (table base/stride/
// width/anchor plus, when present, the clamp's bound/replacement constants)
// -- the same "answers, not guesses" discipline analysis::matchJumpTable and
// analysis::matchDispatchValues already hold to.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "xdec/analysis/dispatcher_shape.h"
#include "xdec/il/function.h"

namespace xdec::analysis {

/// A resolved jump-table's out-of-range guard: `bound < index ? replacement
/// : index` (see structure.cpp's collapseDispatchTree comment and
/// c_expr.cpp's Select case for where this shape used to be named in
/// emission -- clustering needs the same recognition, just to compare
/// constants across sites, never to change how it prints).
struct DispatchClamp {
  il::ExprId index;
  uint64_t bound = 0;
  uint64_t replacement = 0;
  bool isSigned = false;
};

/// `select` when it matches `DispatchClamp`'s exact shape -- `cmp.lt{s,u}
/// (bound, index) ? replacement : index`, with both `bound` and
/// `replacement` literal constants (an index clamp with a computed bound
/// only bounds this one branch, not a table every other site in the region
/// reads the same way, so it is not clustering evidence) at a 32- or 64-bit
/// width, matching the table-index convention `matchDispatchIndexClamp`
/// once used. Nullopt for every other select, including one that clamps but
/// not through a literal pair.
[[nodiscard]] std::optional<DispatchClamp> matchDispatchClamp(const il::Function& function,
                                                               il::ExprId select);

/// One resolved computed branch that reads through a jump table: what
/// analysis::matchJumpTable plus the branch's own resolved targets already
/// know about a single block, gathered so findDispatchRegions can cluster
/// many of these into the one physical table they share.
struct DispatchSite {
  il::BlockId dispatchBlock;
  /// The branch's own resolved targets, in `il::Function::targets`' own
  /// order -- not `matchDispatchValues`' sorted order, so a caller zipping
  /// the two together must sort as it does.
  std::vector<il::BlockId> targets;
  /// The table read's own index expression (matchJumpTable's `index`): the
  /// clamp select itself when this site guards one, the bare index
  /// otherwise.
  il::ExprId indexExpr;
  /// The state value that reaches each of `targets`, in the same order, when
  /// analysis::matchDispatchValues can reconstruct it from `indexExpr`'s own
  /// constant/select structure. Empty when it cannot -- the site is still a
  /// real member of the region, just one whose per-target values are not
  /// statically nameable this way.
  std::vector<uint64_t> caseValues;
};

/// The shared-tail shape voted across every site in a region's pooled
/// targets -- the region-level generalisation of analysis::DispatcherShape,
/// for the case where no single site has enough targets of its own to vote
/// with (each site here typically has exactly two) but many sites together
/// do. `merge` is the block most targets across the whole region fall into
/// before jumping on to `hub`.
struct DispatchRegionTail {
  il::BlockId merge;
  il::BlockId hub;
};

/// Every dispatch site sharing one physical jump table -- same base, stride,
/// entry width and offset signedness (see analysis::JumpTable) -- and, when
/// they clamp their index, the same bound/replacement pair. A data identity,
/// not a control-flow shape: two sites can belong to the same region while
/// looking nothing alike in the CFG, because what makes them one region is
/// which memory they both dispatch through, not how either one branches.
struct DispatchRegion {
  uint64_t tableBase = 0;
  uint32_t tableStride = 0;
  uint32_t tableEntryBits = 0;
  bool tableRelative = false;
  /// The anchor the region's *first* site adds its offsets to. Offset tables
  /// are anchored per site (typically at the reading dispatcher's own
  /// address), so this is a sample, not a property every site shares -- it is
  /// not part of what makes these sites one region.
  uint64_t tableAnchor = 0;
  bool tableSignedOffsets = false;
  /// Absent when no site in this region clamps its index at all -- a table
  /// read straight off an already-bounded value is still one region by
  /// table identity alone.
  std::optional<uint64_t> clampBound;
  std::optional<uint64_t> clampReplacement;
  std::vector<DispatchSite> sites;
  /// The region's own shared-tail vote (see DispatchRegionTail), when its
  /// pooled targets converge on one. Nullopt is the honest, common case for
  /// a region whose sites are otherwise-unrelated small decisions rather
  /// than one N-way switch's cases restoring a common set of live registers
  /// before looping back -- reported, never guessed into existence (see
  /// docs/00-core-vs-plugin-prompt.md).
  std::optional<DispatchRegionTail> sharedTail;
};

/// Scans every block ending in a resolved `IndirectBranch` whose target
/// expression matches analysis::matchJumpTable, and clusters them into
/// `DispatchRegion`s by table identity (see above). One region per distinct
/// table/clamp signature, in the order each signature is first seen; a
/// function with no such branch (including one where every indirect branch
/// is still unresolved) returns no regions at all -- there is nothing to
/// over-report on honest or not-yet-resolved code.
[[nodiscard]] std::vector<DispatchRegion> findDispatchRegions(const il::Function& function);

/// A block reached only from two or more of a region's own private handler
/// tails -- the many-hub generalisation of `DispatchRegionTail` for a region
/// whose sites converge on several different local tails instead of voting
/// one dominant one region-wide (see `DispatchRegion::sharedTail`'s own
/// nullopt case: a scatter-dispatcher's hundreds of two-way sites usually
/// have no single block most of them agree on, but many can still each
/// independently converge, a handful at a time, on a hub of their own).
/// `tails` -- always at least two, since a single feeding predecessor is not
/// a join at all, just an ordinary private handler one level up -- are each
/// some region site's own resolved target: `hub`'s sole predecessor from
/// that site, itself ending in one plain jump straight into `hub`.
struct DispatchJoin {
  il::BlockId hub;
  std::vector<il::BlockId> tails;
};

/// Finds every join hub in `region`. A candidate hub's vote is cast the same
/// way analysis::matchDispatcherShape casts one dispatch block's own vote --
/// a target counts as a tail when its sole predecessor is the site that
/// names it and it ends in one plain jump -- except cast once per target
/// across the whole region rather than once per dispatch block, so a hub fed
/// by different sites still counts. A hub qualifies only when *every one* of
/// its real predecessors turned out to be one of the tails counted this way
/// (never a mix of a counted tail and some unrelated block reaching the same
/// hub) and at least two did; there is no threshold to clear beyond that,
/// unlike the region-wide vote's 80% bar, because there is no larger pool of
/// "other candidates" a hub with two genuine private tails is competing
/// against.
[[nodiscard]] std::vector<DispatchJoin> findDispatchJoins(const il::Function& function,
                                                           const DispatchRegion& region);

/// Confirms `dispatch`'s own shared-tail shape from `region`'s pooled
/// evidence rather than from `dispatch`'s own target list alone --
/// analysis::matchDispatcherShape's single-block vote never even attempts a
/// site with fewer than three targets, but a flattening obfuscator's value-
/// set narrowing routinely leaves individual sites with exactly two live
/// targets while the physical table (and thus the region) they all read
/// through still has hundreds. `region.sharedTail`'s vote is evidence the
/// shape exists *somewhere* in the region; this still individually verifies
/// it for `dispatch` specifically -- every one of `targets` must itself be a
/// private handler (dispatch's sole predecessor) falling straight into
/// `region.sharedTail->merge` -- because a majority elsewhere is not
/// evidence about this one site if this site's own targets do not actually
/// reach that tail (a target that itself keeps dispatching, say, is not
/// "falls into the tail" no matter how the rest of the region votes).
/// Nullopt when `region` has no shared tail, or when `dispatch` is not
/// individually confirmed by it.
[[nodiscard]] std::optional<DispatcherShape> confirmDispatcherShapeFromRegion(
    const il::Function& function, il::BlockId dispatch, std::span<const il::BlockId> targets,
    const DispatchRegion& region);

/// The CFG shape a scatter-dispatcher's own sites take when `sharedTail` is
/// absent (docs/19-scatter-dispatch-target-shape.md): one site's handler
/// routinely does a couple of ops and then dispatches again through the
/// exact same table, so the region is not a flat pool of unrelated
/// decisions but a nested decision forest -- one binary decision per site,
/// chained through whichever arm keeps dispatching. `children[site]` is
/// every other site in the region that some target of `site` leads into
/// (see `buildDispatchNestGraph`'s own search radius); a site absent from
/// `children` is a leaf, its own handlers never re-entering the table.
/// `roots` are the sites no other site's own targets ever lead into --
/// where a reader walking the function from its entry would first meet
/// this region's own decision tree, of which there can be several,
/// independently reached, in one function. `maxDepth` is the longest chain
/// from any root, and `nestedSiteCount` is how many distinct sites appear
/// as *someone's* child -- how many of the region's sites are themselves
/// reached this way rather than only from the function's ordinary control
/// flow.
struct DispatchNestGraph {
  std::map<il::BlockId, std::vector<il::BlockId>> children;
  std::vector<il::BlockId> roots;
  std::size_t maxDepth = 0;
  std::size_t nestedSiteCount = 0;
};

/// Builds `region`'s own nest graph by walking, from each of `region`'s
/// sites' own targets, forward through the CFG for a bounded number of
/// hops looking for the next block that is itself one of `region`'s own
/// sites -- exactly what a chained scatter site's "keep dispatching" arm
/// looks like once its handler runs a short straight-line span and re-enters
/// the same table. A target that dead-ends (returns, or never reaches
/// another site within the search radius) contributes no edge; nothing here
/// is guessed when the walk comes up empty. Purely descriptive: never
/// claims a block, never changes what `il::Function` reports, and answers
/// only questions about `region`'s own sites, not the rest of the CFG.
[[nodiscard]] DispatchNestGraph buildDispatchNestGraph(const il::Function& function,
                                                        const DispatchRegion& region);

}  // namespace xdec::analysis
