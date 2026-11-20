#include "xdec/analysis/effect_summary.h"

#include <unordered_set>

namespace xdec::analysis {

namespace {

[[nodiscard]] il::ExprId unwrapCasts(const il::Function& function, il::ExprId id) {
  for (unsigned depth = 0; depth < 8 && id.valid(); ++depth) {
    const il::Expr& e = function.expr(id);
    if ((e.op == il::ExprOp::ZExt || e.op == il::ExprOp::SExt ||
         e.op == il::ExprOp::Trunc || e.op == il::ExprOp::Bitcast) &&
        e.operandCount == 1) {
      id = e.operands[0];
      continue;
    }
    break;
  }
  return id;
}

struct AddTerm {
  il::ExprId expr{};
  int64_t sign = 1;  // +1 or -1
};

void collectAdditiveTerms(const il::Function& function, il::ExprId id, int64_t sign,
                          int64_t& offset, std::vector<AddTerm>& terms,
                          unsigned depth = 0) {
  if (!id.valid() || depth > 16) {
    return;
  }
  uint64_t c = 0;
  if (function.asConstant(id, c)) {
    offset += sign * static_cast<int64_t>(c);
    return;
  }
  const il::Expr& e = function.expr(id);
  if (e.op == il::ExprOp::Add && e.operandCount == 2) {
    collectAdditiveTerms(function, e.operands[0], sign, offset, terms, depth + 1);
    collectAdditiveTerms(function, e.operands[1], sign, offset, terms, depth + 1);
    return;
  }
  if (e.op == il::ExprOp::Sub && e.operandCount == 2) {
    collectAdditiveTerms(function, e.operands[0], sign, offset, terms, depth + 1);
    collectAdditiveTerms(function, e.operands[1], -sign, offset, terms, depth + 1);
    return;
  }
  if (e.op == il::ExprOp::Neg && e.operandCount == 1) {
    collectAdditiveTerms(function, e.operands[0], -sign, offset, terms, depth + 1);
    return;
  }
  terms.push_back({id, sign});
}

[[nodiscard]] bool matchScaledTerm(const il::Function& function, il::ExprId id,
                                   int64_t sign, il::ExprId& outIndex,
                                   int64_t& outScale) {
  const il::ExprId unwrapped = unwrapCasts(function, id);
  if (!unwrapped.valid()) {
    return false;
  }
  const il::Expr& e = function.expr(unwrapped);
  uint64_t c = 0;
  if (e.op == il::ExprOp::Shl && e.operandCount == 2 &&
      function.asConstant(e.operands[1], c) && c < 63) {
    outIndex = unwrapCasts(function, e.operands[0]);
    outScale = sign * static_cast<int64_t>(uint64_t{1} << c);
    return outIndex.valid();
  }
  if (e.op == il::ExprOp::Mul && e.operandCount == 2) {
    if (function.asConstant(e.operands[1], c)) {
      outIndex = unwrapCasts(function, e.operands[0]);
      outScale = sign * static_cast<int64_t>(c);
      return outIndex.valid();
    }
    if (function.asConstant(e.operands[0], c)) {
      outIndex = unwrapCasts(function, e.operands[1]);
      outScale = sign * static_cast<int64_t>(c);
      return outIndex.valid();
    }
  }
  return false;
}

[[nodiscard]] bool looksLikeBaseTerm(const il::Function& function, il::ExprId id) {
  const il::Expr& e = function.expr(id);
  return e.op == il::ExprOp::EntryReg || e.op == il::ExprOp::Value;
}

}  // namespace

IndexedSlotRef decomposeIndexedSlot(const il::Function& function, il::ExprId address) {
  IndexedSlotRef out;
  if (!address.valid()) {
    return out;
  }
  out.rawAddress = address;

  int64_t offset = 0;
  std::vector<AddTerm> terms;
  collectAdditiveTerms(function, address, 1, offset, terms);
  out.offset = offset;

  if (terms.empty()) {
    return out;
  }

  std::vector<AddTerm> unscaled;
  for (const AddTerm& t : terms) {
    il::ExprId idx;
    int64_t scale = 1;
    if (!out.index.valid() && matchScaledTerm(function, t.expr, t.sign, idx, scale)) {
      out.index = idx;
      out.scale = scale;
      continue;
    }
    unscaled.push_back(t);
  }

  if (unscaled.size() == 1 && unscaled[0].sign > 0) {
    out.base = unscaled[0].expr;
  } else if (unscaled.size() >= 2) {
    // Prefer an unscaled EntryReg / Value with +1 sign as base, and treat the
    // remaining positive/negative term as an unscaled index (scale +/-1).
    std::size_t baseIdx = 0;
    for (std::size_t i = 0; i < unscaled.size(); ++i) {
      if (unscaled[i].sign > 0 && looksLikeBaseTerm(function, unscaled[i].expr)) {
        baseIdx = i;
        break;
      }
    }
    if (unscaled[baseIdx].sign > 0) {
      out.base = unscaled[baseIdx].expr;
      if (!out.index.valid()) {
        for (std::size_t i = 0; i < unscaled.size(); ++i) {
          if (i != baseIdx) {
            out.index = unwrapCasts(function, unscaled[i].expr);
            out.scale = unscaled[i].sign;
            break;
          }
        }
      }
    }
  }
  return out;
}

ExprRootSummary summarizeExprRoot(const il::Function& function, il::ExprId expr) {
  ExprRootSummary out;
  if (!expr.valid()) {
    return out;
  }
  out.rawExpr = expr;
  out.resultBits = function.expr(expr).type.isScalarInteger() ||
                           function.expr(expr).type.isFloat()
                       ? function.expr(expr).type.bits()
                       : 0;

  il::ExprId cur = expr;
  for (unsigned depth = 0; depth < 8 && cur.valid(); ++depth) {
    const il::Expr& e = function.expr(cur);
    if (e.op == il::ExprOp::SExt && e.operandCount == 1) {
      out.signExtended = true;
      cur = e.operands[0];
      continue;
    }
    if ((e.op == il::ExprOp::ZExt || e.op == il::ExprOp::Trunc ||
         e.op == il::ExprOp::Bitcast) &&
        e.operandCount == 1) {
      if (e.op == il::ExprOp::ZExt) {
        out.zeroExtended = true;
      }
      cur = e.operands[0];
      continue;
    }
    if (e.op == il::ExprOp::Extract && e.operandCount == 1 && e.immediate == 0) {
      cur = e.operands[0];
      continue;
    }
    break;
  }

  out.coreExpr = cur;
  const il::Expr& core = function.expr(cur);
  out.rootOp = core.op;
  if (core.operandCount >= 1) {
    out.lhs = core.operands[0];
  }
  if (core.operandCount >= 2) {
    out.rhs = core.operands[1];
  }
  if (core.op == il::ExprOp::Select && core.operandCount == 3) {
    out.cond = core.operands[0];
    out.lhs = core.operands[1];
    out.rhs = core.operands[2];
  }
  if (core.op == il::ExprOp::Value) {
    const il::ValueId val{static_cast<uint32_t>(core.immediate)};
    if (function.hasValue(val)) {
      const il::OpId defId = function.value(val).definition;
      if (defId.valid() && function.hasOp(defId)) {
        const il::Op& defOp = function.op(defId);
        if (defOp.code == il::OpCode::Load) {
          out.isMemoryLoad = true;
          out.loadOp = defId;
          out.loadBits = defOp.type.bits();
          const auto ops = function.operands(defOp);
          if (!ops.empty()) {
            out.loadSlot = decomposeIndexedSlot(function, ops[0]);
          }
        }
      }
    }
  }
  return out;
}

CaseEffectSummary summarizeCaseEffects(const il::Function& function,
                                       std::span<const il::BlockId> blocks) {
  CaseEffectSummary summary;
  std::unordered_set<uint32_t> seenBlocks;
  for (const il::BlockId bid : blocks) {
    if (!bid.valid() || !function.hasBlock(bid) || !seenBlocks.insert(bid.index()).second) {
      continue;
    }
    summary.blocks.push_back(bid);
    const il::Block& block = function.block(bid);
    for (const il::OpId opId : block.ops) {
      const il::Op& op = function.op(opId);
      const auto operands = function.operands(op);
      switch (op.code) {
        case il::OpCode::Store:
          if (operands.size() >= 2) {
            SlotWriteEffect w;
            w.op = opId;
            w.va = op.va;
            w.widthBits = op.type.bits();
            w.target = decomposeIndexedSlot(function, operands[0]);
            w.value = summarizeExprRoot(function, operands[1]);
            summary.stores.push_back(std::move(w));
          }
          break;
        case il::OpCode::Load:
          if (!operands.empty()) {
            SlotReadEffect r;
            r.op = opId;
            r.va = op.va;
            r.widthBits = op.type.bits();
            r.source = decomposeIndexedSlot(function, operands[0]);
            summary.loads.push_back(std::move(r));
          }
          break;
        case il::OpCode::WriteReg:
          if (!operands.empty()) {
            RegWriteEffect rw;
            rw.op = opId;
            rw.va = op.va;
            rw.reg = op.reg();
            rw.value = summarizeExprRoot(function, operands[0]);
            summary.regWrites.push_back(std::move(rw));
          }
          break;
        case il::OpCode::Call:
          if (!operands.empty()) {
            CallEffect ce;
            ce.op = opId;
            ce.va = op.va;
            ce.target = operands[0];
            uint64_t targetVa = 0;
            if (function.asConstant(operands[0], targetVa)) {
              ce.constantTarget = targetVa;
            }
            summary.calls.push_back(std::move(ce));
          }
          break;
        case il::OpCode::CondBranch: {
          CondBranchEffect cb;
          cb.op = opId;
          cb.va = op.va;
          if (!operands.empty()) {
            cb.condition = operands[0];
            cb.condOp = function.expr(operands[0]).op;
          }
          const auto targets = function.targets(op);
          if (targets.size() >= 2) {
            cb.trueTarget = targets[0];
            cb.falseTarget = targets[1];
          }
          summary.condBranches.push_back(std::move(cb));
          break;
        }
        case il::OpCode::Return:
          summary.returns.push_back(opId);
          break;
        case il::OpCode::IndirectBranch:
          summary.indirectBranches.push_back(opId);
          break;
        default:
          break;
      }
    }
  }
  return summary;
}

}  // namespace xdec::analysis
