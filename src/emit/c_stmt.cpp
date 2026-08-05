// StmtPrinter (see the header for where phi copies go and why).
#include "c_stmt.h"

#include <algorithm>
#include <format>
#include <set>
#include <vector>

#include "xdec/passes/recover_syscall.h"
#include "xdec/types/syscall_table.h"

namespace xdec::emit {

namespace {

/// Every Value leaf under `root`, iteratively: expression trees here are DAGs
/// deep enough that recursion is a stack-overflow risk on obfuscated input.
void collectValueLeaves(const il::Function& function, il::ExprId root,
                        std::set<uint32_t>& out) {
  std::vector<il::ExprId> stack{root};
  std::set<uint32_t> seen;
  while (!stack.empty()) {
    const il::ExprId id = stack.back();
    stack.pop_back();
    if (!seen.insert(id.index()).second) {
      continue;
    }
    const il::Expr& expr = function.expr(id);
    if (expr.op == il::ExprOp::Value) {
      out.insert(static_cast<uint32_t>(expr.immediate));
      continue;
    }
    for (unsigned index = 0; index < expr.operandCount; ++index) {
      stack.push_back(expr.operands[index]);
    }
  }
}

/// The low `width` bits as a C constant, in a form that works past 64 bits.
[[nodiscard]] std::string lowMask(uint32_t width, const std::string& type) {
  if (width >= 64) {
    return std::format("(((({})1) << {}) - 1)", type, width);
  }
  return std::format("0x{:x}", (uint64_t{1} << width) - 1);
}

/// Every expression `printOp` will hand to the expression printer for `op`,
/// so a block's ops can be counted for shared subexpressions before any of
/// them is printed. Mirrors printOp's own dispatch; an op this misses just
/// never gets to share across statements, so erring towards including one
/// costs nothing beyond a wasted count.
void addExprRoots(const il::Function& function, const il::Op& op,
                  std::vector<il::ExprId>& roots) {
  const auto operands = function.operands(op);
  switch (op.code) {
    case il::OpCode::Load:
      if (!operands.empty()) {
        roots.push_back(operands[0]);
      }
      break;
    case il::OpCode::Store:
    case il::OpCode::Call:
    case il::OpCode::Intrinsic:
      roots.insert(roots.end(), operands.begin(), operands.end());
      break;
    case il::OpCode::WriteReg:
    case il::OpCode::Return:
      if (!operands.empty()) {
        roots.push_back(operands[0]);
      }
      break;
    default:
      break;
  }
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
      for (const StmtPtr& item : stmt->items) {
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
  std::string condition = assignedText(stmt.cond, out);
  if (stmt.invertCond) {
    condition = std::format("!({})", condition);
  }
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

void StmtPrinter::printSwitch(const Stmt& stmt, std::string& out) {
  if (stmt.block.valid()) {
    consumePending(stmt.block, out);
    if (ctx_.structured.isLabeled(stmt.block)) {
      // A compare-chain switch replaces its dispatcher blocks, so a handler
      // branching back to the dispatcher lands on this label.
      out += std::format("{}:\n", label(stmt.block));
    }
  }
  pending_ = il::BlockId{};

  const bool enumerated = stmt.tableMode || !stmt.caseValues.empty();
  if (!enumerated) {
    // Nothing matched a table or a chain: an honest compare sequence over the
    // computed target, which at least names every successor.
    expressions_.beginScope({stmt.cond});
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
    return;
  }

  expressions_.beginScope({stmt.cond});
  const std::string discriminant = assignedText(stmt.cond, out);
  line(out, std::format("switch ({}) {{", discriminant));
  ++indent_;
  for (std::size_t index = 0; index < stmt.cases.size(); ++index) {
    if (stmt.tableMode) {
      line(out, std::format("case {}:", index));
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
}

void StmtPrinter::printBlock(const Stmt& stmt, std::string& out) {
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
  std::vector<il::ExprId> roots;
  for (const il::OpId opId : block.ops) {
    addExprRoots(ctx_.function, ctx_.function.op(opId), roots);
  }
  expressions_.beginScope(roots);
  for (const il::OpId opId : block.ops) {
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

void StmtPrinter::printOp(il::OpId opId, std::string& out) {
  const il::Op& op = ctx_.function.op(opId);
  // Ahead of the statement, on its own line: what a pass established about this
  // op but could not write as code — an indirect call it recognised as a table
  // dispatch, say. It goes before rather than trailing because the statements
  // these land on are the long ones, and a comment at the end of a 300-column
  // line is a comment nobody reads.
  if (const std::string_view note = ctx_.function.noteOn(opId); !note.empty()) {
    line(out, std::format("/* {} */", note));
  }
  const auto operands = ctx_.function.operands(op);
  switch (op.code) {
    case il::OpCode::Load: {
      const std::string* temp = ctx_.tempFor(op.result);
      const std::string lvalue =
          memoryLvalue(operands[0], op.type.bits(), out, op.result);
      line(out, std::format("{} = {};", temp == nullptr ? "/*lost*/" : *temp, lvalue));
      break;
    }
    case il::OpCode::Store: {
      // The lvalue first, always: it can hoist a temporary of its own, and one
      // hoisted under a guard would be missing on the path that skips it.
      const std::string lvalue = memoryLvalue(operands[0], op.type.bits(), out);
      const auto assign = [&lvalue](const std::string& arm) {
        return std::format("{} = {};", lvalue, arm);
      };
      if (!printSelectAssign(operands[1], out, assign)) {
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
      printIntrinsic(op, out);
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
  std::string args;
  for (std::size_t index = 1; index <= argCount; ++index) {
    args += index == 1 ? "" : ", ";
    args += exprText(operands[index], out);
  }
  const std::string suffix = trimmed ? " /* + unknown arg(s) */" : "";
  const std::string* temp = ctx_.tempFor(op.result);
  const std::string assignee = temp == nullptr ? "" : *temp + " = ";
  uint64_t address = 0;
  if (ctx_.function.asConstant(operands[0], address)) {
    const std::string callee = ctx_.calleeName(address);
    ctx_.callees.emplace(address, callee);
    line(out, std::format("{}{}({}){};", assignee, callee, args, suffix));
    return;
  }
  const std::string prototype = calleeCast(ctx_.calleeType(operands[0]), argCount,
                                           temp != nullptr);
  const std::string target = exprInt(operands[0], out);
  line(out, std::format("{}(({}){})({}){};", assignee, prototype, target, args, suffix));
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

void StmtPrinter::printIntrinsic(const il::Op& op, std::string& out) {
  if (ctx_.function.nameOf(op.payload) == passes::kSyscallIntrinsic &&
      printSyscall(op, out)) {
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
  args.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    args.push_back(exprText(operands[passes::kSyscallFirstArgOperand + index], out));
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
      arguments += info->hasSignature() && index < info->argTypes.size()
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

std::string StmtPrinter::memoryLvalue(il::ExprId address, uint32_t width,
                                      std::string& out, il::ValueId result) {
  const analysis::AddressInfo info = ctx_.frame.classify(address);
  if (info.kind == analysis::AddressKind::StackSlot) {
    if (const analysis::Variable* local = ctx_.variables.localAt(info.delta)) {
      if (local->type.pointerDepth == 0 && local->type.width == width) {
        return local->name;
      }
      return std::format("(*({}*)(&{}))", intType(width), local->name);
    }
  }
  // A field of a struct a header described, where one describes this access.
  if (std::string field = ctx_.fieldAccess(address, width, result); !field.empty()) {
    return field;
  }
  // A fixed address is a global, and prints as one where the image can say that
  // much. The cast stays: the same global is read at several widths in this code,
  // so a declaration cannot carry the type and the access has to.
  uint64_t absolute = 0;
  if (ctx_.function.asConstant(address, absolute)) {
    if (const std::string* global = ctx_.globalName(absolute)) {
      return std::format("(*({}*)({}))", intType(width), *global);
    }
  }
  return std::format("(*({}*)({}))", intType(width), assignedInt(address, out));
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
  const std::vector<EdgeCopy> copies = edgeCopies(from, to);
  if (copies.empty()) {
    return;
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
  for (const EdgeCopy& copy : copies) {
    const auto assign = [&copy](const std::string& arm) {
      return std::format("{} = {};", copy.name, arm);
    };
    if (!printSelectAssign(copy.source, out, assign)) {
      line(out, assign(assignedText(copy.source, out)));
    }
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
      return il::BlockId{};  // it jumps to a loop header it does not begin with
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
