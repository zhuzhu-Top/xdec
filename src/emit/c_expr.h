// IL expressions as C expressions.
//
// Two rules govern everything here. First, the IL's arithmetic is exact at its
// declared width, so a sub-64-bit operation carries a cast that is semantics
// rather than decoration. Second, C's usual arithmetic conversions are not the
// IL's: a signed operation must have BOTH operands in a signed type, or C
// silently performs it unsigned.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "c_context.h"

namespace xdec::emit {

class ExprPrinter {
 public:
  explicit ExprPrinter(CContext& context) : ctx_(context) {}

  /// The expression as C.
  [[nodiscard]] std::string text(il::ExprId id);

  /// The expression in a context that does integer arithmetic on it: a
  /// recovered pointer variable is cast back to an integer first.
  [[nodiscard]] std::string integerOperand(il::ExprId id);

  // -- root contexts ---------------------------------------------------------
  //
  // A ZExt's own cast to its destination width is never optional when its
  // text is embedded as an operand of another C operator: the shifted side
  // of `<<`/`>>` in particular is only well-defined up to its OWN promoted
  // width, not whatever width the surrounding expression implies, so
  // `text()`/`integerOperand()` (used from `inner()` for every nested
  // operand) always keep it. But a "root" call -- the whole value handed to
  // a plain C assignment, a return, or a truth test -- performs a genuine
  // conversion instead of an operator's promotion, and that conversion
  // already widens an unsigned value with no ambiguity regardless of
  // source width. `rootText`/`rootInteger` are for exactly those call
  // sites (see c_stmt.cpp) and drop a redundant top-level ZExt cast that
  // would otherwise just repeat what the assignment already does.
  [[nodiscard]] std::string rootText(il::ExprId id);
  [[nodiscard]] std::string rootInteger(il::ExprId id);

  // -- subexpression sharing ------------------------------------------------
  //
  // The IL's expression pool is hash-consed (see il/expr.h), so a
  // subexpression an obfuscator's MBA identities reuse many times is ONE
  // ExprId reachable from many parents, not many copies. Printing it
  // naively at every reference re-emits its full text each time, which is
  // fine for a handful of nodes but blows up into an unreadable, multi-KB
  // statement when a shared node sits several levels deep under other
  // shared nodes.
  //
  // The caller (StmtPrinter) owns the scope this operates over: exactly the
  // expressions about to be printed together as one run of statements (one
  // block's ops, one condition, one edge's phi copies). `beginScope` counts
  // references across that whole set up front and forgets any name a
  // previous, unrelated scope declared, so a shared node is recognised
  // across the statements that print together without ever reusing a name
  // whose assignment might not have executed yet on this path. A node
  // referenced 2+ times becomes an ordinary `name = <text>;` statement,
  // queued in `takePendingDecls()` for the caller to print immediately
  // before the code that first needs it — the same shape any other
  // decompiler emits a temporary in, not a C local declared mid-expression.
  void beginScope(const std::vector<il::ExprId>& roots);

  /// Adds `moreRoots` to the scope `beginScope` most recently opened,
  /// without forgetting anything it already counted or materialized. For a
  /// caller whose statements are two `printBlock`/`printSwitch`-shaped runs
  /// that always execute together (a resolved computed branch's own block
  /// immediately followed by the switch built from its terminator): a value
  /// referenced once in each run is referenced twice in their shared scope,
  /// so it is recognised as shared and materialized exactly once, in
  /// whichever run reaches it first, rather than a second time when the
  /// other run's `assignedText`/`exprText` reaches the same ExprId.
  void extendScope(const std::vector<il::ExprId>& moreRoots);
  [[nodiscard]] std::vector<std::string> takePendingDecls();

  [[nodiscard]] CContext& context() noexcept { return ctx_; }

 private:
  /// An expression's text plus the one type fact C cares about: whether it is
  /// pointer-typed and therefore cannot go bare into arithmetic.
  struct Text {
    std::string text;
    bool pointer = false;
  };

  [[nodiscard]] Text value(il::ExprId id);
  [[nodiscard]] Text materialized(il::ExprId id);
  [[nodiscard]] std::string inner(il::ExprId id);

  [[nodiscard]] std::string infix(il::ExprId id, std::string_view op, bool sign);
  [[nodiscard]] std::string shift(il::ExprId id, std::string_view op);
  [[nodiscard]] std::string compare(il::ExprId id, std::string_view op, bool sign);
  [[nodiscard]] std::string signExtend(il::ExprId id);
  [[nodiscard]] std::string widthWrap(const il::Expr& expr, std::string text,
                                      bool sign);

  /// The operand of a signed operation, in the signed type of `width`.
  [[nodiscard]] std::string signedOperand(il::ExprId id, uint32_t width);

  /// True when `expr` is a ZExt whose leading cast a root context (see
  /// above) can drop: its own operand is already a plain scalar integer,
  /// so the conversion the root context performs widens it correctly on
  /// its own.
  [[nodiscard]] bool isRedundantRootZext(const il::Expr& expr) const;

  /// Adds `root` and everything under it to `referenceCounts_`, without
  /// walking a node's operands more than once regardless of how many
  /// parents in the current scope share it.
  void countReferences(il::ExprId root);
  /// Worth naming: reached 2+ times in the current scope, and not a leaf a
  /// name would not shorten anything for (a bare constant or variable is
  /// already as short as a reference to it would be).
  [[nodiscard]] bool isShared(il::ExprId id, const il::Expr& expr) const;

  CContext& ctx_;
  unsigned cseCounter_ = 0;
  std::unordered_map<uint32_t, unsigned> referenceCounts_;
  std::unordered_map<uint32_t, Text> materializedText_;
  std::vector<std::string> pendingDecls_;
};

}  // namespace xdec::emit
