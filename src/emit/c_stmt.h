// Blocks, ops and structured statements as C.
//
// The delicate part is where a phi copy goes. A phi's incoming value belongs to
// one EDGE, so its copy must be emitted where control takes that edge and
// nowhere else: at the end of a block that branches unconditionally, inside the
// arm of an if, before a goto, under a switch case. Emitting a block's copies
// for all of its successors would run the wrong edge's copies, and emitting
// them in sequence would let them observe each other's writes; both are
// handled here rather than left to the caller.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "c_context.h"
#include "c_expr.h"
#include "xdec/analysis/live_register_frame.h"

namespace xdec::emit {

/// A resolved computed branch's own `IndirectBranch` never prints (control
/// flow belongs to the statement tree, like every other terminator -- see
/// StmtPrinter::printOp's terminator cases): once `switchStmt` is a genuine
/// table-mode switch built from `blockStmt`'s terminator, the branch's
/// ORIGINAL target -- table-base-plus-index-times-stride, read out of the
/// jump table by a `Load` right before it -- has no reader left anywhere in
/// the block. The switch dispatches on the index alone (see structure.cpp's
/// `switchFor`), not the address that load computed, and `addExprRoots`
/// already agrees nothing else in the block reaches it either. Printing
/// that load's assignment anyway is exactly the shape D duplicate
/// docs/09-expression-reuse.md calls out: a real memory read the target
/// binary performs, now entirely vestigial in the reconstruction once the
/// switch it fed is resolved. Returns that load's `OpId` when the shape
/// matches exactly (single reader, resolved table mode), invalid otherwise.
///
/// Free (not a `StmtPrinter` member) so `collectDeadOps` can answer this
/// once, before any op is named or printed -- see its own note on why that
/// matters for declarations.
[[nodiscard]] il::OpId deadJumpTableLoad(const il::Function& function, il::BlockId block,
                                         bool tableMode, il::ExprId cond);

/// Walks the whole structured tree for every Block-immediately-followed-by-
/// its-own-Switch pairing (see `deadJumpTableLoad`), wherever it is nested --
/// inside a loop body, an if arm, another switch's case -- and for every
/// import-accessor call a later Store in the same block folds into itself
/// (see `printOp`'s Store case and docs/10-import-resolution.md's errno
/// idiom: `t = __errno_location(); *(uint32_t*)t = -ret;` becomes
/// `*__errno_location() = -ret;`, and the call's own temporary is one this
/// set marks dead the same way a vestigial jump-table load is). Computed
/// once, up front: `c_printer.cpp`'s declaration pass must not declare a
/// temporary for an op the body will never print, and `StmtPrinter::printOp`
/// must skip (or fold away) that same op, so both read this one answer
/// instead of risking two independently-reasoned ones drifting apart.
[[nodiscard]] std::unordered_set<uint32_t> collectDeadOps(const il::Function& function,
                                                          const Stmt& root,
                                                          const COptions& options);

class StmtPrinter {
 public:
  StmtPrinter(CContext& context, ExprPrinter& expressions)
      : ctx_(context), expressions_(expressions) {}

  /// The function body, indented one level.
  [[nodiscard]] std::string run();

 private:
  // -- statements -------------------------------------------------------------

  void printStmt(const StmtPtr& stmt, std::string& out);
  void printIf(const Stmt& stmt, std::string& out);
  void printWhile(const Stmt& stmt, std::string& out);
  void printDoWhile(const Stmt& stmt, std::string& out);
  /// `openScope` is false only when `printStmt`'s Sequence handler has
  /// already folded this switch's discriminant into the CSE scope its
  /// immediately preceding paired block opened (see printBlock's
  /// `extraRoots` and the Sequence case below): the switch then reuses that
  /// scope instead of resetting it, so a value the block already shares
  /// with the discriminant is named once, not twice.
  void printSwitch(const Stmt& stmt, std::string& out, bool openScope = true);

  /// `stmt.frame`, printed once as `shadow[i] = live[i]` for every slot,
  /// right before the switch it belongs to. Establishes the invariant that
  /// lets `printEdge` skip a handler's copy into a slot it never actually
  /// changes (see the class-level note on `activeFrame_`): without this,
  /// skipping that copy on whichever handler happens to run first would read
  /// the shadow register's declaration-time garbage instead of the live
  /// value it is supposed to still equal. `unanimous` slots (see
  /// `ActiveFrame::unanimous`) skip even this: nothing downstream ever reads
  /// the shadow variable's value for one of those, seeded or not.
  void printFrameSeed(const analysis::LiveRegisterFrame& frame,
                      const std::vector<bool>& unanimous, std::string& out);

  /// A select chain flattened into the guards it really is, every piece already
  /// printed to a name.
  ///
  /// `arms` is the chain longest-first -- a select whose else-arm is another
  /// select is an else-if, which is how a compiler builds a clamp or a
  /// three-way compare -- and `fallback` is what the innermost else yields.
  struct SelectChain {
    std::vector<std::pair<std::string, std::string>> arms;  // condition, value
    std::string fallback;
  };

  /// Decomposes a select chain, naming every subexpression in it *before* the
  /// caller emits a single branch. Nothing printed and nothing returned when
  /// the value is not a select.
  ///
  /// The ordering is the whole reason this is one function rather than each
  /// caller's business: the expression printer hoists a shared subexpression to
  /// a temporary at its first use, and a first use inside a guard would leave
  /// the arm that does not run reading a temporary nothing assigned.
  [[nodiscard]] std::optional<SelectChain> flattenSelect(il::ExprId value,
                                                         std::string& out);

  /// Writes `return cond ? a : b;` as a guard chain. False when the returned
  /// value is not a select at all, and then nothing has been printed.
  bool printSelectReturn(il::ExprId value, std::string& out);

  /// Writes `lhs = cond ? a : b;` as an if/else chain, where `assign` renders
  /// the assignment of one arm -- a store's lvalue, a register write's
  /// masking insert, a phi copy's name. False when the value is not a select,
  /// and then nothing has been printed.
  ///
  /// Unlike the return form this needs the `else`: an assignment does not leave
  /// the statement, so the arms have to exclude each other explicitly.
  bool printSelectAssign(il::ExprId value, std::string& out,
                         const std::function<std::string(const std::string&)>& assign);
  /// `extraRoots` are folded into this block's own CSE scope before any of
  /// its ops print, so a value the block shares with them is recognised as
  /// shared from its very first use (see printSwitch's `openScope`). An op
  /// in `ctx_.deadOps` (see `collectDeadOps`) is skipped entirely: neither
  /// counted as a CSE root nor printed.
  void printBlock(const Stmt& stmt, std::string& out,
                  const std::vector<il::ExprId>& extraRoots = {});

  // -- ops --------------------------------------------------------------------

  /// Takes the handle, not the op, because what an op prints as includes what
  /// analysis noted about it (il::Function::noteOn), and a note is keyed by
  /// handle.
  void printOp(il::OpId opId, std::string& out);
  void printCall(const il::Op& op, std::string& out);
  /// The function-pointer type a computed call is cast through, from what a
  /// header said about the callee where it said anything.
  [[nodiscard]] std::string calleeCast(const types::TypeEntry* callee,
                                       std::size_t argCount, bool hasResult);
  /// `*__errno_location() = -(x);` in place of the ordinary two-statement
  /// form, for the one Store `collectDeadOps`'s `foldableErrnoCall` matched
  /// (see its own note): the call this store's address value comes from is
  /// already in `ctx_.deadOps`, so printing it here is this fold's only
  /// remaining trace, same as any other dead op that never gets its own
  /// statement. False for every other Store, and then nothing is printed.
  [[nodiscard]] bool printFoldedImportStore(const il::Op& op, std::string& out);
  void printIntrinsic(const il::Op& op, std::string& out);
  /// The `svc` intrinsic as a syscall. False when this op is not one, or when
  /// nothing is known about the number, and then nothing has been printed and
  /// the caller falls back to the generic intrinsic form.
  bool printSyscall(const il::Op& op, std::string& out);

  /// A memory access as an lvalue: the local variable for a stack slot, a
  /// field of an imported struct where one describes the address, a typed
  /// dereference otherwise. `result` is the value a load defines, so that a
  /// field's declared type can carry to it; invalid for a store, which defines
  /// nothing.
  [[nodiscard]] std::string memoryLvalue(il::ExprId address, uint32_t width,
                                        std::string& out,
                                        il::ValueId result = {});

  // -- subexpression sharing ----------------------------------------------
  //
  // `expressions_` only knows about the scope the most recent `beginScope`
  // call set up (see c_expr.h); it is on the caller here to open one that
  // covers exactly the expressions about to print as one run of statements,
  // then thread `out` through every `text()`/`integerOperand()` call in
  // that run through these two wrappers so a subexpression the scope found
  // shared is declared as its own statement immediately before the code
  // that first needs it, instead of duplicated inline.
  [[nodiscard]] std::string exprText(il::ExprId id, std::string& out);
  [[nodiscard]] std::string exprInt(il::ExprId id, std::string& out);

  /// Like exprText/exprInt, but for a call site that hands the WHOLE value
  /// to a genuine C conversion -- a plain assignment, a return, an
  /// address cast, a truth test -- rather than to another operator. See
  /// ExprPrinter::rootText for why that distinction lets a redundant
  /// top-level ZExt cast be dropped here but nowhere inside inner().
  [[nodiscard]] std::string assignedText(il::ExprId id, std::string& out);
  [[nodiscard]] std::string assignedInt(il::ExprId id, std::string& out);

  /// Reading and writing the registers register SSA left in op form. A view
  /// (w0 in x0, s0 in q0) goes through its root variable so the two never
  /// drift apart into unrelated variables.
  [[nodiscard]] std::string registerRead(il::RegId reg);
  [[nodiscard]] std::string registerWrite(il::RegId reg, const std::string& value);

  // -- edges ------------------------------------------------------------------

  /// One phi copy along an edge.
  struct EdgeCopy {
    il::ValueId destination;
    std::string name;
    std::string type;
    il::ExprId source;
  };

  [[nodiscard]] std::vector<EdgeCopy> edgeCopies(il::BlockId from,
                                                il::BlockId to) const;
  void printEdge(il::BlockId from, il::BlockId to, std::string& out);

  /// Consumes `pending_`: the edge into `to` that a structured arm or body
  /// entry left for its first statement to emit.
  void consumePending(il::BlockId to, std::string& out);

  /// The block a statement tree transfers control to first, invalid when the
  /// tree begins with no block of its own.
  [[nodiscard]] il::BlockId firstBlockOf(const Stmt* stmt) const;

  [[nodiscard]] const il::Op* terminatorOf(il::BlockId block) const;
  void line(std::string& out, std::string_view text) const {
    appendLine(out, indent_, text);
  }
  [[nodiscard]] std::string label(il::BlockId block) const;

  CContext& ctx_;
  ExprPrinter& expressions_;
  unsigned indent_ = 1;
  /// The block printed most recently: the source of a structured arm's edge.
  il::BlockId last_{};
  /// An edge whose copies the next statement must emit before its own code.
  il::BlockId pending_{};

  /// The dispatcher frame the switch currently being printed carries (see
  /// `printFrameSeed`), for the whole time its case bodies and epilogue are
  /// printing. `printEdge` consults this to drop a handler's copy into a
  /// slot `analysis::classifyHandlerExit` says it never touches: correct
  /// only because `printFrameSeed` already made that slot's shadow variable
  /// equal to the live one before any case ran, and the switch's own
  /// (unsuppressed) restore edge copies it back out unconditionally on every
  /// path, so a handler that skipped its copy leaves the same value there
  /// the restore then reads. Null outside of such a switch, and whenever the
  /// switch has no `frame` at all.
  struct ActiveFrame {
    il::BlockId merge;
    analysis::LiveRegisterFrame frame;
    /// Parallel to `frame.slots`: `analysis::unanimousPassthroughSlots`,
    /// computed once when the switch is entered rather than per edge. A
    /// unanimous slot needs no seed (see `printFrameSeed`) and no restore
    /// copy either -- every handler already leaves the hub phi's own value
    /// sitting exactly where the restore would have copied it from.
    std::vector<bool> unanimous;
  };
  std::optional<ActiveFrame> activeFrame_;
};

}  // namespace xdec::emit
