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

#include <memory>
#include <optional>
#include <vector>

#include "xdec/analysis/dominators.h"
#include "xdec/analysis/live_register_frame.h"
#include "xdec/analysis/loops.h"
#include "xdec/il/function.h"

namespace xdec::emit {

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
};

/// Structures the whole function. The analyses must be computed from the
/// same function and outlive the call.
[[nodiscard]] StructuredFunction structureFunction(
    const il::Function& function, const analysis::Dominators& dominators,
    const analysis::PostDominators& postDominators,
    std::span<const analysis::NaturalLoop> loops);

}  // namespace xdec::emit
