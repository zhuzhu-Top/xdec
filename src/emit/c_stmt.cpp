// StmtPrinter (see the header for where phi copies go and why).
#include "c_stmt.h"

#include <algorithm>
#include <array>
#include <format>
#include <set>
#include <string_view>
#include <vector>

#include "address_render.h"
#include "xdec/analysis/import_callee.h"
#include "xdec/analysis/noreturn.h"
#include "xdec/il/expr_roots.h"
#include "xdec/passes/recover_syscall.h"
#include "xdec/types/syscall_table.h"

namespace xdec::emit {

namespace {

/// The low `width` bits as a C constant, in a form that works past 64 bits.
[[nodiscard]] std::string lowMask(uint32_t width, const std::string& type) {
  if (width >= 64) {
    return std::format("(((({})1) << {}) - 1)", type, width);
  }
  return std::format("0x{:x}", (uint64_t{1} << width) - 1);
}

using il::addExprRoots;
using il::collectValueLeaves;

/// The bits of what a declared pointer parameter points at, or 0 when
/// `callee` is null, has no such parameter at `index`, or that parameter is
/// not a pointer at all -- every one of which means "no positive evidence
/// this argument is address-shaped", the gate callArgumentText's own Global
/// branch will not cross without.
[[nodiscard]] uint32_t paramPointeeWidth(const CContext& ctx, const types::TypeEntry* callee,
                                        std::size_t index) {
  if (callee == nullptr || ctx.options.types == nullptr || index >= callee->params.size()) {
    return 0;
  }
  const types::TypeDatabase& database = *ctx.options.types;
  const types::TypeEntry* param = database.get(database.resolveTypedef(callee->params[index].type));
  if (param == nullptr || param->kind != types::TypeKind::Pointer) {
    return 0;
  }
  return static_cast<uint32_t>(database.sizeOf(param->element).value_or(1) * 8);
}

}  // namespace

std::string StmtPrinter::run() {
  std::string body;
  indent_ = 1;
  printStmt(ctx_.structured.root, body);
  return body;
}

// ---------------------------------------------------------------------------
// Subexpression sharing
// ---------------------------------------------------------------------------

std::string StmtPrinter::exprText(il::ExprId id, std::string& out) {
  std::string result = expressions_.text(id);
  for (const std::string& decl : expressions_.takePendingDecls()) {
    line(out, decl);
  }
  return result;
}

std::string StmtPrinter::exprInt(il::ExprId id, std::string& out) {
  std::string result = expressions_.integerOperand(id);
  for (const std::string& decl : expressions_.takePendingDecls()) {
    line(out, decl);
  }
  return result;
}

std::string StmtPrinter::exprPointer(il::ExprId id, uint32_t width, std::string& out) {
  std::string result = expressions_.pointerOperand(id, width);
  for (const std::string& decl : expressions_.takePendingDecls()) {
    line(out, decl);
  }
  return result;
}

std::string StmtPrinter::assignedText(il::ExprId id, std::string& out) {
  std::string result = expressions_.rootText(id);
  for (const std::string& decl : expressions_.takePendingDecls()) {
    line(out, decl);
  }
  return result;
}

std::string StmtPrinter::assignedInt(il::ExprId id, std::string& out) {
  std::string result = expressions_.rootInteger(id);
  for (const std::string& decl : expressions_.takePendingDecls()) {
    line(out, decl);
  }
  return result;
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

void StmtPrinter::printStmt(const StmtPtr& stmt, std::string& out) {
  switch (stmt->kind) {
    case StmtKind::Sequence:
      // A resolved computed branch always lowers to exactly this pair,
      // consecutive and over the same block (see structure.cpp's
      // IndirectBranch case): the block's own straight-line ops, then the
      // switch built from its terminator. They print together on every
      // path that reaches either, so counting the switch's discriminant
      // into the block's CSE scope up front is safe -- unlike an if/while
      // condition, which stays a scope of its own (see c_expr.h) because an
      // untaken arm must not read a name only the other arm assigned.
      for (std::size_t index = 0; index < stmt->items.size(); ++index) {
        const StmtPtr& item = stmt->items[index];
        if (item->kind == StmtKind::Block && index + 1 < stmt->items.size()) {
          const StmtPtr& next = stmt->items[index + 1];
          if (next->kind == StmtKind::Switch && next->block == item->block &&
              next->cond.valid()) {
            printBlock(*item, out, {next->cond});
            printSwitch(*next, out, /*openScope=*/false);
            ++index;
            continue;
          }
        }
        printStmt(item, out);
      }
      break;
    case StmtKind::Block:
      printBlock(*stmt, out);
      break;
    case StmtKind::If:
      printIf(*stmt, out);
      break;
    case StmtKind::While:
      printWhile(*stmt, out);
      break;
    case StmtKind::DoWhile:
      printDoWhile(*stmt, out);
      break;
    case StmtKind::Switch:
      printSwitch(*stmt, out);
      break;
    case StmtKind::Goto:
      consumePending(stmt->block, out);
      line(out, std::format("goto {};", label(stmt->block)));
      break;
    case StmtKind::Continue:
      // Same as a goto to the header in every respect but the spelling: the back
      // edge's copies are what the next iteration reads, and they are printed by
      // whatever branched here, exactly as they would be before a goto.
      consumePending(stmt->block, out);
      line(out, "continue;");
      break;
    case StmtKind::Break:
      // A dispatcher case's `Break` names no block of its own -- the code it
      // hands control to (the switch's epilogue) prints right where the
      // switch itself sits, and nothing is left pending by the time
      // claimDispatcherCaseBody appends one. A loop's own early-exit `Break`
      // (see Structurizer::rewriteLoopExitToBreak) keeps the block its
      // `Goto` would have named, solely so the edge it took still gets its
      // copies printed here, exactly as the `goto` it replaced would have.
      if (stmt->block.valid()) {
        consumePending(stmt->block, out);
      }
      line(out, "break;");
      break;
  }
}

/// The head block was printed immediately before, so it is `last_`: both arms
/// take their edge from it. An arm the structurizer left empty is where the
/// head's own copies for that target belong, so an `else` appears when the
/// untaken edge carries copies even though it has no code.
void StmtPrinter::printIf(const Stmt& stmt, std::string& out) {
  const il::BlockId head = last_;
  expressions_.beginScope({stmt.cond});
  std::string condition = assignedText(stmt.cond, out);
  if (stmt.invertCond) {
    condition = std::format("!({})", condition);
  }

  // The two targets, so an empty arm can still carry its edge's copies.
  il::BlockId taken;
  il::BlockId untaken;
  if (const il::Op* terminator = terminatorOf(head);
      terminator != nullptr && terminator->code == il::OpCode::CondBranch) {
    const auto targets = ctx_.function.targets(*terminator);
    taken = targets[stmt.invertCond ? 1 : 0];
    untaken = targets[stmt.invertCond ? 0 : 1];
  }
  const il::BlockId thenFirst = firstBlockOf(stmt.thenArm.get());
  const il::BlockId elseFirst = firstBlockOf(stmt.elseArm.get());
  // A null arm's edge target is whichever of the two the other arm did not
  // claim; with both arms present neither fallback is needed.
  const auto uncovered = [&](il::BlockId claimed) {
    if (taken.valid() && taken != claimed) {
      return taken;
    }
    if (untaken.valid() && untaken != claimed) {
      return untaken;
    }
    return il::BlockId{};
  };

  line(out, std::format("if ({}) {{", condition));
  ++indent_;
  if (stmt.thenArm) {
    pending_ = head;
    printStmt(stmt.thenArm, out);
  } else if (const il::BlockId target = uncovered(elseFirst); target.valid()) {
    printEdge(head, target, out);
  }
  --indent_;
  const il::BlockId elseTarget = uncovered(thenFirst);
  const bool elseCopies =
      !stmt.elseArm && elseTarget.valid() && !edgeCopies(head, elseTarget).empty();
  if (stmt.elseArm || elseCopies) {
    line(out, "} else {");
    ++indent_;
    if (stmt.elseArm) {
      pending_ = head;
      printStmt(stmt.elseArm, out);
    } else {
      printEdge(head, elseTarget, out);
    }
    --indent_;
  }
  line(out, "}");
  pending_ = il::BlockId{};
  last_ = head;
}

/// `while (cond) { body }`: the header is the condition and prints no code, so
/// its incoming edge is emitted before the loop, the edge into the body at the
/// body's start, and the exit edge is left pending for what follows. The back
/// edge needs nothing here — the body's last block branches to the header and
/// emits its own copies, which is exactly the end of the body.
void StmtPrinter::printWhile(const Stmt& stmt, std::string& out) {
  const il::BlockId header = stmt.block;
  consumePending(header, out);
  // No condition: a loop whose only exits are the returns and jumps inside it,
  // which is the shape of a dispatch loop. The header's own code is in the body.
  if (!stmt.cond.valid()) {
    line(out, "while (true) {");
    ++indent_;
    pending_ = il::BlockId{};
    printStmt(stmt.body, out);
    --indent_;
    line(out, "}");
    return;
  }
  expressions_.beginScope({stmt.cond});
  std::string condition = assignedText(stmt.cond, out);
  if (stmt.invertCond) {
    condition = std::format("!({})", condition);
  }
  line(out, std::format("while ({}) {{", condition));
  ++indent_;
  pending_ = header;
  printStmt(stmt.body, out);
  --indent_;
  line(out, "}");
  // Whatever follows takes the header's exit edge.
  pending_ = header;
  last_ = header;
}

std::string StmtPrinter::doWhileCondition(il::ExprId cond, bool invertCond, std::string& out) {
  const il::Expr& e = ctx_.function.expr(cond);
  bool negate = invertCond;
  il::ExprId operand{};
  uint64_t constant = 0;
  if (e.op == il::ExprOp::CmpNe || e.op == il::ExprOp::CmpEq) {
    if (ctx_.function.asConstant(e.operand(1), constant) && constant == 0) {
      operand = e.operand(0);
    } else if (ctx_.function.asConstant(e.operand(0), constant) && constant == 0) {
      operand = e.operand(1);
    }
    if (operand.valid() && e.op == il::ExprOp::CmpEq) {
      negate = !negate;
    }
  }
  const std::string text = operand.valid() ? exprInt(operand, out) : assignedText(cond, out);
  return negate ? std::format("!({})", text) : text;
}

/// `do { body } while (cond)`: the body contains the header and the latch, so
/// the only edge without a home is the latch's back edge. It runs on the
/// iterating path only, which in this shape means under the condition.
void StmtPrinter::printDoWhile(const Stmt& stmt, std::string& out) {
  const il::BlockId header = stmt.block;
  consumePending(header, out);
  line(out, "do {");
  ++indent_;
  printStmt(stmt.body, out);
  const il::BlockId latch = last_;
  expressions_.beginScope({stmt.cond});
  const std::string condition = doWhileCondition(stmt.cond, stmt.invertCond, out);
  if (header.valid() && !edgeCopies(latch, header).empty()) {
    line(out, std::format("if ({}) {{", condition));
    ++indent_;
    printEdge(latch, header, out);
    --indent_;
    line(out, "}");
  }
  --indent_;
  line(out, std::format("}} while ({});", condition));
  pending_ = latch;
  last_ = latch;
}

void StmtPrinter::printSwitch(const Stmt& stmt, std::string& out, bool openScope) {
  if (stmt.block.valid()) {
    consumePending(stmt.block, out);
    // A compare-chain switch replaces its dispatcher block outright (no
    // separate `Block` stmt precedes it), so a handler branching back to the
    // dispatcher needs this label. A resolved computed branch's switch is
    // different: `emitRegion` always prints that block's own straight-line
    // ops as a `Block` stmt right before this one, which already wrote the
    // same label if the block needed one -- `last_ == stmt.block` says so,
    // and printing it again here would be the one label appearing twice.
    if (ctx_.structured.isLabeled(stmt.block) && last_ != stmt.block) {
      out += std::format("{}:\n", label(stmt.block));
    }
  }
  pending_ = il::BlockId{};

  // Saved/restored (rather than simply cleared on the way out) so that a
  // handler ending in its own nested resolved switch, if that one somehow
  // matched a frame of its own, cannot leave this switch's remaining case
  // bodies printing without the suppression they are entitled to.
  const std::optional<ActiveFrame> enclosingFrame = activeFrame_;
  if (stmt.frame) {
    // Only `shape.merge` is read by either analysis call below (see
    // `classifyHandlerExit`), so the rest of the shape -- not something a
    // `Stmt` keeps around once structuring is done -- can stay unset.
    const analysis::DispatcherShape shape{il::BlockId{}, stmt.mergeBlock, il::BlockId{}};
    const std::vector<bool> unanimous =
        analysis::unanimousPassthroughSlots(ctx_.function, shape, *stmt.frame);
    printFrameSeed(*stmt.frame, unanimous, out);
    activeFrame_ = ActiveFrame{stmt.mergeBlock, *stmt.frame, unanimous};
  }

  const bool enumerated = stmt.tableMode || !stmt.caseValues.empty();
  if (!enumerated) {
    // Nothing matched a table or a chain: an honest compare sequence over the
    // computed target, which at least names every successor.
    if (openScope) {
      expressions_.beginScope({stmt.cond});
    }
    const std::string target = assignedText(stmt.cond, out);
    bool first = true;
    for (std::size_t index = 0; index < stmt.cases.size(); ++index) {
      const il::BlockId arm = stmt.cases[index];
      const il::BlockId from = index < stmt.casePreds.size() ? stmt.casePreds[index]
                                                            : stmt.block;
      line(out, std::format("{}if ({} == 0x{:x}) {{", first ? "" : "} else ",
                            target, ctx_.function.block(arm).va));
      ++indent_;
      if (from.valid()) {
        printEdge(from, arm, out);
      }
      if (index < stmt.caseBodies.size() && stmt.caseBodies[index]) {
        pending_ = il::BlockId{};
        printStmt(stmt.caseBodies[index], out);
      } else {
        line(out, std::format("goto {};", label(arm)));
      }
      --indent_;
      first = false;
    }
    if (!first) {
      line(out, "}");
    }
    if (stmt.epilogue) {
      pending_ = il::BlockId{};
      printStmt(stmt.epilogue, out);
    }
    if (stmt.frame) {
      activeFrame_ = enclosingFrame;
    }
    return;
  }

  if (openScope) {
    expressions_.beginScope({stmt.cond});
  }
  const std::string discriminant = assignedText(stmt.cond, out);
  line(out, std::format("switch ({}) {{", discriminant));
  ++indent_;
  for (std::size_t index = 0; index < stmt.cases.size(); ++index) {
    if (stmt.tableMode) {
      // The case label is the table's ordinal, not anything a disassembler
      // would show at this address; whether or not `claimCaseBody` inlined
      // the handler below (in which case no `goto` prints its address at
      // all), naming the target here is what lets a reader match this case
      // up against the jump table or the graph view in another tool.
      line(out, std::format("case {}: /* handler @0x{:x} */", index,
                            ctx_.function.block(stmt.cases[index]).va));
    } else {
      line(out, std::format("case 0x{:x}:", stmt.caseValues[index]));
    }
    ++indent_;
    if (index < stmt.casePreds.size() && stmt.casePreds[index].valid()) {
      printEdge(stmt.casePreds[index], stmt.cases[index], out);
    }
    // A handler the structurizer could claim for this case is written here. Where
    // it could not — because other paths reach it too — the jump is what is true.
    if (index < stmt.caseBodies.size() && stmt.caseBodies[index]) {
      pending_ = il::BlockId{};
      printStmt(stmt.caseBodies[index], out);
    } else {
      line(out, std::format("goto {};", label(stmt.cases[index])));
    }
    --indent_;
  }
  if (stmt.defaultCase.valid()) {
    line(out, "default:");
    ++indent_;
    if (stmt.defaultPred.valid()) {
      printEdge(stmt.defaultPred, stmt.defaultCase, out);
    }
    if (stmt.defaultBody) {
      pending_ = il::BlockId{};
      printStmt(stmt.defaultBody, out);
    } else {
      line(out, std::format("goto {};", label(stmt.defaultCase)));
    }
    --indent_;
  }
  --indent_;
  line(out, "}");
  // The dispatcher shape's shared tail (see analysis::DispatcherShape),
  // structured once here instead of once per `goto`. Every case a `Break`
  // closes falls into it exactly where it now prints: right after the
  // switch, at the switch's own indentation.
  if (stmt.epilogue) {
    pending_ = il::BlockId{};
    printStmt(stmt.epilogue, out);
  }
  if (stmt.frame) {
    activeFrame_ = enclosingFrame;
  }
}

void StmtPrinter::printFrameSeed(const analysis::LiveRegisterFrame& frame,
                                 const std::vector<bool>& unanimous, std::string& out) {
  for (std::size_t slot = 0; slot < frame.slots.size(); ++slot) {
    if (slot < unanimous.size() && unanimous[slot]) {
      continue;
    }
    const analysis::LiveRegisterSlot& registerSlot = frame.slots[slot];
    const il::ValueId liveValue = ctx_.function.op(registerSlot.livePhiAtHub).result;
    const il::ValueId shadowValue = ctx_.function.op(registerSlot.shadowPhiAtMerge).result;
    const analysis::Variable* liveVar = ctx_.variables.tempFor(liveValue);
    const analysis::Variable* shadowVar = ctx_.variables.tempFor(shadowValue);
    if (liveVar == nullptr || shadowVar == nullptr) {
      continue;
    }
    line(out, std::format("{} = {};", shadowVar->name, liveVar->name));
  }
}

il::OpId deadJumpTableLoad(const il::Function& function, il::BlockId block, bool tableMode,
                          il::ExprId cond) {
  if (!tableMode || !block.valid()) {
    return {};
  }
  const auto& ops = function.block(block).ops;
  if (ops.empty()) {
    return {};
  }
  const il::Op& terminator = function.op(ops.back());
  if (terminator.code != il::OpCode::IndirectBranch) {
    return {};
  }
  const auto termOperands = function.operands(terminator);
  if (termOperands.empty()) {
    return {};
  }
  const il::Expr& targetExpr = function.expr(termOperands[0]);
  if (targetExpr.op != il::ExprOp::Value) {
    return {};
  }
  const il::ValueId target{static_cast<uint32_t>(targetExpr.immediate)};

  il::OpId candidate{};
  for (const il::OpId opId : ops) {
    if (const il::Op& op = function.op(opId); op.code == il::OpCode::Load && op.result == target) {
      candidate = opId;
      break;
    }
  }
  if (!candidate.valid()) {
    return {};
  }

  // The switch's own discriminant is the terminator's replacement; if it
  // still reaches `target` (an obfuscator's index computation folded back
  // through the loaded target rather than staying a separate value), the
  // load is not vestigial after all.
  std::set<uint32_t> reached;
  il::collectValueLeaves(function, cond, reached);
  if (reached.contains(target.index())) {
    return {};
  }
  // Nor may any other op in the block still read it.
  for (const il::OpId opId : ops) {
    if (opId == candidate) {
      continue;
    }
    std::vector<il::ExprId> roots;
    addExprRoots(function, function.op(opId), roots);
    for (const il::ExprId root : roots) {
      il::collectValueLeaves(function, root, reached);
    }
    if (reached.contains(target.index())) {
      return {};
    }
  }
  return candidate;
}

namespace {

/// Whether `va` names the syscall-error accessor a header declared under --
/// `__errno_location` on Android, the one name `printOp`'s Store-idiom fold
/// (see `foldableErrnoCall`) knows to look for. Reads exactly the two
/// resolvers `CContext::calleeName` does, in the same order, so a call this
/// finds foldable is one that would otherwise have printed under this exact
/// name (see c_context.h's `calleeName` and `binder_` for why the two must
/// agree).
[[nodiscard]] bool namesErrnoLocation(const COptions& options, uint64_t va) {
  if (options.symbols) {
    if (const SymbolRef symbol = options.symbols(va); symbol.exact() && symbol.isFunction) {
      return symbol.name == "__errno_location";
    }
  }
  if (options.names) {
    const types::BoundName named = options.names(va);
    return !named.empty() && named.isFunction && named.name == "__errno_location";
  }
  return false;
}

/// The call `foldableErrnoCall` (in `block`) folds into a later Store, when
/// this store is that fold's target: `*(width*)(t) = -(x)` where `t` is that
/// call's own (untrimmed) result. Nullopt for every other Store, including
/// one whose address is not a plain value at all.
[[nodiscard]] std::optional<il::ValueId> importAccessorStoreValue(const il::Function& function,
                                                                  const il::Op& store) {
  const auto operands = function.operands(store);
  if (function.expr(operands[1]).op != il::ExprOp::Neg) {
    return std::nullopt;
  }
  const il::Expr& addressExpr = function.expr(operands[0]);
  if (addressExpr.op != il::ExprOp::Value) {
    return std::nullopt;
  }
  return il::ValueId{static_cast<uint32_t>(addressExpr.immediate)};
}

/// A call to the errno accessor whose only reader, in `block`, is a Store
/// matching `*(width*)(errno_location()) = -(x)` (see
/// `importAccessorStoreValue`): the shape `sub_199214`-like syscall wrappers
/// leave behind once the accessor's name resolves (see nameResolverOf in
/// xdec_main.cpp), and `printOp`'s Store case folds into one line (see
/// docs/10-import-resolution.md). Invalid when the block matches no such
/// pair, so nothing is marked dead and the ordinary two-statement form
/// prints, same as before this fold existed.
[[nodiscard]] il::OpId foldableErrnoCall(const il::Function& function, il::BlockId block,
                                        const COptions& options) {
  if (!block.valid()) {
    return {};
  }
  const auto& ops = function.block(block).ops;
  for (const il::OpId opId : ops) {
    const il::Op& op = function.op(opId);
    if (op.code != il::OpCode::Store) {
      continue;
    }
    const std::optional<il::ValueId> value = importAccessorStoreValue(function, op);
    if (!value.has_value() || !function.hasValue(*value)) {
      continue;
    }
    const il::OpId definition = function.value(*value).definition;
    if (!definition.valid()) {
      continue;
    }
    const il::Op& call = function.op(definition);
    // Already trimmed to no arguments: the accessor takes none, and folding
    // a call that still carries the convention's full register set would
    // silently drop them rather than say so.
    if (call.code != il::OpCode::Call || function.operands(call).size() != 1) {
      continue;
    }
    uint64_t address = 0;
    if (!function.asConstant(function.operands(call)[0], address) ||
        !namesErrnoLocation(options, address)) {
      continue;
    }
    // No other op *anywhere in the function* may still read the call's
    // result -- not just this block. A dispatcher's merge point is exactly
    // the case that makes this matter: the call's result sits in x0, the
    // same register the ABI carries a live argument in, so a Phi at some
    // successor block's head (see edgeCopies) can read this value on this
    // very edge, and Phi operands do not live in this block's own `ops`. Only
    // when nothing anywhere still needs it does folding away its temporary
    // cost nothing.
    std::set<uint32_t> reached;
    for (const il::BlockId other_block : function.blockHandles()) {
      for (const il::OpId other : function.block(other_block).ops) {
        if (other == opId || other == definition) {
          continue;
        }
        const il::Op& otherOp = function.op(other);
        // addExprRoots does not know Phi's shape (its "operands" are one
        // value per incoming edge, not an expression tree to walk into), so
        // a Phi reading this value would otherwise pass right through this
        // scan unseen -- exactly the shape a dispatcher merge takes.
        if (otherOp.code == il::OpCode::Phi) {
          for (const il::ExprId edgeValue : function.operands(otherOp)) {
            collectValueLeaves(function, edgeValue, reached);
          }
          continue;
        }
        std::vector<il::ExprId> roots;
        addExprRoots(function, otherOp, roots);
        for (const il::ExprId root : roots) {
          collectValueLeaves(function, root, reached);
        }
      }
    }
    if (reached.contains(value->index())) {
      continue;
    }
    return definition;
  }
  return {};
}

void collectDeadOpsInto(const il::Function& function, const Stmt& stmt, const COptions& options,
                        std::unordered_set<uint32_t>& dead) {
  switch (stmt.kind) {
    case StmtKind::Sequence:
      for (std::size_t index = 0; index < stmt.items.size(); ++index) {
        const Stmt& item = *stmt.items[index];
        if (item.kind == StmtKind::Block && index + 1 < stmt.items.size()) {
          const Stmt& next = *stmt.items[index + 1];
          if (next.kind == StmtKind::Switch && next.block == item.block && next.cond.valid()) {
            if (const il::OpId dead_load =
                    deadJumpTableLoad(function, item.block, next.tableMode, next.cond);
                dead_load.valid()) {
              dead.insert(dead_load.index());
            }
          }
        }
        collectDeadOpsInto(function, item, options, dead);
      }
      break;
    case StmtKind::Block:
      if (const il::OpId dead_call = foldableErrnoCall(function, stmt.block, options);
          dead_call.valid()) {
        dead.insert(dead_call.index());
      }
      break;
    case StmtKind::If:
      if (stmt.thenArm) {
        collectDeadOpsInto(function, *stmt.thenArm, options, dead);
      }
      if (stmt.elseArm) {
        collectDeadOpsInto(function, *stmt.elseArm, options, dead);
      }
      break;
    case StmtKind::While:
    case StmtKind::DoWhile:
      if (stmt.body) {
        collectDeadOpsInto(function, *stmt.body, options, dead);
      }
      break;
    case StmtKind::Switch:
      for (const StmtPtr& body : stmt.caseBodies) {
        if (body) {
          collectDeadOpsInto(function, *body, options, dead);
        }
      }
      if (stmt.defaultBody) {
        collectDeadOpsInto(function, *stmt.defaultBody, options, dead);
      }
      if (stmt.epilogue) {
        collectDeadOpsInto(function, *stmt.epilogue, options, dead);
      }
      break;
    default:
      break;
  }
}

}  // namespace

std::unordered_set<uint32_t> collectDeadOps(const il::Function& function, const Stmt& root,
                                            const COptions& options) {
  std::unordered_set<uint32_t> dead;
  collectDeadOpsInto(function, root, options, dead);
  return dead;
}

void StmtPrinter::printBlock(const Stmt& stmt, std::string& out,
                              const std::vector<il::ExprId>& extraRoots) {
  // The incoming edge's copies precede the label: they belong to one
  // predecessor, and a label is where every other predecessor arrives.
  consumePending(stmt.block, out);
  const il::Block& block = ctx_.function.block(stmt.block);
  if (ctx_.structured.isLabeled(stmt.block)) {
    out += std::format("{}:\n", label(stmt.block));
  }
  if (ctx_.options.annotateBlocks) {
    line(out, std::format("// b{} @0x{:x}", stmt.block.index(), block.va));
  }
  // One shared scope for the whole block: an obfuscator's MBA identities
  // often recompute the exact same subexpression in several sibling
  // statements (see c_expr.h), and those statements always run together, so
  // naming it once here is safe and keeps the redundant statements short.
  // `extraRoots` (a paired switch's discriminant; see the Sequence case in
  // printStmt) join the count before any op prints, so a value shared with
  // that discriminant is recognised as shared from its first use here
  // rather than only once the switch reaches it in a scope of its own.
  std::vector<il::ExprId> roots;
  for (const il::OpId opId : block.ops) {
    if (ctx_.deadOps.contains(opId.index())) {
      continue;
    }
    addExprRoots(ctx_.function, ctx_.function.op(opId), roots);
  }
  expressions_.beginScope(roots);
  if (!extraRoots.empty()) {
    expressions_.extendScope(extraRoots);
  }
  for (const il::OpId opId : block.ops) {
    if (ctx_.deadOps.contains(opId.index())) {
      continue;
    }
    printOp(opId, out);
  }
  last_ = stmt.block;
  // An unconditional branch has exactly one edge, so its copies belong here.
  // Conditional and computed branches leave theirs to the arms and cases.
  if (const il::Op* terminator = terminatorOf(stmt.block);
      terminator != nullptr && terminator->code == il::OpCode::Branch) {
    printEdge(stmt.block, ctx_.function.targets(*terminator)[0], out);
  }
}

// ---------------------------------------------------------------------------
// Ops
// ---------------------------------------------------------------------------

bool StmtPrinter::printFoldedImportStore(const il::Op& op, std::string& out) {
  const std::optional<il::ValueId> value = importAccessorStoreValue(ctx_.function, op);
  if (!value.has_value() || !ctx_.function.hasValue(*value)) {
    return false;
  }
  const il::OpId definition = ctx_.function.value(*value).definition;
  // `foldableErrnoCall` is the only thing that ever marks a Call dead, so
  // finding this store's address value defined by a dead op *is* finding the
  // pairing it matched -- nothing else could have put a Call's index there.
  if (!definition.valid() || !ctx_.deadOps.contains(definition.index())) {
    return false;
  }
  const il::Op& call = ctx_.function.op(definition);
  if (call.code != il::OpCode::Call) {
    return false;
  }
  uint64_t address = 0;
  if (!ctx_.function.asConstant(ctx_.function.operands(call)[0], address)) {
    return false;
  }
  const std::string callee = ctx_.calleeName(address);
  ctx_.callees.emplace(address, callee);
  const auto operands = ctx_.function.operands(op);
  line(out, std::format("*{}() = {};", callee, assignedText(operands[1], out)));
  return true;
}

void StmtPrinter::printOp(il::OpId opId, std::string& out) {
  const il::Op& op = ctx_.function.op(opId);
  // Ahead of the statement, on its own line: what a pass established about this
  // op but could not write as code — an indirect call it recognised as a table
  // dispatch, say. It goes before rather than trailing because the statements
  // these land on are the long ones, and a comment at the end of a 300-column
  // line is a comment nobody reads.
  //
  // A Phi's own note (ssa_construct.cpp's "reg:xN", currently the only kind
  // placed on one) is bookkeeping for analysis to find that phi again --
  // there is no statement here for it to sit next to, since a phi's value
  // prints on the edges that feed it, never at its own definition site.
  if (op.code != il::OpCode::Phi) {
    if (const std::string_view note = ctx_.function.noteOn(opId); !note.empty()) {
      line(out, std::format("/* {} */", note));
    }
  }
  const auto operands = ctx_.function.operands(op);
  switch (op.code) {
    case il::OpCode::Load: {
      const std::string* temp = ctx_.tempFor(op.result);
      std::string lvalue;
      if (ctx_.exclusiveLoads.contains(opId.index())) {
        // ldaxr/ldxr, reassembled from the reserve+load pair specs/arm64/
        // loadstore.xspec splits it into (see analysis::findExclusiveLoads):
        // the reservation itself is dead, folded away with no line of its own.
        const uint32_t width = op.type.bits();
        ctx_.helpers.insert(std::format("ldaxr{}", width));
        lvalue = std::format("__ldaxr{}({})", width, exprPointer(operands[0], width, out));
      } else {
        lvalue = memoryLvalue(operands[0], op.type.bits(), out, op.result);
      }
      line(out, std::format("{} = {};", temp == nullptr ? "/*lost*/" : *temp, lvalue));
      break;
    }
    case il::OpCode::Store: {
      if (printFoldedImportStore(op, out)) {
        break;
      }
      // The lvalue first, always: it can hoist a temporary of its own, and one
      // hoisted under a guard would be missing on the path that skips it.
      const std::string lvalue = memoryLvalue(operands[0], op.type.bits(), out);
      // A pointer-typed stack slot (see analysis::VariableTable's stack-slot
      // pointer refinement) needs an explicit cast on what is stored into
      // it: the IL's value is an ordinary integer computation, and C never
      // implicitly converts one to the pointer type the declaration now
      // carries.
      std::string castPrefix;
      const analysis::AddressInfo storeAddress = ctx_.frame.classify(operands[0]);
      const analysis::Variable* storeLocal =
          storeAddress.kind == analysis::AddressKind::StackSlot
              ? ctx_.variables.localAt(storeAddress.delta)
              : nullptr;
      if (storeLocal != nullptr && storeLocal->type.pointerDepth > 0 &&
          storeLocal->type.width == op.type.bits()) {
        castPrefix = std::format("({})", storeLocal->type.format());
      }
      const auto assign = [&lvalue, &castPrefix](const std::string& arm) {
        return std::format("{} = {}{};", lvalue, castPrefix, arm);
      };
      if (!printSelectAssign(operands[1], out, assign)) {
        // H2 (docs/09, docs/14 Phase 4): a plain store into a named local
        // (no pointer cast, `lvalue` exactly the local's own bare name --
        // ruling out a field access, an aliased local, or a width mismatch,
        // any of which give `memoryLvalue` different text) whose value is
        // about to be materialized as a CSE temp for the first time in this
        // scope folds `_cseN = <expr>; var_X = _cseN;` into one `var_X =
        // <expr>;` -- the local already carries the name a fresh `_cseN`
        // would have needed.
        if (castPrefix.empty() && storeLocal != nullptr && lvalue == storeLocal->name) {
          if (const auto initializer = expressions_.materializeAs(operands[1], lvalue)) {
            line(out, assign(*initializer));
            break;
          }
        }
        line(out, assign(assignedText(operands[1], out)));
      }
      break;
    }
    case il::OpCode::ReadReg: {
      // Registers register SSA does not track stay as ops; naming the value is
      // what keeps it visible to every later use.
      const std::string* temp = ctx_.tempFor(op.result);
      line(out, std::format("{} = {};", temp == nullptr ? "/*lost*/" : *temp,
                            registerRead(op.reg())));
      break;
    }
    case il::OpCode::WriteReg: {
      const auto assign = [this, reg = op.reg()](const std::string& arm) {
        return registerWrite(reg, arm);
      };
      if (!printSelectAssign(operands[0], out, assign)) {
        line(out, assign(assignedText(operands[0], out)));
      }
      break;
    }
    case il::OpCode::Call:
      printCall(op, out);
      break;
    case il::OpCode::Intrinsic:
      printIntrinsic(opId, op, out);
      break;
    case il::OpCode::Unimplemented:
      line(out, std::format("__xdec_unimplemented(\"{}\");",
                            ctx_.function.nameOf(op.payload)));
      break;
    case il::OpCode::Return:
      if (operands.empty()) {
        line(out, "return;");
      } else if (!printSelectReturn(operands[0], out)) {
        const std::string value = assignedText(operands[0], out);
        line(out, std::format("return {};", value));
      }
      break;
    case il::OpCode::Unreachable:
      line(out, "__builtin_unreachable();");
      break;
    case il::OpCode::Phi:      // assigned on the incoming edges
    case il::OpCode::Nop:
    case il::OpCode::Branch:   // control flow belongs to the statement tree
    case il::OpCode::CondBranch:
    case il::OpCode::IndirectBranch:
      break;
    default:
      line(out, std::format("/* unprinted op: {} */", il::toString(op.code)));
      break;
  }
}

std::optional<StmtPrinter::SelectChain> StmtPrinter::flattenSelect(il::ExprId value,
                                                                   std::string& out) {
  if (!ctx_.options.preferIfOverTernary) {
    return std::nullopt;
  }
  // The convention widens a narrow result to the whole register, so a select
  // arrives wrapped in a cast. The expression printer already leaves that cast
  // out where the whole value is being converted anyway — an assignment, a
  // return — so looking through it here changes nothing about what the arms
  // say.
  while (true) {
    const il::Expr& expr = ctx_.function.expr(value);
    if (expr.op != il::ExprOp::ZExt && expr.op != il::ExprOp::SExt &&
        expr.op != il::ExprOp::Trunc) {
      break;
    }
    value = expr.operands[0];
  }
  if (ctx_.function.expr(value).op != il::ExprOp::Select) {
    return std::nullopt;
  }
  // Read as one expression a nested select is a wall of parentheses; read as a
  // chain of guards it is what the source said.
  std::vector<std::pair<il::ExprId, il::ExprId>> guards;
  il::ExprId rest = value;
  while (ctx_.function.expr(rest).op == il::ExprOp::Select) {
    const il::Expr& select = ctx_.function.expr(rest);
    guards.emplace_back(select.operands[0], select.operands[1]);
    rest = select.operands[2];
  }

  SelectChain chain;
  chain.arms.reserve(guards.size());
  for (const auto& [cond, taken] : guards) {
    const std::string condText = assignedText(cond, out);
    chain.arms.emplace_back(condText, assignedText(taken, out));
  }
  chain.fallback = assignedText(rest, out);
  return chain;
}

bool StmtPrinter::printSelectReturn(il::ExprId value, std::string& out) {
  const std::optional<SelectChain> chain = flattenSelect(value, out);
  if (!chain.has_value()) {
    return false;
  }
  for (const auto& [cond, taken] : chain->arms) {
    line(out, std::format("if ({}) {{", cond));
    ++indent_;
    line(out, std::format("return {};", taken));
    --indent_;
    line(out, "}");
  }
  line(out, std::format("return {};", chain->fallback));
  return true;
}

bool StmtPrinter::printSelectAssign(
    il::ExprId value, std::string& out,
    const std::function<std::string(const std::string&)>& assign) {
  const std::optional<SelectChain> chain = flattenSelect(value, out);
  if (!chain.has_value()) {
    return false;
  }
  bool first = true;
  for (const auto& [cond, taken] : chain->arms) {
    line(out, std::format("{}if ({}) {{", first ? "" : "} else ", cond));
    first = false;
    ++indent_;
    line(out, assign(taken));
    --indent_;
  }
  line(out, "} else {");
  ++indent_;
  line(out, assign(chain->fallback));
  --indent_;
  line(out, "}");
  return true;
}

std::string StmtPrinter::callArgumentText(const types::TypeEntry* callee, std::size_t paramIndex,
                                          il::ExprId operand, std::string& out) {
  if (std::string address = ctx_.addressOfLocal(operand); !address.empty()) {
    return address;
  }
  // Only past a recovered local's own address (StackSlot, handled above,
  // unconditionally -- see CContext::addressOfLocal) does this position's
  // *declared* type start to matter: a Global address is otherwise exactly
  // as ambiguous a bare number as it always was, and guessing it names a
  // pointer without the callee's own signature saying so is how a decoded
  // flag value would turn into a fabricated string the moment it happened
  // to double as a readable rodata address.
  if (const uint32_t pointeeWidth = paramPointeeWidth(ctx_, callee, paramIndex); pointeeWidth != 0) {
    const analysis::AddressInfo info = ctx_.frame.classify(operand);
    if (info.kind == analysis::AddressKind::Global) {
      if (const std::optional<analysis::ImageLiteral> literal = ctx_.literals.at(info.address)) {
        return analysis::quoteCString(literal->text);
      }
    }
    if (const auto rendered = AddressRenderer(ctx_).render(operand, AddressRole::PointerValue, pointeeWidth)) {
      return rendered->text;
    }
  }
  return exprText(operand, out);
}

void StmtPrinter::printCall(const il::Op& op, std::string& out) {
  const auto operands = ctx_.function.operands(op);
  // At Vars and beyond the operand list *is* the recovered argument list (see
  // passes/vars.h), so it prints verbatim: trimming it again here would be the
  // printer second-guessing an analysis, and the caveat below would be denying
  // a recovery that did in fact happen.
  //
  // Below Vars nothing has asked the question yet, and every call still carries
  // the full argument-register set whatever the callee's arity. A trailing run
  // of `/*undef*/0` tells a reader nothing beyond "unknown", so it is dropped —
  // and then the caveat is worth printing, because it says precisely what is
  // true of this output: it was emitted before arity recovery ran.
  const bool recovered = ctx_.function.maturity() >= il::Maturity::Vars;
  std::size_t argCount = operands.size() - 1;
  while (!recovered && argCount > 0 &&
         ctx_.function.expr(operands[argCount]).op == il::ExprOp::Undef) {
    --argCount;
  }
  const bool trimmed = argCount + 1 < operands.size();
  // Asked once, ahead of either branch below, because it also answers this
  // loop's own question (see callArgumentText) -- calleeType already
  // resolves a direct constant target the same way the branch that prints
  // one does, so there is nothing here for the direct/computed split to
  // change.
  const types::TypeEntry* callee = ctx_.calleeType(operands[0]);
  std::string args;
  for (std::size_t index = 1; index <= argCount; ++index) {
    args += index == 1 ? "" : ", ";
    args += callArgumentText(callee, index - 1, operands[index], out);
  }
  const std::string suffix = trimmed ? " /* + unknown arg(s) */" : "";
  const std::string* temp = ctx_.tempFor(op.result);
  const std::string assignee = temp == nullptr ? "" : *temp + " = ";
  uint64_t address = 0;
  if (ctx_.function.asConstant(operands[0], address)) {
    const std::string calleeName = ctx_.calleeName(address);
    ctx_.callees.emplace(address, calleeName);
    // Same annotation the syscall path prints for a noreturn number (see
    // printSyscall): a reader who has just read `abort()` or
    // `__stack_chk_fail()` should not have to also know each one's contract
    // to see why nothing here worries about what follows on this path.
    const std::string_view note =
        analysis::isKnownNoreturnSymbol(calleeName) ? " /* does not return */" : "";
    line(out, std::format("{}{}({}){}{};", assignee, calleeName, args, suffix, note));
    return;
  }
  const std::string prototype = calleeCast(callee, argCount, temp != nullptr);
  const std::string target = exprInt(operands[0], out);
  // The cast above already carries the callee's prototype when a header
  // covers it (see CContext::calleeType); what it cannot carry is the
  // callee's *name*, since the target stays a computed expression either
  // way. Naming it in a comment is the conservative half of Phase 3.2 (see
  // docs/10-import-resolution.md): a reader gets `write` for free without
  // this becoming a claim that the call is direct when the code shows that
  // it is not.
  const std::optional<std::string> imported =
      analysis::importNameThroughSlot(ctx_.function, ctx_.options.memory, operands[0]);
  const std::string note = imported.has_value() ? std::format(" /* import: {} */", *imported) : "";
  line(out, std::format("{}(({}){})({}){};{}", assignee, prototype, target, args, suffix, note));
}

/// The function-pointer type a computed call is cast through.
///
/// Without a header there is nothing to say beyond the machine's own answer:
/// every argument register is a word, and the result is one if the code kept
/// it. A header replaces exactly the parts it covers -- the parameters it
/// declares, and the return type when the code did keep a result, because a
/// prototype saying `void` over a body that uses the return value is a
/// prototype for a different function and the body is the evidence.
std::string StmtPrinter::calleeCast(const types::TypeEntry* callee,
                                    std::size_t argCount, bool hasResult) {
  std::string returns = hasResult ? "uint64_t" : "void";
  if (callee != nullptr && hasResult && ctx_.binder()->registerShaped(callee->returnType) &&
      ctx_.options.types->format(callee->returnType) != "void") {
    returns = ctx_.spell(callee->returnType);
  }
  std::string out = returns + " (*)(";
  for (std::size_t index = 0; index < argCount; ++index) {
    out += index == 0 ? "" : ", ";
    const bool declared = callee != nullptr && index < callee->params.size() &&
                          ctx_.binder()->registerShaped(callee->params[index].type);
    out += declared ? ctx_.spell(callee->params[index].type) : "uint64_t";
  }
  // `(void)` only on a header's authority. Empty parentheses are C's "nothing
  // is being said about the arguments", which is the truth about a call whose
  // arity was recovered from an empty register set and not from a declaration.
  if (argCount == 0 && callee != nullptr && callee->params.empty()) {
    out += "void";
  }
  return out + ")";
}

void StmtPrinter::printIntrinsic(il::OpId opId, const il::Op& op, std::string& out) {
  if (ctx_.function.nameOf(op.payload) == passes::kSyscallIntrinsic &&
      printSyscall(op, out)) {
    return;
  }
  if (ctx_.function.nameOf(op.payload) == "aarch64.store_exclusive_status" &&
      printExclusiveStore(opId, op, out)) {
    return;
  }
  if (ctx_.function.nameOf(op.payload) == "aarch64.cas" && printCas(op, out)) {
    return;
  }
  if (ctx_.options.securityHintsAsComments && printSecurityHint(op, out)) {
    return;
  }
  const auto operands = ctx_.function.operands(op);
  ctx_.helpers.insert("intrin");
  std::string args;
  for (std::size_t index = 0; index < operands.size(); ++index) {
    args += index == 0 ? "" : ", ";
    args += exprText(operands[index], out);
  }
  const std::string call =
      std::format("__xdec_intrin_{}({})", ctx_.function.nameOf(op.payload), args);
  if (const std::string* temp = ctx_.tempFor(op.result)) {
    line(out, std::format("{} = {};", *temp, call));
  } else {
    line(out, call + ";");
  }
}

bool StmtPrinter::printSyscall(const il::Op& op, std::string& out) {
  const auto operands = ctx_.function.operands(op);
  if (operands.size() <= passes::kSyscallNumberOperand) {
    // Lifted without the ABI attached, so there is no x8 to read and nothing
    // here is better informed than the generic form.
    return false;
  }

  uint64_t number = 0;
  const bool known =
      ctx_.function.asConstantThroughCasts(operands[passes::kSyscallNumberOperand], number);
  const types::SyscallTable* table = ctx_.options.syscalls;
  const types::SyscallInfo* info =
      known && table != nullptr ? table->find(static_cast<uint32_t>(number)) : nullptr;

  // Every argument text is produced before the call line is written, because
  // printing one can emit a preceding statement for a shared subexpression.
  const std::size_t available = operands.size() - passes::kSyscallFirstArgOperand;
  const std::size_t count =
      info == nullptr ? available : std::min<std::size_t>(available, info->argCount);
  std::vector<std::string> args;
  std::vector<bool> addressed;
  args.reserve(count);
  addressed.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const il::ExprId operand = operands[passes::kSyscallFirstArgOperand + index];
    std::string address = ctx_.addressOfLocal(operand);
    addressed.push_back(!address.empty());
    args.push_back(address.empty() ? exprText(operand, out) : std::move(address));
  }
  const std::string numberText =
      known ? std::string{} : exprText(operands[passes::kSyscallNumberOperand], out);

  std::string callee;
  std::string arguments;
  if (info != nullptr) {
    // `sys_` rather than the bare name: this is the kernel entry point, which
    // returns -errno where the libc function of the same name returns -1 and
    // sets errno. Code that reads the result tells the two apart, and a reader
    // who takes one for the other misreads every error path that follows.
    callee = std::format("sys_{}", info->name);
    for (std::size_t index = 0; index < args.size(); ++index) {
      arguments += index == 0 ? "" : ", ";
      // An already-named address needs no cast: the declaration it points at
      // (see applyImportedTypes) is why the address is nameable at all.
      arguments += !addressed[index] && info->hasSignature() && index < info->argTypes.size()
                       ? std::format("({}){}", info->argTypes[index], args[index])
                       : args[index];
    }
    ctx_.useSyscall(*info);
  } else {
    // Unknown number, or a number the table does not list. The number itself is
    // still the most useful thing on the line, so it leads.
    callee = "__xdec_syscall";
    ctx_.helpers.insert("syscall");
    arguments = known ? std::format("{}", number) : numberText + " /* x8 */";
    for (const std::string& arg : args) {
      arguments += ", " + arg;
    }
  }

  const std::string suffix =
      info != nullptr && info->noreturn ? " /* does not return */" : "";
  if (const std::string* temp = ctx_.tempFor(op.result)) {
    line(out, std::format("{} = {}({});{}", *temp, callee, arguments, suffix));
  } else {
    line(out, std::format("{}({});{}", callee, arguments, suffix));
  }
  return true;
}

bool StmtPrinter::printExclusiveStore(il::OpId opId, const il::Op& op, std::string& out) {
  const auto found = ctx_.exclusiveStoreFor.find(opId.index());
  if (found == ctx_.exclusiveStoreFor.end()) {
    return false;
  }
  const il::Op& store = ctx_.function.op(found->second);
  const auto storeOperands = ctx_.function.operands(store);
  const uint32_t width = store.type.bits();
  ctx_.helpers.insert(std::format("stlxr{}", width));
  const std::string value = assignedText(storeOperands[1], out);
  const std::string address = exprPointer(storeOperands[0], width, out);
  const std::string call = std::format("__stlxr{}({}, {})", width, value, address);
  if (const std::string* temp = ctx_.tempFor(op.result)) {
    line(out, std::format("{} = {};", *temp, call));
  } else {
    line(out, call + ";");
  }
  return true;
}

bool StmtPrinter::printCas(const il::Op& op, std::string& out) {
  const auto operands = ctx_.function.operands(op);
  if (operands.size() != 3) {
    // Not the shape specs/arm64/loadstore.xspec's cas/casa/casl/casal rules
    // produce (address, expected, new) -- lifted without the ABI attached,
    // say, the same caveat printSyscall makes for a bare svc.
    return false;
  }
  const uint32_t width = op.type.bits();
  const std::string address = exprPointer(operands[0], width, out);
  const std::string expected = assignedText(operands[1], out);
  const std::string desired = assignedText(operands[2], out);
  const std::string* temp = ctx_.tempFor(op.result);
  const std::string oldValue = temp == nullptr ? "/*lost*/" : *temp;
  line(out, "/* logical compare-and-swap; not one atomic step in C */");
  line(out, std::format("{} = *{};", oldValue, address));
  line(out, std::format("if ({} == {}) {{", expected, oldValue));
  ++indent_;
  line(out, std::format("*{} = {};", address, desired));
  --indent_;
  line(out, "}");
  return true;
}

bool StmtPrinter::printSecurityHint(const il::Op& op, std::string& out) {
  const std::string_view name = ctx_.function.nameOf(op.payload);
  const auto operands = ctx_.function.operands(op);
  if (name == "aarch64.bti") {
    uint64_t variant = 0;
    if (operands.empty() || !ctx_.function.asConstant(operands[0], variant)) {
      return false;
    }
    static constexpr std::array<std::string_view, 4> kVariants{"", " c", " j", " jc"};
    line(out, std::format("/* BTI{} */", variant < kVariants.size() ? kVariants[variant] : ""));
    return true;
  }
  if (name == "aarch64.pac.ia" || name == "aarch64.pac.ib") {
    line(out, std::format("/* PAC: sign with key {} */", name.back() == 'a' ? 'A' : 'B'));
    return true;
  }
  if (name == "aarch64.aut.ia" || name == "aarch64.aut.ib") {
    line(out, std::format("/* PAC: authenticate with key {} */", name.back() == 'a' ? 'A' : 'B'));
    return true;
  }
  return false;
}

std::string StmtPrinter::memoryLvalue(il::ExprId address, uint32_t width,
                                      std::string& out, il::ValueId result) {
  // A field of a struct a header described, where one describes this
  // access -- checked ahead of AddressRenderer because it answers a
  // different question (an argument-plus-offset's declared shape) that a
  // StackSlot/Global address never has an opinion on either way (see
  // CContext::fieldAccess).
  if (std::string field = ctx_.fieldAccess(address, width, result); !field.empty()) {
    return field;
  }
  if (const auto rendered = AddressRenderer(ctx_).render(address, AddressRole::MemoryLvalue, width)) {
    return rendered->text;
  }
  // A bare pointer (an argument, a typed field) needs no round trip through
  // an integer just to be dereferenced: `exprPointer` already casts it where
  // it is not one, the same cast `assignedInt` would have produced here, so
  // nothing this could dereference is any less honest for skipping it.
  return std::format("(*{})", exprPointer(address, width, out));
}

std::string StmtPrinter::registerRead(il::RegId reg) {
  const il::RegId root = ctx_.function.registers().rootOf(reg);
  const RegVar& variable = ctx_.registerVariable(root);
  const il::RegisterInfo& info = ctx_.function.registers()[reg];
  if (!info.isSubRegister()) {
    return variable.name;
  }
  const std::string window =
      info.offsetInParent == 0
          ? variable.name
          : std::format("({} >> {})", variable.name, info.offsetInParent);
  return std::format("(({})({}))", intType(info.bits), window);
}

std::string StmtPrinter::registerWrite(il::RegId reg, const std::string& value) {
  const il::RegId root = ctx_.function.registers().rootOf(reg);
  const RegVar& variable = ctx_.registerVariable(root);
  const il::RegisterInfo& info = ctx_.function.registers()[reg];
  const std::string rootType = intType(variable.width);
  if (!info.isSubRegister()) {
    return std::format("{} = ({})({});", variable.name, rootType, value);
  }
  if (info.zeroExtendsParent) {
    // The view's write clears the rest of the register, so the insert is a
    // plain assignment of the widened value.
    return std::format("{} = ({})({});", variable.name, rootType, value);
  }
  const std::string mask = lowMask(info.bits, rootType);
  const std::string shifted =
      info.offsetInParent == 0
          ? std::format("(({})({}) & {})", rootType, value, mask)
          : std::format("((({})({}) & {}) << {})", rootType, value, mask,
                        info.offsetInParent);
  const std::string cleared =
      info.offsetInParent == 0
          ? std::format("({} & ~({}){})", variable.name, rootType, mask)
          : std::format("({} & ~((({}){}) << {}))", variable.name, rootType, mask,
                        info.offsetInParent);
  return std::format("{} = {} | {};", variable.name, cleared, shifted);
}

// ---------------------------------------------------------------------------
// Edges
// ---------------------------------------------------------------------------

std::vector<StmtPrinter::EdgeCopy> StmtPrinter::edgeCopies(il::BlockId from,
                                                          il::BlockId to) const {
  std::vector<EdgeCopy> copies;
  if (!from.valid() || !to.valid()) {
    return copies;
  }
  const il::Block& target = ctx_.function.block(to);
  const auto& predecessors = target.predecessors;
  const auto slot = std::find(predecessors.begin(), predecessors.end(), from);
  if (slot == predecessors.end()) {
    return copies;
  }
  // One copy per phi, even when the edge appears twice in the predecessor
  // list: both occurrences carry the same value along the same transfer.
  const auto index = static_cast<std::size_t>(slot - predecessors.begin());
  for (const il::OpId opId : target.ops) {
    const il::Op& op = ctx_.function.op(opId);
    if (op.code != il::OpCode::Phi) {
      break;  // phis lead the block
    }
    if (!op.result.valid()) {
      continue;
    }
    const analysis::Variable* temp = ctx_.variables.tempFor(op.result);
    const auto operands = ctx_.function.operands(op);
    if (temp == nullptr || index >= operands.size()) {
      continue;
    }
    copies.push_back(
        EdgeCopy{op.result, temp->name, temp->type.format(), operands[index]});
  }
  return copies;
}

void StmtPrinter::printEdge(il::BlockId from, il::BlockId to, std::string& out) {
  std::vector<EdgeCopy> copies = edgeCopies(from, to);
  if (copies.empty()) {
    return;
  }
  // A handler's own save into the active frame's merge: whichever slots
  // `classifyHandlerExit` says this handler leaves alone already hold the
  // right value there, seeded by `printFrameSeed` before the switch and
  // never touched by any other case's own copies (each is its own C
  // assignment statement, guarded by its own `case`). Printing them again
  // here would say nothing the reader does not already know from every
  // other passthrough handler saying the same thing.
  if (activeFrame_ && to == activeFrame_->merge) {
    const analysis::DispatcherShape shape{il::BlockId{}, activeFrame_->merge, il::BlockId{}};
    const analysis::HandlerFrameExit exit =
        analysis::classifyHandlerExit(ctx_.function, from, shape, activeFrame_->frame);
    if (exit.kind != analysis::HandlerExitKind::Return) {
      const auto isSuppressed = [&](const EdgeCopy& copy) {
        for (std::size_t slot = 0; slot < activeFrame_->frame.slots.size(); ++slot) {
          if (!exit.unchanged[slot]) {
            continue;
          }
          const il::OpId shadowPhi = activeFrame_->frame.slots[slot].shadowPhiAtMerge;
          if (copy.destination == ctx_.function.op(shadowPhi).result) {
            return true;
          }
        }
        return false;
      };
      copies.erase(std::remove_if(copies.begin(), copies.end(), isSuppressed), copies.end());
      if (copies.empty()) {
        return;
      }
    }
  }
  // The frame's own restore, back out of the merge into the hub: a
  // `unanimous` slot's shadow phi is, by construction, always exactly the
  // live phi's own value (every handler that reaches merge proved as much),
  // so this copy is `live[i] = live[i]` on every single call -- not merely
  // redundant with something else printed nearby, but never anything other
  // than a no-op, on any path, seeded or not.
  if (activeFrame_ && from == activeFrame_->merge) {
    const auto isIdentityRestore = [&](const EdgeCopy& copy) {
      for (std::size_t slot = 0; slot < activeFrame_->unanimous.size(); ++slot) {
        if (!activeFrame_->unanimous[slot]) {
          continue;
        }
        const il::OpId livePhi = activeFrame_->frame.slots[slot].livePhiAtHub;
        if (copy.destination == ctx_.function.op(livePhi).result) {
          return true;
        }
      }
      return false;
    };
    copies.erase(std::remove_if(copies.begin(), copies.end(), isIdentityRestore), copies.end());
    if (copies.empty()) {
      return;
    }
  }
  // Parallel semantics: a source reading a DIFFERENT copy's destination on
  // this same edge must read the value from before the edge, so that
  // destination is snapshotted. A copy reading its OWN destination (the
  // ordinary loop-counter shape, `t26 = t26 - 0x10`) needs no snapshot: one C
  // assignment statement already reads the old value before writing the new
  // one, and no sibling copy can observe anything in between.
  std::vector<std::set<uint32_t>> leavesOf(copies.size());
  for (std::size_t index = 0; index < copies.size(); ++index) {
    collectValueLeaves(ctx_.function, copies[index].source, leavesOf[index]);
  }
  std::vector<const EdgeCopy*> conflicting;
  for (std::size_t index = 0; index < copies.size(); ++index) {
    for (std::size_t other = 0; other < copies.size(); ++other) {
      if (other != index && leavesOf[other].contains(copies[index].destination.index())) {
        conflicting.push_back(&copies[index]);
        break;
      }
    }
  }

  const bool scoped = !conflicting.empty();
  if (scoped) {
    line(out, "{");
    ++indent_;
    for (const EdgeCopy* copy : conflicting) {
      const std::string snapshot = copy->name + "__prev";
      line(out, std::format("const {} {} = {};", copy->type, snapshot, copy->name));
      ctx_.snapshots.emplace(copy->destination.index(), snapshot);
    }
  }
  // One shared scope across every copy on this edge: they always run
  // together, so a phi source two of them repeat is worth naming once.
  std::vector<il::ExprId> roots;
  roots.reserve(copies.size());
  for (const EdgeCopy& copy : copies) {
    roots.push_back(copy.source);
  }
  expressions_.beginScope(roots);
  // A dispatcher merge with several argument-register phis often finds this
  // predecessor never set up more than one or two of them (see
  // ssa_construct.cpp's own note on why the edge contributes Undef here
  // rather than the value being wrong): one `/*undef*/0` line per register
  // says the same thing seven times over. A run of them that are not
  // `conflicting` -- and an Undef source, having no leaves of its own, never
  // is -- collapses into one chained assignment instead, with the same value
  // (0) landing in the same destinations either way.
  const auto isUndef = [this](const EdgeCopy& copy) {
    return ctx_.function.expr(copy.source).op == il::ExprOp::Undef;
  };
  const auto isConflicting = [&conflicting](const EdgeCopy& copy) {
    return std::find(conflicting.begin(), conflicting.end(), &copy) != conflicting.end();
  };
  for (std::size_t index = 0; index < copies.size();) {
    if (isUndef(copies[index]) && !isConflicting(copies[index])) {
      std::size_t end = index + 1;
      while (end < copies.size() && isUndef(copies[end]) && !isConflicting(copies[end])) {
        ++end;
      }
      std::string assignees;
      std::string names;
      for (std::size_t i = index; i < end; ++i) {
        assignees += copies[i].name + " = ";
        names += (i == index ? "" : ", ") + copies[i].name;
      }
      line(out, std::format("{}0; /* {}: never set up on this edge */", assignees, names));
      index = end;
      continue;
    }
    const EdgeCopy& copy = copies[index];
    const auto assign = [&copy](const std::string& arm) {
      return std::format("{} = {};", copy.name, arm);
    };
    if (!printSelectAssign(copy.source, out, assign)) {
      line(out, assign(assignedText(copy.source, out)));
    }
    ++index;
  }
  if (scoped) {
    ctx_.snapshots.clear();
    --indent_;
    line(out, "}");
  }
}

void StmtPrinter::consumePending(il::BlockId to, std::string& out) {
  if (!pending_.valid()) {
    return;
  }
  const il::BlockId from = pending_;
  pending_ = il::BlockId{};
  printEdge(from, to, out);
}

il::BlockId StmtPrinter::firstBlockOf(const Stmt* stmt) const {
  if (stmt == nullptr) {
    return il::BlockId{};
  }
  switch (stmt->kind) {
    case StmtKind::Block:
    case StmtKind::Goto:
    case StmtKind::Switch:
      return stmt->block;
    case StmtKind::While:
    case StmtKind::DoWhile:
      return stmt->block.valid() ? stmt->block : firstBlockOf(stmt->body.get());
    case StmtKind::Sequence:
      for (const StmtPtr& item : stmt->items) {
        if (const il::BlockId found = firstBlockOf(item.get()); found.valid()) {
          return found;
        }
      }
      return il::BlockId{};
    case StmtKind::If:
      return il::BlockId{};  // the head block precedes the if, not inside it
    case StmtKind::Continue:
    case StmtKind::Break:
      return il::BlockId{};  // neither begins with a block of its own
  }
  return il::BlockId{};
}

const il::Op* StmtPrinter::terminatorOf(il::BlockId block) const {
  if (!block.valid()) {
    return nullptr;
  }
  const auto& ops = ctx_.function.block(block).ops;
  if (ops.empty()) {
    return nullptr;
  }
  const il::Op& last = ctx_.function.op(ops.back());
  return last.isTerminator() ? &last : nullptr;
}

std::string StmtPrinter::label(il::BlockId block) const {
  return std::format("L_0x{:x}", ctx_.function.block(block).va);
}

}  // namespace xdec::emit
