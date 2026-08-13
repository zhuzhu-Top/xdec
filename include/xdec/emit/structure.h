// Control-flow structuring: IL CFG to a statement tree.
//
// The matcher is deliberately conservative pattern structuring, not a full
// region analysis (that is the DREAM-class algorithm, planned for a later
// maturity). Three patterns inline cleanly:
//   * if / if-else diamonds    — a conditional whose arms reconverge at its
//                                immediate post-dominator, each arm linear;
//   * while / do-while loops   — a natural loop whose body walks linearly;
//   * switch                   — a resolved indirect branch, either over a
//                                recognized jump-table index or as a target
//                                compare chain.
// Everything else degrades to labeled blocks and explicit gotos — the same
// answer every production decompiler gives for irreducible flow, never a
// guess. Every block is emitted exactly once; a block inlined into a
// structured region is verified to have no predecessors outside that region.
#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "xdec/analysis/dominators.h"
#include "xdec/analysis/indexed_transform_loop.h"
#include "xdec/analysis/live_register_frame.h"
#include "xdec/analysis/loops.h"
#include "xdec/il/function.h"

namespace xdec::emit {

/// One pattern `Structurizer::emitRegion`'s `CondBranch` site tries, and the
/// fixed priority it tries them in -- doc-only metadata naming the same
/// order the if-chain in structure.cpp's `emitRegion` already encodes, not a
/// dispatch table it drives: each pattern's actual match logic keeps its own
/// method (`tryDiamond`, `tryGuardCascade`, ...), its own signature, and its
/// own claim/rollback discipline, since `tryDispatchTree` mutates the
/// sequence directly while every other pattern returns a `StmtPtr` on
/// success -- unifying those under one function-pointer signature would cost
/// more than the five sites here are worth (see the architecture plan's own
/// note on why a full Structurizer registry waits for pattern #6+). What
/// this table buys instead: one place that states the priority order in
/// code, checked by a test, so a future reorder cannot silently drift from
/// what the comments describe -- and the stable names `StructuredFunction::
/// matchedPatterns` records a claim under.
struct PatternAttempt {
  std::string_view name;
  int priority;  // lower tries first, matching emitRegion's own if-chain
};

/// The `CondBranch`-site patterns `emitRegion` tries, in the exact order it
/// tries them. A goto chain -- emitRegion's unconditional fallback once every
/// pattern here has declined -- always succeeds, so it is not a "pattern
/// attempt" and is not listed; see `StructuredFunction::matchedPatterns` for
/// what actually claimed each site, goto chains included (recorded as
/// `"goto-chain"` there even though it has no entry of its own here).
inline constexpr std::array<PatternAttempt, 4> kCondBranchPatterns{{
    {"diamond", 0},
    {"guard-cascade", 1},
    {"dispatch-tree", 2},
    {"one-sided", 3},
}};

/// J2's own reserved slot (docs/architecture-optimization-eval-prompt.md §3
/// Phase 3): a *region*-level pass -- one that claims an entire
/// analysis::DispatchRegion's worth of `IndirectBranch` sites into a single
/// mega-switch (analysis::DispatchRegion's own many small two-way sites,
/// collapsed rather than left as a nested tree) -- is not one of
/// `kCondBranchPatterns` above, which is a `CondBranch`-site table only, and
/// runs at a different point in `Structurizer::run()` (once per qualifying
/// region, ahead of `emitRegion`'s per-block walk, not once per site inside
/// it). Named and frozen here as its own table, the same doc-only,
/// checked-by-a-test discipline `kCondBranchPatterns` already holds to,
/// purely so the interface exists before the implementation does: nothing
/// in `Structurizer` reads this yet, and `StructureOptions::regionStructuring`
/// (default off) is what Phase 3's actual pass gates on once it exists.
struct RegionPatternAttempt {
  std::string_view name;
  int priority;
};

inline constexpr std::array<RegionPatternAttempt, 1> kRegionPatterns{{
    {"region-switch", 0},
}};

enum class StmtKind : uint8_t {
  Sequence,  // items, in order
  Block,     // one IL basic block's straight-line ops; `block` set
  If,        // cond; thenArm, optional elseArm; invertCond negates
  DoWhile,   // body; cond
  While,     // cond; body
  Switch,    // index expr (table mode) or target expr (chain mode); cases
  Goto,      // `block` is the target
  Continue,  // back edge to the enclosing loop's header, which is `block`
  Break,     // exits the nearest switch or loop. A dispatcher case's own back
             // edge to the switch's shared tail (see Stmt::epilogue) prints
             // this way instead of a `goto` to it, `block` left invalid. A
             // loop body's own edge to the loop's exit block does the same
             // once that block is proven to be exactly where control already
             // falls once the loop is done (see Structurizer::tryLoop) --
             // there `block` is kept (as a plain `Goto` leaving it would be)
             // solely so the exit's edge copies still print here, ahead of
             // the `break;` they belong to.
};

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

struct Stmt {
  StmtKind kind = StmtKind::Sequence;
  il::BlockId block{};
  /// While: an invalid condition is `while (true)` — a loop nothing leaves by
  /// failing a test, only by returning or jumping out of it.
  il::ExprId cond{};
  bool invertCond = false;
  /// Switch in table mode: `cond` is the index and cases[i] is case i's
  /// target. In chain mode `cond` is the raw target expression and each case
  /// compares against its block's address.
  bool tableMode = false;
  std::vector<StmtPtr> items;   // Sequence
  StmtPtr thenArm;              // If
  StmtPtr elseArm;              // If
  StmtPtr body;                 // loops
  std::vector<il::BlockId> cases;  // Switch
  /// Switch: the handler for each case, written inside the case, when the handler
  /// belongs to this switch alone. A null entry means the case jumps to the
  /// handler's label instead, which is all that can be said when other paths
  /// reach it too. Parallel to `cases` when non-empty.
  std::vector<StmtPtr> caseBodies;  // Switch
  /// Compare-chain switches carry their case constants; table switches use
  /// the case index instead and leave this empty.
  std::vector<uint64_t> caseValues;  // Switch, chain mode
  /// The dispatcher block each case edge leaves from (chain mode, parallel
  /// to cases): phi edge assignments print from there.
  std::vector<il::BlockId> casePreds;  // Switch, chain mode
  /// The chain's fall-through printed as a `default:` arm; invalid when the
  /// switch is exhaustive (table mode) or flows on.
  il::BlockId defaultCase{};  // Switch, chain mode
  /// The dispatcher block the default edge leaves from, and the default's
  /// handler when this switch alone reaches it — `casePreds`/`caseBodies` for
  /// the default arm.
  il::BlockId defaultPred{};  // Switch, chain mode
  StmtPtr defaultBody;        // Switch, chain mode
  /// Switch: the dispatcher shape's shared tail (see
  /// analysis::DispatcherShape), structured once and printed right after the
  /// switch's closing brace instead of once per case. A case that reaches it
  /// ends with a `Break` rather than a `goto`/`return`. Null when no such
  /// shape was found for this switch.
  StmtPtr epilogue;          // Switch
  /// The block `epilogue` was built from; invalid when `epilogue` is null.
  il::BlockId mergeBlock{};  // Switch
  /// The shadow/live register pairing the dispatcher's handlers save into and
  /// restore out of on their way to `epilogue` (see
  /// analysis::LiveRegisterFrame). Null whenever `epilogue` is, or the shape
  /// was found but this exact save/restore protocol was not -- emission then
  /// prints every case's edge copies in full, same as any ordinary switch.
  std::optional<analysis::LiveRegisterFrame> frame;  // Switch
  /// While/DoWhile: the load/transform/store shape found in this loop's own
  /// body (see analysis::matchIndexedTransformLoop). Null when the loop was
  /// not this shape at all -- most loops -- printing nothing extra for it.
  std::optional<analysis::IndexedTransformLoop> transform;  // While, DoWhile

  static StmtPtr make(StmtKind kind) {
    auto stmt = std::make_unique<Stmt>();
    stmt->kind = kind;
    return stmt;
  }
};

struct StructuredFunction {
  StmtPtr root;
  /// Blocks that appear with a label (goto targets), in emission order.
  [[nodiscard]] bool isLabeled(il::BlockId block) const;

  std::vector<il::BlockId> labeled;
  /// The name (see `kCondBranchPatterns`, plus `"goto-chain"` for the
  /// unconditional fallback) of whichever pattern claimed each `CondBranch`
  /// site `emitRegion` visited, in claim order. Purely observational --
  /// nothing downstream of structuring reads this or depends on what is in
  /// it -- kept for tests and metrics that want to know which patterns a
  /// function's control flow actually exercised without re-walking the
  /// `Stmt` tree to guess from its shape.
  std::vector<std::string_view> matchedPatterns;
};

/// Track B / J1 (docs/18-architecture-optimization-plan.md §5.2): `switchFor`'s
/// two-live-target table dispatch normally collapses straight to `if (cond)
/// A else B` (see analysis::matchDispatchValues) -- the right call for an
/// isolated site, but wrong for one of hundreds of sites all reading through
/// the same physical table (see analysis::DispatchRegion): collapsing every
/// one loses the table identity that ties them together, with nothing
/// (yet) rebuilding it. `minRegionSites` defers that collapse -- keeping the
/// table-mode `Switch` instead -- for a site that is a member of a region
/// with at least this many sites; below it, a two-way table dispatch really
/// is just an isolated `if` and collapses exactly as before.
struct StructureOptions {
  /// A region this large is `analysis::ObfuscationProfile::likelyFlattened`'s
  /// own `dispatcherFanIn >= 8` floor, restated for region membership rather
  /// than one block's unresolved fan-in -- the same "this is not an ordinary
  /// switch table" line, drawn from IL shape alone.
  std::size_t minRegionSites = 8;
  /// Test/diagnostic override, independent of `minRegionSites`: when true,
  /// every resolved two-way table dispatch defers its collapse, regardless
  /// of whether it belongs to any region at all -- lets a small synthetic
  /// fixture exercise "what a deferred site prints like" without first
  /// building a region large enough to cross `minRegionSites` on its own.
  /// Never set by decompileToC()'s own default path.
  bool deferRegionCollapse = false;
  /// J2 (docs/architecture-optimization-eval-prompt.md §3 Phase 3): gates the
  /// not-yet-implemented region pass named at `kRegionPatterns` -- collapsing
  /// a whole analysis::DispatchRegion's nested tree of two-way sites into one
  /// mega-switch rather than leaving `switchFor` to structure each site on
  /// its own. Frozen here, ahead of that pass's own landing, so
  /// `StructureOptions`'s shape does not have to change again once it does;
  /// `Structurizer` does not read this field yet, so setting it true today
  /// changes nothing.
  bool regionStructuring = false;
};

/// Structures the whole function. The analyses must be computed from the
/// same function and outlive the call.
[[nodiscard]] StructuredFunction structureFunction(
    const il::Function& function, const analysis::Dominators& dominators,
    const analysis::PostDominators& postDominators,
    std::span<const analysis::NaturalLoop> loops,
    const StructureOptions& options = {});

}  // namespace xdec::emit
