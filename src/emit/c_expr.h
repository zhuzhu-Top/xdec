// IL expressions as C expressions.
//
// Two rules govern everything here. First, the IL's arithmetic is exact at its
// declared width, so a sub-64-bit operation carries a cast that is semantics
// rather than decoration. Second, C's usual arithmetic conversions are not the
// IL's: a signed operation must have BOTH operands in a signed type, or C
// silently performs it unsigned.
#pragma once

#include <optional>
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

  /// The expression as a pointer to `width` bits, for a context that takes
  /// the address itself rather than dereferencing it -- an ACLE atomic call
  /// is the first such case (see c_stmt.cpp's Load/Store/Intrinsic
  /// handling). Bare when the expression is already declared as a pointer
  /// (an argument, a typed field): unlike `integerOperand`, this is not
  /// arithmetic on the value, so there is no reason to round-trip it
  /// through `(uint64_t)` first. Cast through `width`'s own pointer type
  /// otherwise, the same as `integerOperand`'s plain text would be cast by
  /// whatever dereferences it.
  [[nodiscard]] std::string pointerOperand(il::ExprId id, uint32_t width);

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

  /// For a `Store`'s own value operand (see `c_stmt.cpp`'s `Store` case,
  /// docs/09 shape H2): if `id` is about to be named for the first time in
  /// this scope (shared, and not already materialized under some other
  /// name), names it `preferredName` instead of a fresh `_cseN` and returns
  /// the text that would have been its initializer, so the caller can print
  /// `preferredName = <text>;` directly rather than `_cseN = <text>;`
  /// followed by a second line copying `_cseN` into the local. Any later
  /// reference to the same node in this scope then reads `preferredName`
  /// straight from the cache `materialized()`/`rootText()`/`rootInteger()`
  /// all check first, the same way a `_cseN` name would have been reused.
  ///
  /// Returns `std::nullopt` in the two cases where renaming would be wrong
  /// or pointless: `id` was already materialized by an earlier statement in
  /// this scope (that statement already printed the name it got, which
  /// cannot be retroactively changed), or `id` is not shared at all (it
  /// would have printed inline, one line, with nothing to fold).
  [[nodiscard]] std::optional<std::string> materializeAs(il::ExprId id,
                                                          const std::string& preferredName);

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

  /// `id`'s text for one side of an `==`/`!=` comparison, where
  /// `comparedToZero` says the OTHER side is the literal 0. Zero-extension
  /// never changes whether a value is zero, so `ZExt(x) == 0` reads exactly
  /// as `x == 0` at `x`'s own width -- the one comparison outcome that does
  /// not depend on the width `compare()`'s cast would otherwise assert.
  /// Every other comparison (against a non-zero constant, or between two
  /// live values) really does depend on it, so this only unwraps here.
  [[nodiscard]] std::string equalityOperand(il::ExprId id, bool comparedToZero);

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
