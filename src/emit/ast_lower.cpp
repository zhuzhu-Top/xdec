#include "ast_lower.h"

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "address_render.h"
#include "dead_ops.h"
#include "phi_lowering.h"
#include "xdec/analysis/import_callee.h"
#include "xdec/analysis/indexed_transform_loop.h"
#include "xdec/analysis/noreturn.h"
#include "xdec/il/expr_roots.h"
#include "xdec/obs/span.h"
#include "xdec/passes/recover_syscall.h"
#include "xdec/types/syscall_table.h"

namespace xdec::emit {

namespace {

[[nodiscard]] std::string filterOpNote(std::string_view note, bool verboseNotes) {
  if (verboseNotes || note.empty()) {
    return std::string{note};
  }
  std::string out;
  std::size_t pos = 0;
  while (pos < note.size()) {
    const std::size_t sep = note.find("; ", pos);
    const std::string_view part =
        sep == std::string_view::npos ? note.substr(pos) : note.substr(pos, sep - pos);
    pos = sep == std::string_view::npos ? note.size() : sep + 2;
    if (part.starts_with("the imported prototype takes ") ||
        part.ends_with("further register(s) dropped") ||
        part.starts_with("the loader fills that slot from ") ||
        part.starts_with("indirect call: target = load(")) {
      continue;
    }
    if (!out.empty()) {
      out += "; ";
    }
    if (part.starts_with("tail call through ")) {
      out += "tailcall";
    } else {
      out += part;
    }
  }
  return out;
}

[[nodiscard]] std::string describeIndexedTransformLoop(const analysis::IndexedTransformLoop& t) {
  std::string expr;
  switch (t.op) {
    case analysis::TransformOp::RotateLeft:
      expr = "rotl(src[i], key)";
      break;
    case analysis::TransformOp::RotateRight:
      expr = "rotr(src[i], key)";
      break;
    default:
      expr = t.loadIsFirstOperand
                 ? std::format("src[i] {} key", analysis::toString(t.op))
                 : std::format("key {} src[i]", analysis::toString(t.op));
      break;
  }
  const std::string start = t.indexStart ? std::format("0x{:x}", *t.indexStart) : "?";
  return std::format(
      "indexed transform loop: dst[i] = {}; i = {}, step {}{}, element size {}", expr, start,
      t.indexStride >= 0 ? "+" : "", t.indexStride, t.elementScale);
}

[[nodiscard]] std::string lowMask(uint32_t width, const std::string& type) {
  if (width >= 64) {
    return std::format("(((({})0x1) << 0x{:x}) - 0x1)", type, width);
  }
  return std::format("0x{:x}", (uint64_t{1} << width) - 1);
}

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

[[nodiscard]] bool implicitUnsignedCallWidening(
    const CContext& ctx, const types::TypeEntry* callee, std::size_t index,
    il::ExprId operand) {
  if (callee == nullptr || callee->variadic || ctx.options.types == nullptr ||
      index >= callee->params.size()) {
    return false;
  }
  const il::Expr& expression = ctx.function.expr(operand);
  if (expression.op != il::ExprOp::ZExt || expression.operandCount != 1) {
    return false;
  }
  const il::Expr& source = ctx.function.expr(expression.operand(0));
  if (!source.type.isScalarInteger()) {
    return false;
  }
  bool sourceIsUnsigned = false;
  if (source.op == il::ExprOp::Const || source.op == il::ExprOp::Trunc ||
      source.op == il::ExprOp::ZExt || source.op == il::ExprOp::CmpEq ||
      source.op == il::ExprOp::CmpNe || source.op == il::ExprOp::CmpLtU ||
      source.op == il::ExprOp::CmpLeU) {
    sourceIsUnsigned = true;
  } else if (source.op == il::ExprOp::EntryReg) {
    const analysis::Variable* argument = ctx.variables.argumentFor(
        il::RegId{static_cast<uint32_t>(source.immediate)});
    sourceIsUnsigned = argument != nullptr && !argument->type.isSigned;
    if (argument != nullptr) {
      const types::TypeId declared = ctx.argumentType(*argument);
      if (declared.valid()) {
        const types::TypeEntry* entry = ctx.options.types->get(
            ctx.options.types->resolveTypedef(declared));
        sourceIsUnsigned =
            entry != nullptr &&
            (entry->kind == types::TypeKind::Bool ||
             (entry->kind == types::TypeKind::Int && !entry->isSigned));
      }
    }
  } else if (source.op == il::ExprOp::Value) {
    const il::ValueId value{static_cast<uint32_t>(source.immediate)};
    if (ctx.function.hasValue(value)) {
      const il::OpId definition = ctx.function.value(value).definition;
      sourceIsUnsigned =
          definition.valid() && ctx.function.op(definition).code == il::OpCode::Load;
    }
    if (const analysis::Variable* temporary = ctx.variables.tempFor(value)) {
      sourceIsUnsigned = sourceIsUnsigned || !temporary->type.isSigned;
    }
    if (const types::TypeId declared = ctx.typeOfValue(value); declared.valid()) {
      const types::TypeEntry* entry = ctx.options.types->get(
          ctx.options.types->resolveTypedef(declared));
      sourceIsUnsigned =
          entry != nullptr &&
          (entry->kind == types::TypeKind::Bool ||
           (entry->kind == types::TypeKind::Int && !entry->isSigned));
    }
  }
  if (!sourceIsUnsigned) {
    return false;
  }
  const types::TypeDatabase& database = *ctx.options.types;
  const types::TypeEntry* destination =
      database.get(database.resolveTypedef(callee->params[index].type));
  if (destination == nullptr ||
      (destination->kind != types::TypeKind::Int &&
       destination->kind != types::TypeKind::Bool &&
       destination->kind != types::TypeKind::Enum) ||
      destination->isSigned) {
    return false;
  }
  const std::optional<uint64_t> size = database.sizeOf(callee->params[index].type);
  return size.has_value() && *size * 8 >= source.type.bits() &&
         expression.type.bits() == *size * 8;
}

class AstLowerer {
 public:
  AstLowerer(CContext& ctx, ExprPrinter& expressions)
      : ctx_(ctx), expressions_(expressions) {}

  [[nodiscard]] HlFunction run() {
    HlFunction fn;
    fn.body = HlStmt::makeSequence();
    lowerStmt(ctx_.structured.root, fn.body->items);
    return fn;
  }

 private:
  struct SelectChain {
    std::vector<std::pair<std::string, std::string>> arms;
    std::string fallback;
  };

  using StmtList = std::vector<HlStmtPtr>;

  void flushPending(StmtList& out) {
    for (std::string& decl : expressions_.takePendingDecls()) {
      if (!decl.empty() && decl.back() == ';') {
        decl.pop_back();
      }
      const std::size_t eq = decl.find(" = ");
      if (eq != std::string::npos) {
        out.push_back(HlStmt::makeAssign(
            HlExpr::makeVar(decl.substr(0, eq)),
            HlExpr::makeRaw(decl.substr(eq + 3))));
      } else {
        out.push_back(HlStmt::makeExprStmt(HlExpr::makeRaw(std::move(decl))));
      }
    }
  }

  [[nodiscard]] std::string exprText(il::ExprId id, StmtList& out) {
    std::string result = expressions_.text(id);
    flushPending(out);
    return result;
  }

  [[nodiscard]] std::string exprInt(il::ExprId id, StmtList& out) {
    std::string result = expressions_.integerOperand(id);
    flushPending(out);
    return result;
  }

  [[nodiscard]] std::string exprPointer(il::ExprId id, uint32_t width, StmtList& out) {
    std::string result = expressions_.pointerOperand(id, width);
    flushPending(out);
    return result;
  }

  [[nodiscard]] std::string assignedText(il::ExprId id, StmtList& out) {
    std::string result = expressions_.rootText(id);
    flushPending(out);
    return result;
  }

  [[nodiscard]] std::string assignedInt(il::ExprId id, StmtList& out) {
    std::string result = expressions_.rootInteger(id);
    flushPending(out);
    return result;
  }

  [[nodiscard]] std::string labelName(il::BlockId block) const {
    return std::format("L_0x{:x}", ctx_.function.block(block).va);
  }

  [[nodiscard]] HlStmtPtr makeSeq(StmtList items) {
    auto s = HlStmt::makeSequence();
    s->items = std::move(items);
    return s;
  }

  [[nodiscard]] bool hasLiveSretX8() const {
    std::vector<uint8_t> seen(ctx_.function.exprCount(), 0);
    std::vector<il::ExprId> stack;
    for (const il::BlockId bid : ctx_.function.blockHandles()) {
      for (const il::OpId oid : ctx_.function.block(bid).ops) {
        const il::Op& op = ctx_.function.op(oid);
        for (const il::ExprId arg : ctx_.function.operands(op)) {
          if (arg.valid()) {
            stack.push_back(arg);
          }
        }
      }
    }
    while (!stack.empty()) {
      const il::ExprId id = stack.back();
      stack.pop_back();
      if (!id.valid() || id.index() >= seen.size() || seen[id.index()] != 0) {
        continue;
      }
      seen[id.index()] = 1;
      const il::Expr& e = ctx_.function.expr(id);
      if (e.op == il::ExprOp::EntryReg) {
        const il::RegId root{static_cast<uint32_t>(e.immediate)};
        if (ctx_.function.registers()[root].name == "x8" &&
            ctx_.variables.argumentFor(root) == nullptr) {
          return true;
        }
      }
      for (unsigned i = 0; i < e.operandCount; ++i) {
        if (e.operands[i].valid()) {
          stack.push_back(e.operands[i]);
        }
      }
    }
    return false;
  }

  void lowerStmt(const StmtPtr& stmt, StmtList& out) {
    switch (stmt->kind) {
      case StmtKind::Sequence:
        for (std::size_t index = 0; index < stmt->items.size(); ++index) {
          const StmtPtr& item = stmt->items[index];
          if (item->kind == StmtKind::Block && index + 1 < stmt->items.size()) {
            const StmtPtr& next = stmt->items[index + 1];
            if (next->kind == StmtKind::Switch && next->block == item->block &&
                next->cond.valid()) {
              std::string assignDiscriminantTo;
              const bool enumerated = next->tableMode || !next->caseValues.empty();
              if (const il::OpId stateStore = deadStateDiscriminantStore(
                      ctx_.function, ctx_.frame, ctx_.variables, enumerated, item->block,
                      next->cond);
                  stateStore.valid()) {
                const auto operands = ctx_.function.operands(ctx_.function.op(stateStore));
                const analysis::AddressInfo address = ctx_.frame.classify(operands[0]);
                if (const analysis::Variable* local = ctx_.variables.localAt(address.delta)) {
                  assignDiscriminantTo = local->name;
                }
              }
              lowerBlock(*item, out, {next->cond});
              lowerSwitch(*next, out, /*openScope=*/false, assignDiscriminantTo);
              ++index;
              continue;
            }
          }
          lowerStmt(item, out);
        }
        break;
      case StmtKind::Block:
        lowerBlock(*stmt, out);
        break;
      case StmtKind::If:
        lowerIf(*stmt, out);
        break;
      case StmtKind::While:
        lowerWhile(*stmt, out);
        break;
      case StmtKind::DoWhile:
        lowerDoWhile(*stmt, out);
        break;
      case StmtKind::Switch:
        lowerSwitch(*stmt, out);
        break;
      case StmtKind::Goto:
        consumePending(stmt->block, out);
        out.push_back(HlStmt::makeGoto(labelName(stmt->block)));
        break;
      case StmtKind::Continue:
        consumePending(stmt->block, out);
        out.push_back(HlStmt::makeSimple(HlStmtKind::Continue));
        break;
      case StmtKind::Break:
        if (stmt->block.valid()) {
          consumePending(stmt->block, out);
        }
        out.push_back(HlStmt::makeSimple(HlStmtKind::Break));
        break;
    }
  }

  void lowerIf(const Stmt& stmt, StmtList& out) {
    const il::BlockId head = last_;
    expressions_.beginScope({stmt.cond});
    const std::string condition = expressions_.condition(stmt.cond, stmt.invertCond);
    flushPending(out);

    il::BlockId taken;
    il::BlockId untaken;
    if (const il::Op* terminator = terminatorOf(ctx_.function, head);
        terminator != nullptr && terminator->code == il::OpCode::CondBranch) {
      const auto targets = ctx_.function.targets(*terminator);
      taken = targets[stmt.invertCond ? 1 : 0];
      untaken = targets[stmt.invertCond ? 0 : 1];
    }
    const il::BlockId thenFirst = firstBlockOf(stmt.thenArm.get());
    const il::BlockId elseFirst = firstBlockOf(stmt.elseArm.get());
    const auto uncovered = [&](il::BlockId claimed) {
      if (taken.valid() && taken != claimed) {
        return taken;
      }
      if (untaken.valid() && untaken != claimed) {
        return untaken;
      }
      return il::BlockId{};
    };

    StmtList thenItems;
    if (stmt.thenArm) {
      pending_ = head;
      lowerStmt(stmt.thenArm, thenItems);
    } else if (const il::BlockId target = uncovered(elseFirst); target.valid()) {
      lowerEdge(head, target, thenItems);
    }

    const il::BlockId elseTarget = uncovered(thenFirst);
    const bool elseCopies =
        !stmt.elseArm && elseTarget.valid() && !collectEdgeCopies(ctx_, head, elseTarget).empty();
    HlStmtPtr elseBody;
    if (stmt.elseArm || elseCopies) {
      StmtList elseItems;
      if (stmt.elseArm) {
        pending_ = head;
        lowerStmt(stmt.elseArm, elseItems);
      } else {
        lowerEdge(head, elseTarget, elseItems);
      }
      elseBody = makeSeq(std::move(elseItems));
    }

    out.push_back(HlStmt::makeIf(HlExpr::makeRaw(condition),
                                 makeSeq(std::move(thenItems)),
                                 std::move(elseBody)));
    pending_ = il::BlockId{};
    last_ = head;
    if (stmt.epilogue) {
      lowerStmt(stmt.epilogue, out);
    }
  }

  void lowerWhile(const Stmt& stmt, StmtList& out) {
    const il::BlockId header = stmt.block;
    consumePending(header, out);
    if (stmt.transform) {
      out.push_back(HlStmt::makeComment(describeIndexedTransformLoop(*stmt.transform)));
    }
    if (!stmt.cond.valid()) {
      pending_ = il::BlockId{};
      StmtList bodyItems;
      lowerStmt(stmt.body, bodyItems);
      out.push_back(HlStmt::makeWhile(nullptr, makeSeq(std::move(bodyItems))));
      return;
    }
    expressions_.beginScope({stmt.cond});
    const std::string condition = expressions_.condition(stmt.cond, stmt.invertCond);
    flushPending(out);

    pending_ = header;
    StmtList bodyItems;
    lowerStmt(stmt.body, bodyItems);
    out.push_back(HlStmt::makeWhile(HlExpr::makeRaw(condition), makeSeq(std::move(bodyItems))));
    pending_ = header;
    last_ = header;
  }

  [[nodiscard]] std::string doWhileCondition(il::ExprId cond, bool invertCond, StmtList& out) {
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

  void lowerDoWhile(const Stmt& stmt, StmtList& out) {
    const il::BlockId header = stmt.block;
    consumePending(header, out);
    if (stmt.transform) {
      out.push_back(HlStmt::makeComment(describeIndexedTransformLoop(*stmt.transform)));
    }
    StmtList bodyItems;
    lowerStmt(stmt.body, bodyItems);
    const il::BlockId latch = last_;
    expressions_.beginScope({stmt.cond});
    const std::string condition = doWhileCondition(stmt.cond, stmt.invertCond, bodyItems);
    if (header.valid() && !collectEdgeCopies(ctx_, latch, header).empty()) {
      StmtList backEdgeItems;
      lowerEdge(latch, header, backEdgeItems);
      bodyItems.push_back(HlStmt::makeIf(HlExpr::makeRaw(condition),
                                         makeSeq(std::move(backEdgeItems))));
    }
    out.push_back(HlStmt::makeDoWhile(makeSeq(std::move(bodyItems)), HlExpr::makeRaw(condition)));
    pending_ = latch;
    last_ = latch;
  }

  void lowerFrameSeed(const analysis::LiveRegisterFrame& frame,
                      const std::vector<bool>& unanimous, StmtList& out) {
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
      out.push_back(HlStmt::makeAssign(HlExpr::makeVar(shadowVar->name),
                                       HlExpr::makeVar(liveVar->name)));
    }
  }

  void lowerSwitch(const Stmt& stmt, StmtList& out, bool openScope = true,
                   const std::string& assignDiscriminantTo = {}) {
    if (stmt.block.valid()) {
      consumePending(stmt.block, out);
      if (ctx_.structured.isLabeled(stmt.block) && last_ != stmt.block) {
        out.push_back(HlStmt::makeLabel(labelName(stmt.block)));
      }
    }
    pending_ = il::BlockId{};

    const std::optional<ActiveFrame> enclosingFrame = activeFrame_;
    if (stmt.frame) {
      const analysis::DispatcherShape shape{il::BlockId{}, stmt.mergeBlock, il::BlockId{}};
      const std::vector<bool> unanimous =
          analysis::unanimousPassthroughSlots(ctx_.function, shape, *stmt.frame);
      lowerFrameSeed(*stmt.frame, unanimous, out);
      activeFrame_ = ActiveFrame{stmt.mergeBlock, *stmt.frame, unanimous};
    }

    const bool enumerated = stmt.tableMode || !stmt.caseValues.empty();
    if (!enumerated) {
      if (openScope) {
        expressions_.beginScope({stmt.cond});
      }
      const std::string target = assignedText(stmt.cond, out);
      HlStmtPtr rootIf;
      HlStmt* tailIf = nullptr;
      for (std::size_t index = 0; index < stmt.cases.size(); ++index) {
        const il::BlockId arm = stmt.cases[index];
        const il::BlockId from = index < stmt.casePreds.size() ? stmt.casePreds[index] : stmt.block;
        StmtList armItems;
        if (from.valid()) {
          lowerEdge(from, arm, armItems);
        }
        if (index < stmt.caseBodies.size() && stmt.caseBodies[index]) {
          pending_ = il::BlockId{};
          lowerStmt(stmt.caseBodies[index], armItems);
        } else {
          armItems.push_back(HlStmt::makeGoto(labelName(arm)));
        }
        HlStmtPtr armIf = HlStmt::makeIf(
            HlExpr::makeRaw(std::format("{} == 0x{:x}", target, ctx_.function.block(arm).va)),
            makeSeq(std::move(armItems)));
        if (!rootIf) {
          rootIf = std::move(armIf);
          tailIf = rootIf.get();
        } else {
          tailIf->elseArm = std::move(armIf);
          tailIf = tailIf->elseArm.get();
        }
      }
      if (rootIf) {
        out.push_back(std::move(rootIf));
      }
      if (stmt.epilogue) {
        pending_ = il::BlockId{};
        lowerStmt(stmt.epilogue, out);
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
    auto sw = std::make_unique<HlStmt>();
    sw->kind = HlStmtKind::Switch;
    sw->cond = assignDiscriminantTo.empty()
                   ? HlExpr::makeRaw(discriminant)
                   : HlExpr::makeBinary("=", HlExpr::makeVar(assignDiscriminantTo),
                                        HlExpr::makeRaw(discriminant));

    for (std::size_t index = 0; index < stmt.cases.size(); ++index) {
      HlSwitchCase c;
      if (stmt.tableMode) {
        c.value = index < stmt.caseValues.size() ? stmt.caseValues[index] : index;
        c.comment = std::format("handler @0x{:x}", ctx_.function.block(stmt.cases[index]).va);
      } else {
        c.value = stmt.caseValues[index];
      }
      StmtList bodyItems;
      if (index < stmt.casePreds.size() && stmt.casePreds[index].valid()) {
        lowerEdge(stmt.casePreds[index], stmt.cases[index], bodyItems);
      }
      if (index < stmt.caseBodies.size() && stmt.caseBodies[index]) {
        pending_ = il::BlockId{};
        lowerStmt(stmt.caseBodies[index], bodyItems);
      } else {
        bodyItems.push_back(HlStmt::makeGoto(labelName(stmt.cases[index])));
      }
      c.body = makeSeq(std::move(bodyItems));
      sw->cases.push_back(std::move(c));
    }

    if (stmt.defaultCase.valid()) {
      HlSwitchCase def;
      def.isDefault = true;
      StmtList bodyItems;
      if (stmt.defaultPred.valid()) {
        lowerEdge(stmt.defaultPred, stmt.defaultCase, bodyItems);
      }
      if (stmt.defaultBody) {
        pending_ = il::BlockId{};
        lowerStmt(stmt.defaultBody, bodyItems);
      } else {
        bodyItems.push_back(HlStmt::makeGoto(labelName(stmt.defaultCase)));
      }
      def.body = makeSeq(std::move(bodyItems));
      sw->cases.push_back(std::move(def));
    }

    out.push_back(std::move(sw));
    if (stmt.epilogue) {
      pending_ = il::BlockId{};
      lowerStmt(stmt.epilogue, out);
    }
    if (stmt.frame) {
      activeFrame_ = enclosingFrame;
    }
  }

  void lowerBlock(const Stmt& stmt, StmtList& out,
                  const std::vector<il::ExprId>& extraRoots = {}) {
    consumePending(stmt.block, out);
    const il::Block& block = ctx_.function.block(stmt.block);
    XDEC_SPAN(blockSpan, "emit", "block", "va", obs::hex(block.va), "ops", block.ops.size());
    if (ctx_.structured.isLabeled(stmt.block)) {
      out.push_back(HlStmt::makeLabel(labelName(stmt.block)));
    }
    if (ctx_.options.annotateBlocks) {
      out.push_back(HlStmt::makeComment(std::format("b{} @0x{:x}", stmt.block.index(), block.va),
                                        /*slashSlash=*/true));
    }

    std::vector<il::ExprId> roots;
    for (const il::OpId opId : block.ops) {
      if (ctx_.deadOps.contains(opId.index())) {
        continue;
      }
      il::addExprRoots(ctx_.function, ctx_.function.op(opId), roots);
    }
    expressions_.beginScope(roots);
    if (!extraRoots.empty()) {
      expressions_.extendScope(extraRoots);
    }
    for (const il::OpId opId : block.ops) {
      if (ctx_.deadOps.contains(opId.index())) {
        continue;
      }
      lowerOp(opId, out);
    }
    last_ = stmt.block;
    if (const il::Op* terminator = terminatorOf(ctx_.function, stmt.block);
        terminator != nullptr && terminator->code == il::OpCode::Branch) {
      lowerEdge(stmt.block, ctx_.function.targets(*terminator)[0], out);
    }
  }

  [[nodiscard]] bool lowerFoldedImportStore(const il::Op& op, StmtList& out) {
    const std::optional<il::ValueId> value = importAccessorStoreValue(ctx_.function, op);
    if (!value.has_value() || !ctx_.function.hasValue(*value)) {
      return false;
    }
    const il::OpId definition = ctx_.function.value(*value).definition;
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
    auto callExpr = HlExpr::makeCall(HlExpr::makeVar(callee), {});
    callExpr->isDirectCall = true;
    auto lhs = HlExpr::makeUnary("*", std::move(callExpr));
    out.push_back(
        HlStmt::makeAssign(std::move(lhs), HlExpr::makeRaw(assignedText(operands[1], out))));
    return true;
  }

  [[nodiscard]] bool lowerFoldedStringStore(il::OpId opId, StmtList& out) {
    const auto found = ctx_.foldableStringStores.find(opId.index());
    if (found == ctx_.foldableStringStores.end()) {
      return false;
    }
    std::vector<HlExprPtr> args;
    args.push_back(HlExpr::makeRaw(found->second.address));
    args.push_back(HlExpr::makeStringLit(found->second.text));
    auto callExpr = HlExpr::makeCall(HlExpr::makeVar("strcpy"), std::move(args));
    callExpr->isDirectCall = true;
    out.push_back(HlStmt::makeExprStmt(std::move(callExpr)));
    return true;
  }

  void lowerOp(il::OpId opId, StmtList& out) {
    const il::Op& op = ctx_.function.op(opId);
    if (op.code != il::OpCode::Phi) {
      if (const std::string note =
              filterOpNote(ctx_.function.noteOn(opId), ctx_.options.verboseNotes);
          !note.empty()) {
        out.push_back(HlStmt::makeComment(note));
      }
    }
    const auto operands = ctx_.function.operands(op);
    switch (op.code) {
      case il::OpCode::Load: {
        const std::string* temp = ctx_.tempFor(op.result);
        std::string lvalue;
        if (ctx_.exclusiveLoads.contains(opId.index())) {
          const uint32_t width = op.type.bits();
          ctx_.helpers.insert(std::format("ldaxr{}", width));
          lvalue = std::format("__ldaxr{}({})", width, exprPointer(operands[0], width, out));
        } else {
          lvalue = memoryLvalue(operands[0], op.type.bits(), out, op.result);
        }
        out.push_back(HlStmt::makeAssign(
            HlExpr::makeVar(temp == nullptr ? "/*lost*/" : *temp),
            HlExpr::makeRaw(std::move(lvalue))));
        break;
      }
      case il::OpCode::Store: {
        if (lowerFoldedImportStore(op, out) || lowerFoldedStringStore(opId, out)) {
          break;
        }
        const std::string lvalue = memoryLvalue(operands[0], op.type.bits(), out);
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
        const auto assignArm = [&](const std::string& arm) {
          return HlStmt::makeAssign(
              HlExpr::makeRaw(lvalue),
              castPrefix.empty() ? HlExpr::makeRaw(arm) : HlExpr::makeRaw(castPrefix + arm));
        };
        bool folded = false;
        if (castPrefix.empty() && storeLocal != nullptr && lvalue == storeLocal->name) {
          if (const auto initializer = expressions_.materializeAs(operands[1], lvalue)) {
            out.push_back(assignArm(*initializer));
            folded = true;
          }
        }
        if (!folded && !lowerSelectAssign(operands[1], out, assignArm)) {
          out.push_back(assignArm(assignedText(operands[1], out)));
        }
        break;
      }
      case il::OpCode::ReadReg: {
        const std::string* temp = ctx_.tempFor(op.result);
        out.push_back(HlStmt::makeAssign(
            HlExpr::makeVar(temp == nullptr ? "/*lost*/" : *temp),
            HlExpr::makeRaw(registerRead(op.reg()))));
        break;
      }
      case il::OpCode::WriteReg: {
        const il::RegId reg = op.reg();
        const auto assignArm = [this, reg](const std::string& arm) -> HlStmtPtr {
          return lowerRegisterWrite(reg, arm);
        };
        if (!lowerSelectAssign(operands[0], out, assignArm)) {
          out.push_back(assignArm(assignedText(operands[0], out)));
        }
        break;
      }
      case il::OpCode::Call:
        lowerCall(opId, op, out);
        break;
      case il::OpCode::Intrinsic:
        lowerIntrinsic(opId, op, out);
        break;
      case il::OpCode::Unimplemented:
        out.push_back(HlStmt::makeSimple(HlStmtKind::Unimplemented,
                                         std::string{ctx_.function.nameOf(op.payload)}));
        break;
      case il::OpCode::Return: {
        const types::TypeEntry* declared = ctx_.prototype();
        const types::TypeId typedReturn = ctx_.functionReturnType();
        const SymbolRef entrySym =
            ctx_.symbolAt(ctx_.function.block(ctx_.function.entryBlock()).va);
        const bool objcVoid =
            entrySym.exact() && !entrySym.typeEncoding.empty() &&
            entrySym.typeEncoding.front() == 'v';
        const bool hasSret = hasLiveSretX8();
        const bool isVoid =
            objcVoid || hasSret ||
            (declared != nullptr &&
             ctx_.options.types->format(declared->returnType) == "void") ||
            (typedReturn.valid() &&
             ctx_.options.types->format(typedReturn) == "void") ||
            (!typedReturn.valid() && !ctx_.variables.returnType().has_value());
        if (isVoid || operands.empty()) {
          out.push_back(HlStmt::makeReturn());
        } else if (!lowerSelectReturn(operands[0], out)) {
          out.push_back(HlStmt::makeReturn(HlExpr::makeRaw(assignedText(operands[0], out))));
        }
        break;
      }
      case il::OpCode::Unreachable:
        out.push_back(HlStmt::makeSimple(HlStmtKind::Unreachable));
        break;
      case il::OpCode::Phi:
      case il::OpCode::Nop:
      case il::OpCode::Branch:
      case il::OpCode::CondBranch:
      case il::OpCode::IndirectBranch:
        break;
      default:
        out.push_back(
            HlStmt::makeComment(std::format("unprinted op: {}", il::toString(op.code))));
        break;
    }
  }

  [[nodiscard]] std::optional<SelectChain> flattenSelect(il::ExprId value, StmtList& out) {
    if (!ctx_.options.preferIfOverTernary) {
      return std::nullopt;
    }
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

  [[nodiscard]] bool lowerSelectReturn(il::ExprId value, StmtList& out) {
    const std::optional<SelectChain> chain = flattenSelect(value, out);
    if (!chain.has_value()) {
      return false;
    }
    for (const auto& [cond, taken] : chain->arms) {
      StmtList body;
      body.push_back(HlStmt::makeReturn(HlExpr::makeRaw(taken)));
      out.push_back(HlStmt::makeIf(HlExpr::makeRaw(cond), makeSeq(std::move(body))));
    }
    out.push_back(HlStmt::makeReturn(HlExpr::makeRaw(chain->fallback)));
    return true;
  }

  [[nodiscard]] bool lowerSelectAssign(
      il::ExprId value, StmtList& out,
      const std::function<HlStmtPtr(const std::string&)>& buildAssign) {
    const std::optional<SelectChain> chain = flattenSelect(value, out);
    if (!chain.has_value()) {
      return false;
    }
    HlStmtPtr rootIf;
    HlStmt* tailIf = nullptr;
    for (const auto& [cond, taken] : chain->arms) {
      StmtList armBody;
      armBody.push_back(buildAssign(taken));
      HlStmtPtr armIf = HlStmt::makeIf(HlExpr::makeRaw(cond), makeSeq(std::move(armBody)));
      if (!rootIf) {
        rootIf = std::move(armIf);
        tailIf = rootIf.get();
      } else {
        tailIf->elseArm = std::move(armIf);
        tailIf = tailIf->elseArm.get();
      }
    }
    StmtList elseBody;
    elseBody.push_back(buildAssign(chain->fallback));
    tailIf->elseArm = makeSeq(std::move(elseBody));
    out.push_back(std::move(rootIf));
    return true;
  }

  [[nodiscard]] HlExprPtr lowerCallArgument(const types::TypeEntry* callee, std::size_t paramIndex,
                                            il::ExprId operand, StmtList& out) {
    if (std::string address = ctx_.addressOfLocal(operand); !address.empty()) {
      return HlExpr::makeRaw(std::move(address));
    }
    if (const uint32_t width = paramPointeeWidth(ctx_, callee, paramIndex); width != 0) {
      const analysis::AddressInfo info = ctx_.frame.classify(operand);
      if (info.kind == analysis::AddressKind::Global) {
        if (const std::optional<analysis::ImageLiteral> literal = ctx_.literals.at(info.address)) {
          return HlExpr::makeStringLit(literal->text);
        }
      }
      if (const auto rendered =
              AddressRenderer(ctx_).render(operand, AddressRole::PointerValue, width)) {
        return HlExpr::makeRaw(rendered->text);
      }
    } else if (ctx_.options.addresses) {
      const analysis::AddressInfo info = ctx_.frame.classify(operand);
      if (info.kind == analysis::AddressKind::Global && info.address >= 0x10000) {
        const AddressFacts facts = ctx_.options.addresses(info.address);
        if (facts.cstringSection) {
          if (const std::optional<analysis::ImageLiteral> literal =
                  ctx_.literals.at(info.address, /*allowEmpty=*/true)) {
            return HlExpr::makeStringLit(literal->text);
          }
        }
        if (facts.cfstringLiteral.has_value()) {
          const std::string* global = ctx_.globalName(info.address);
          const std::string base =
              global != nullptr ? *global : std::format("{:#x}", info.address);
          return HlExpr::makeCFStringLit(base, *facts.cfstringLiteral);
        }
      }
    }
    if (implicitUnsignedCallWidening(ctx_, callee, paramIndex, operand)) {
      return HlExpr::makeRaw(assignedInt(operand, out));
    }
    return HlExpr::makeRaw(exprText(operand, out));
  }

  [[nodiscard]] std::string calleeCast(const types::TypeEntry* callee, std::size_t argCount,
                                       bool hasResult) {
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
    if (argCount == 0 && callee != nullptr && callee->params.empty()) {
      out += "void";
    }
    return out + ")";
  }

  void lowerCall(il::OpId opId, const il::Op& op, StmtList& out) {
    const auto operands = ctx_.function.operands(op);
    const bool recovered = ctx_.function.maturity() >= il::Maturity::Vars;
    std::size_t argCount = operands.size() - 1;
    while (!recovered && argCount > 0 &&
           ctx_.function.expr(operands[argCount]).op == il::ExprOp::Undef) {
      --argCount;
    }
    const bool trimmed = argCount + 1 < operands.size();
    const types::TypeEntry* callee = ctx_.calleeType(operands[0]);
    std::vector<HlExprPtr> args;
    args.reserve(argCount);
    for (std::size_t index = 1; index <= argCount; ++index) {
      args.push_back(lowerCallArgument(callee, index - 1, operands[index], out));
    }
    const std::string suffix = trimmed ? " /* + unknown arg(s) */" : "";
    const std::string* temp = ctx_.tempFor(op.result);
    const bool returnsVoid =
        callee != nullptr && ctx_.options.types->format(callee->returnType) == "void";

    const auto emitCallStmt = [&](HlExprPtr callExpr) {
      if (temp != nullptr && !returnsVoid) {
        out.push_back(HlStmt::makeAssign(HlExpr::makeVar(*temp), std::move(callExpr)));
      } else {
        out.push_back(HlStmt::makeExprStmt(std::move(callExpr)));
      }
    };

    uint64_t address = 0;
    if (ctx_.function.asConstant(operands[0], address)) {
      const std::string calleeName = ctx_.calleeName(address);
      ctx_.callees.emplace(address, calleeName);
      auto callExpr = HlExpr::makeCall(HlExpr::makeVar(calleeName), std::move(args));
      callExpr->isDirectCall = true;
      callExpr->constant = address;
      callExpr->isNoreturn = analysis::isKnownNoreturnSymbol(calleeName);
      callExpr->callSuffix = suffix;
      emitCallStmt(std::move(callExpr));
      return;
    }

    const std::optional<std::string> imported =
        analysis::importNameThroughSlot(ctx_.function, ctx_.options.memory, operands[0]);
    if (imported.has_value() && !ctx_.options.verboseNotes) {
      const std::string_view opNote = ctx_.function.noteOn(opId);
      const bool isTailCall = opNote.find("tail call") != std::string_view::npos;
      bool readOnlyGot = false;
      if (ctx_.options.addresses) {
        const il::Expr& e = ctx_.function.expr(operands[0]);
        if (e.op == il::ExprOp::Value) {
          const il::OpId defId =
              ctx_.function.value(il::ValueId{static_cast<uint32_t>(e.immediate)}).definition;
          uint64_t slot = 0;
          if (defId.valid() && ctx_.function.op(defId).code == il::OpCode::Load &&
              ctx_.function.asConstantThroughCasts(
                  ctx_.function.operands(ctx_.function.op(defId))[0], slot)) {
            readOnlyGot = !ctx_.options.addresses(slot).writable;
          }
        }
      }
      if (isTailCall || readOnlyGot) {
        auto callExpr = HlExpr::makeCall(HlExpr::makeVar(*imported), std::move(args));
        callExpr->isDirectCall = true;
        callExpr->importedName = *imported;
        callExpr->isTailCall = isTailCall;
        callExpr->isNoreturn = analysis::isKnownNoreturnSymbol(*imported);
        callExpr->callSuffix = suffix;
        emitCallStmt(std::move(callExpr));
        return;
      }
    }

    const std::string prototype = calleeCast(callee, argCount, temp != nullptr);
    const std::string target = exprInt(operands[0], out);
    std::string note = imported.has_value() ? std::format(" /* import: {} */", *imported) : "";
    if (note.empty()) {
      if (const auto found = ctx_.vtableCalls.find(opId.index()); found != ctx_.vtableCalls.end()) {
        note = std::format(" /* vtable slot {:#x} */", found->second.slotOffset);
      }
    }
    auto callExpr = HlExpr::makeCall(HlExpr::makeRaw(target), std::move(args));
    callExpr->isDirectCall = false;
    callExpr->extra = prototype;
    callExpr->importedName = imported.value_or("");
    callExpr->callSuffix = suffix;
    callExpr->callNote = std::move(note);
    emitCallStmt(std::move(callExpr));
  }

  void lowerIntrinsic(il::OpId opId, const il::Op& op, StmtList& out) {
    if (ctx_.function.nameOf(op.payload) == passes::kSyscallIntrinsic &&
        lowerSyscall(op, out)) {
      return;
    }
    if (ctx_.function.nameOf(op.payload) == "aarch64.store_exclusive_status" &&
        lowerExclusiveStore(opId, op, out)) {
      return;
    }
    if (ctx_.function.nameOf(op.payload) == "aarch64.cas" && lowerCas(op, out)) {
      return;
    }
    if (ctx_.options.securityHintsAsComments && lowerSecurityHint(op, out)) {
      return;
    }
    const auto operands = ctx_.function.operands(op);
    ctx_.helpers.insert("intrin");
    std::vector<HlExprPtr> args;
    args.reserve(operands.size());
    for (const il::ExprId operand : operands) {
      args.push_back(HlExpr::makeRaw(exprText(operand, out)));
    }
    auto callExpr = HlExpr::makeCall(
        HlExpr::makeVar(std::format("__xdec_intrin_{}", ctx_.function.nameOf(op.payload))),
        std::move(args));
    callExpr->isDirectCall = true;
    if (const std::string* temp = ctx_.tempFor(op.result)) {
      out.push_back(HlStmt::makeAssign(HlExpr::makeVar(*temp), std::move(callExpr)));
    } else {
      out.push_back(HlStmt::makeExprStmt(std::move(callExpr)));
    }
  }

  [[nodiscard]] bool lowerSyscall(const il::Op& op, StmtList& out) {
    const auto operands = ctx_.function.operands(op);
    if (operands.size() <= passes::kSyscallNumberOperand) {
      return false;
    }
    uint64_t number = 0;
    const bool known =
        ctx_.function.asConstantThroughCasts(operands[passes::kSyscallNumberOperand], number);
    const types::SyscallTable* table = ctx_.options.syscalls;
    const types::SyscallInfo* info =
        known && table != nullptr ? table->find(static_cast<uint32_t>(number)) : nullptr;

    const std::size_t available = operands.size() - passes::kSyscallFirstArgOperand;
    const std::size_t count =
        info == nullptr ? available : std::min<std::size_t>(available, info->argCount);
    std::vector<std::string> rawArgs;
    std::vector<bool> addressed;
    rawArgs.reserve(count);
    addressed.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      const il::ExprId operand = operands[passes::kSyscallFirstArgOperand + index];
      std::string address = ctx_.addressOfLocal(operand);
      addressed.push_back(!address.empty());
      rawArgs.push_back(address.empty() ? exprText(operand, out) : std::move(address));
    }
    const std::string numberText =
        known ? std::string{} : exprText(operands[passes::kSyscallNumberOperand], out);

    std::string callee;
    std::vector<HlExprPtr> args;
    if (info != nullptr) {
      callee = std::format("sys_{}", info->name);
      for (std::size_t index = 0; index < rawArgs.size(); ++index) {
        const std::string arg =
            !addressed[index] && info->hasSignature() && index < info->argTypes.size()
                ? std::format("({}){}", info->argTypes[index], rawArgs[index])
                : rawArgs[index];
        args.push_back(HlExpr::makeRaw(arg));
      }
      ctx_.useSyscall(*info);
    } else {
      callee = "__xdec_syscall";
      ctx_.helpers.insert("syscall");
      args.push_back(HlExpr::makeRaw(known ? std::format("0x{:x}", number)
                                           : numberText + " /* x8 */"));
      for (const std::string& arg : rawArgs) {
        args.push_back(HlExpr::makeRaw(arg));
      }
    }
    auto callExpr = HlExpr::makeCall(HlExpr::makeVar(std::move(callee)), std::move(args));
    callExpr->isDirectCall = true;
    callExpr->isNoreturn = info != nullptr && info->noreturn;
    if (const std::string* temp = ctx_.tempFor(op.result)) {
      out.push_back(HlStmt::makeAssign(HlExpr::makeVar(*temp), std::move(callExpr)));
    } else {
      out.push_back(HlStmt::makeExprStmt(std::move(callExpr)));
    }
    return true;
  }

  [[nodiscard]] bool lowerExclusiveStore(il::OpId opId, const il::Op& op, StmtList& out) {
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
    std::vector<HlExprPtr> args;
    args.push_back(HlExpr::makeRaw(value));
    args.push_back(HlExpr::makeRaw(address));
    auto callExpr =
        HlExpr::makeCall(HlExpr::makeVar(std::format("__stlxr{}", width)), std::move(args));
    callExpr->isDirectCall = true;
    if (const std::string* temp = ctx_.tempFor(op.result)) {
      out.push_back(HlStmt::makeAssign(HlExpr::makeVar(*temp), std::move(callExpr)));
    } else {
      out.push_back(HlStmt::makeExprStmt(std::move(callExpr)));
    }
    return true;
  }

  [[nodiscard]] bool lowerCas(const il::Op& op, StmtList& out) {
    const auto operands = ctx_.function.operands(op);
    if (operands.size() != 3) {
      return false;
    }
    const uint32_t width = op.type.bits();
    const std::string address = exprPointer(operands[0], width, out);
    const std::string expected = assignedText(operands[1], out);
    const std::string desired = assignedText(operands[2], out);
    const std::string* temp = ctx_.tempFor(op.result);
    const std::string oldValue = temp == nullptr ? "/*lost*/" : *temp;
    out.push_back(HlStmt::makeComment("logical compare-and-swap; not one atomic step in C"));
    out.push_back(HlStmt::makeAssign(HlExpr::makeVar(oldValue),
                                     HlExpr::makeRaw("*" + address)));
    StmtList ifBody;
    ifBody.push_back(HlStmt::makeAssign(HlExpr::makeRaw("*" + address), HlExpr::makeRaw(desired)));
    out.push_back(HlStmt::makeIf(
        HlExpr::makeRaw(std::format("{} == {}", expected, oldValue)),
        makeSeq(std::move(ifBody))));
    return true;
  }

  [[nodiscard]] bool lowerSecurityHint(const il::Op& op, StmtList& out) {
    const std::string_view name = ctx_.function.nameOf(op.payload);
    const auto operands = ctx_.function.operands(op);
    if (name == "aarch64.bti") {
      uint64_t variant = 0;
      if (operands.empty() || !ctx_.function.asConstant(operands[0], variant)) {
        return false;
      }
      static constexpr std::array<std::string_view, 4> kVariants{"", " c", " j", " jc"};
      out.push_back(HlStmt::makeComment(
          std::format("BTI{}", variant < kVariants.size() ? kVariants[variant] : "")));
      return true;
    }
    if (name == "aarch64.pac.ia" || name == "aarch64.pac.ib") {
      out.push_back(HlStmt::makeComment(
          std::format("PAC: sign with key {}", name.back() == 'a' ? 'A' : 'B')));
      return true;
    }
    if (name == "aarch64.aut.ia" || name == "aarch64.aut.ib") {
      out.push_back(HlStmt::makeComment(
          std::format("PAC: authenticate with key {}", name.back() == 'a' ? 'A' : 'B')));
      return true;
    }
    return false;
  }

  [[nodiscard]] std::string memoryLvalue(il::ExprId address, uint32_t width, StmtList& out,
                                         il::ValueId result = {}) {
    if (std::string field = ctx_.fieldAccess(address, width, result); !field.empty()) {
      return field;
    }
    if (const auto rendered =
            AddressRenderer(ctx_).render(address, AddressRole::MemoryLvalue, width)) {
      return rendered->text;
    }
    return std::format("(*{})", exprPointer(address, width, out));
  }

  [[nodiscard]] std::string registerRead(il::RegId reg) {
    const il::RegId root = ctx_.function.registers().rootOf(reg);
    const RegVar& variable = ctx_.registerVariable(root);
    const il::RegisterInfo& info = ctx_.function.registers()[reg];
    if (!info.isSubRegister()) {
      return variable.name;
    }
    const std::string window =
        info.offsetInParent == 0
            ? variable.name
            : std::format("({} >> 0x{:x})", variable.name, info.offsetInParent);
    return std::format("(({})({}))", intType(info.bits), window);
  }

  [[nodiscard]] HlStmtPtr lowerRegisterWrite(il::RegId reg, const std::string& value) {
    const il::RegId root = ctx_.function.registers().rootOf(reg);
    const RegVar& variable = ctx_.registerVariable(root);
    const il::RegisterInfo& info = ctx_.function.registers()[reg];
    const std::string rootType = intType(variable.width);
    if (!info.isSubRegister() || info.zeroExtendsParent) {
      return HlStmt::makeAssign(HlExpr::makeVar(variable.name),
                                HlExpr::makeRaw(std::format("({})({})", rootType, value)));
    }
    const std::string mask = lowMask(info.bits, rootType);
    const std::string shifted =
        info.offsetInParent == 0
            ? std::format("(({})({}) & {})", rootType, value, mask)
            : std::format("((({})({}) & {}) << 0x{:x})", rootType, value, mask,
                          info.offsetInParent);
    const std::string cleared =
        info.offsetInParent == 0
            ? std::format("({} & ~({}){})", variable.name, rootType, mask)
            : std::format("({} & ~((({}){}) << 0x{:x}))", variable.name, rootType, mask,
                          info.offsetInParent);
    return HlStmt::makeAssign(HlExpr::makeVar(variable.name),
                              HlExpr::makeRaw(std::format("{} | {}", cleared, shifted)));
  }

  void lowerEdge(il::BlockId from, il::BlockId to, StmtList& out) {
    XDEC_SPAN(edgeSpan, "emit", "edge", "from", obs::hex(ctx_.function.block(from).va));
    const EdgePlan plan = buildEdgePlan(
        ctx_, from, to, activeFrame_ ? &*activeFrame_ : nullptr);
    edgeSpan.set("copies", plan.copies.size());
    if (plan.empty()) {
      return;
    }

    StmtList* targetOut = &out;
    HlStmtPtr scopeStmt;
    if (plan.scoped) {
      scopeStmt = HlStmt::makeScope();
      targetOut = &scopeStmt->items;
      for (const EdgeCopy* copy : plan.snapshots) {
        const std::string snapshot = copy->name + "__prev";
        targetOut->push_back(HlStmt::makeVarDecl(copy->type, snapshot,
                                                 HlExpr::makeVar(copy->name),
                                                 /*constQual=*/true));
        ctx_.snapshots.emplace(copy->destination.index(), snapshot);
      }
    }

    std::vector<il::ExprId> roots;
    roots.reserve(plan.copies.size());
    for (const EdgeCopy& copy : plan.copies) {
      roots.push_back(copy.source);
    }
    expressions_.beginScope(roots);

    const auto isUndef = [this](const EdgeCopy& copy) {
      return ctx_.function.expr(copy.source).op == il::ExprOp::Undef;
    };
    for (std::size_t index = 0; index < plan.copies.size();) {
      if (isUndef(plan.copies[index]) && !plan.needsSnapshot(plan.copies[index])) {
        std::size_t end = index + 1;
        while (end < plan.copies.size() && isUndef(plan.copies[end]) &&
               !plan.needsSnapshot(plan.copies[end])) {
          ++end;
        }
        auto ma = std::make_unique<HlStmt>();
        ma->kind = HlStmtKind::MultiAssign;
        std::string names;
        for (std::size_t i = index; i < end; ++i) {
          ma->names.push_back(plan.copies[i].name);
          names += (i == index ? "" : ", ") + plan.copies[i].name;
        }
        ma->rhs = HlExpr::makeConst(0);
        ma->text = std::format("{}: never set up on this edge", names);
        targetOut->push_back(std::move(ma));
        index = end;
        continue;
      }
      const EdgeCopy& copy = plan.copies[index];
      const auto assignArm = [&copy](const std::string& arm) {
        return HlStmt::makeAssign(HlExpr::makeVar(copy.name), HlExpr::makeRaw(arm));
      };
      if (!lowerSelectAssign(copy.source, *targetOut, assignArm)) {
        targetOut->push_back(assignArm(assignedText(copy.source, *targetOut)));
      }
      ++index;
    }

    if (plan.scoped) {
      ctx_.snapshots.clear();
      out.push_back(std::move(scopeStmt));
    }
  }

  void consumePending(il::BlockId to, StmtList& out) {
    if (!pending_.valid()) {
      return;
    }
    const il::BlockId from = pending_;
    pending_ = il::BlockId{};
    lowerEdge(from, to, out);
  }

  CContext& ctx_;
  ExprPrinter& expressions_;
  il::BlockId last_{};
  il::BlockId pending_{};
  std::optional<ActiveFrame> activeFrame_;
};

}  // namespace

HlFunction lowerToHlAst(CContext& context, ExprPrinter& expressions) {
  return AstLowerer(context, expressions).run();
}

}  // namespace xdec::emit
