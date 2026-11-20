// ollvm_deobf plugin: specialized control-flow flattening (CFF) deobfuscation.
//
// Capabilities:
//   1. `ollvm-state-thread` (IL Pass at Ssa, requires ssa-optimize):
//      For dispatcher headers whose primary `Phi` (`statePhi`) drives a small
//      pure decision tree (`CondBranch` / resolved `IndirectBranch`), binds
//      constant (and `select(cond, c1, c2)`) incoming state edges and threads
//      them directly to the resolved target handler block, allowing downstream
//      `ssa-optimize` / `dce` to collapse dead dispatch trees.
//   2. `ollvm-ast-cleanup` (AST Transform):
//      Unwraps vacuous `while (true)` shells left behind after flattening
//      de-threading when no `continue` or loop-owned `break` remains inside.
#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "xdec/il/ceval.h"
#include "xdec/il/function.h"
#include "xdec/plugin/abi.h"

namespace {

constexpr std::size_t kMaxTreeHops = 24;
constexpr std::size_t kMaxThreadedEdges = 4096;

/// Evaluates `id` under `stateVal -> stateConst`, reading immutable memory for
/// jump-table loads inside the dispatcher tree.
[[nodiscard]] std::optional<uint64_t> evalUnderState(
    xdec::il::Function& function, const xdec::ByteReader* reader,
    const xdec::MemoryFacts& facts, xdec::il::ValueId stateVal,
    uint64_t stateConst, xdec::il::ExprId id, unsigned depth = 0) {
  if (!id.valid() || depth > 32) {
    return std::nullopt;
  }
  const xdec::il::Expr expr = function.expr(id);
  switch (expr.op) {
    case xdec::il::ExprOp::Const:
      return expr.immediate;
    case xdec::il::ExprOp::Undef:
    case xdec::il::ExprOp::EntryReg:
      return std::nullopt;
    case xdec::il::ExprOp::Value: {
      const auto v = xdec::il::ValueId{static_cast<uint32_t>(expr.immediate)};
      if (v == stateVal) {
        return stateConst;
      }
      if (!function.hasValue(v)) {
        return std::nullopt;
      }
      const xdec::il::OpId defId = function.value(v).definition;
      if (!defId.valid() || !function.hasOp(defId)) {
        return std::nullopt;
      }
      const xdec::il::Op& def = function.op(defId);
      if (def.code == xdec::il::OpCode::Load && reader != nullptr) {
        const auto ops = function.operands(def);
        if (ops.empty() || !def.type.isScalarInteger() || def.type.bits() > 64 ||
            def.type.bits() % 8 != 0) {
          return std::nullopt;
        }
        const auto addr = evalUnderState(function, reader, facts, stateVal,
                                         stateConst, ops[0], depth + 1);
        const std::size_t bytes = def.type.bits() / 8;
        if (!addr.has_value() || !facts.isImmutable(*addr, bytes)) {
          return std::nullopt;
        }
        std::vector<std::byte> buf(bytes);
        if (!(*reader)(*addr, buf)) {
          return std::nullopt;
        }
        uint64_t val = 0;
        for (std::size_t i = 0; i < bytes; ++i) {
          val |= static_cast<uint64_t>(static_cast<uint8_t>(buf[i])) << (i * 8);
        }
        return val;
      }
      return std::nullopt;
    }
    default:
      break;
  }
  if (expr.operandCount == 0) {
    return std::nullopt;
  }
  xdec::il::Expr rebuilt = expr;
  for (unsigned i = 0; i < expr.operandCount; ++i) {
    const auto child = evalUnderState(function, reader, facts, stateVal,
                                      stateConst, expr.operands[i], depth + 1);
    if (!child.has_value()) {
      return std::nullopt;
    }
    const xdec::il::Type childType = function.expr(expr.operands[i]).type;
    if (!childType.isScalarInteger() && !childType.isFloat()) {
      return std::nullopt;
    }
    rebuilt.operands[i] = function.constant(childType, *child);
  }
  xdec::il::ConcreteValue out;
  if (xdec::il::tryEvalConst(function, function.intern(rebuilt), out)) {
    return out.lo;
  }
  return std::nullopt;
}

[[nodiscard]] bool isPureTreeBlock(const xdec::il::Function& function,
                                   xdec::il::BlockId blockId, bool allowPhis) {
  const auto& ops = function.block(blockId).ops;
  if (ops.empty() || function.block(blockId).external) {
    return false;
  }
  const xdec::il::Op& term = function.op(ops.back());
  if (term.code != xdec::il::OpCode::CondBranch &&
      term.code != xdec::il::OpCode::Branch &&
      (term.code != xdec::il::OpCode::IndirectBranch ||
       function.targets(term).empty())) {
    return false;
  }
  for (std::size_t i = 0; i + 1 < ops.size(); ++i) {
    const xdec::il::Op& op = function.op(ops[i]);
    if (op.code == xdec::il::OpCode::Phi) {
      if (!allowPhis) {
        return false;
      }
      continue;
    }
    if (op.code == xdec::il::OpCode::Load) {
      continue;  // checked for immutability during evalUnderState
    }
    return false;
  }
  return true;
}

[[nodiscard]] bool blockHasPhis(const xdec::il::Function& function,
                                xdec::il::BlockId blockId) {
  const auto& ops = function.block(blockId).ops;
  return !ops.empty() && function.op(ops.front()).code == xdec::il::OpCode::Phi;
}

struct ThreadTarget {
  xdec::il::BlockId target{};
  xdec::il::BlockId lastTreePred{};
};

[[nodiscard]] std::optional<ThreadTarget> resolveHandlerForState(
    xdec::il::Function& function, const xdec::ByteReader* reader,
    const xdec::MemoryFacts& facts, xdec::il::BlockId header,
    xdec::il::ValueId stateVal, uint64_t stateConst) {
  xdec::il::BlockId cur = header;
  xdec::il::BlockId lastPred{};
  std::unordered_set<uint32_t> visited;

  for (std::size_t step = 0; step < kMaxTreeHops; ++step) {
    if (!visited.insert(cur.index()).second) {
      return std::nullopt;
    }
    if (cur != header &&
        (!isPureTreeBlock(function, cur, /*allowPhis=*/false) ||
         blockHasPhis(function, cur))) {
      return ThreadTarget{cur, lastPred};
    }
    if (cur == header && !isPureTreeBlock(function, cur, /*allowPhis=*/true)) {
      return std::nullopt;
    }
    const xdec::il::Op& term =
        function.op(function.block(cur).ops.back());
    const auto targets = function.targets(term);
    if (term.code == xdec::il::OpCode::Branch) {
      if (targets.size() != 1) {
        return std::nullopt;
      }
      lastPred = cur;
      cur = targets[0];
      continue;
    }
    if (term.code == xdec::il::OpCode::CondBranch) {
      if (targets.size() != 2) {
        return std::nullopt;
      }
      const auto condVal =
          evalUnderState(function, reader, facts, stateVal, stateConst,
                         function.operands(term)[0]);
      if (!condVal.has_value()) {
        return ThreadTarget{cur, lastPred};
      }
      lastPred = cur;
      cur = (*condVal != 0) ? targets[0] : targets[1];
      continue;
    }
    if (term.code == xdec::il::OpCode::IndirectBranch) {
      const auto targetVa =
          evalUnderState(function, reader, facts, stateVal, stateConst,
                         function.operands(term)[0]);
      if (!targetVa.has_value()) {
        return ThreadTarget{cur, lastPred};
      }
      const xdec::il::BlockId next = function.blockAt(*targetVa);
      if (!next.valid() ||
          std::find(targets.begin(), targets.end(), next) == targets.end()) {
        return std::nullopt;
      }
      lastPred = cur;
      cur = next;
      continue;
    }
    return std::nullopt;
  }
  return std::nullopt;
}

class OllvmStateThreadPass final : public xdec::pass::FunctionPass {
 public:
  OllvmStateThreadPass()
      : FunctionPass([] {
          xdec::pass::PassInfo info;
          info.name = "ollvm-state-thread";
          info.level = xdec::il::Maturity::Ssa;
          info.produces = xdec::il::Maturity::Ssa;
          info.requirements = {"ssa-optimize"};
          info.invalidates = {"dominators", "scc"};
          return info;
        }()) {}

  xdec::Result<bool> run(xdec::pass::Context& context) override {
    xdec::il::Function& function = context.function();
    const xdec::ByteReader* reader = context.image();
    const xdec::MemoryFacts& facts = context.memoryFacts();

    // Precompute whether any ValueId defined by a pure tree header is read
    // outside pure dispatcher-tree blocks? Specifically, only consider headers
    // whose *only* Phi is `statePhi` and that have no other Value-defining ops
    // (or whose other Value-defining ops have no uses outside `header`).
    std::vector<std::unordered_set<uint32_t>> userBlocks(function.valueCount());
    std::vector<uint32_t> exprSeen(function.exprCount(), 0);
    uint32_t exprEpoch = 0;
    std::vector<xdec::il::ExprId> exprWork;
    for (const xdec::il::BlockId blockId : function.blockHandles()) {
      for (const xdec::il::OpId opId : function.block(blockId).ops) {
        const xdec::il::Op& op = function.op(opId);
        const bool isPhi = op.code == xdec::il::OpCode::Phi;
        for (const xdec::il::ExprId operand : function.operands(op)) {
          if (++exprEpoch == 0) {
            std::fill(exprSeen.begin(), exprSeen.end(), 0);
            exprEpoch = 1;
          }
          exprWork.clear();
          exprWork.push_back(operand);
          while (!exprWork.empty()) {
            const auto eid = exprWork.back();
            exprWork.pop_back();
            if (eid.index() < exprSeen.size()) {
              if (exprSeen[eid.index()] == exprEpoch) {
                continue;
              }
              exprSeen[eid.index()] = exprEpoch;
            }
            const xdec::il::Expr& e = function.expr(eid);
            if (e.op == xdec::il::ExprOp::Value) {
              const auto v = static_cast<uint32_t>(e.immediate);
              if (v < userBlocks.size() && !isPhi) {
                userBlocks[v].insert(blockId.index());
              }
              continue;
            }
            for (unsigned k = 0; k < e.operandCount; ++k) {
              exprWork.push_back(e.operands[k]);
            }
          }
        }
      }
    }

    struct RewirePlan {
      xdec::il::BlockId pred;
      xdec::il::BlockId header;
      xdec::il::BlockId target1;
      xdec::il::BlockId target2;  // valid iff upgrading Branch -> CondBranch
      xdec::il::ExprId selectCond{};
    };
    std::vector<RewirePlan> plans;

    for (const xdec::il::BlockId header : function.blockHandles()) {
      if (header == function.entryBlock() ||
          !isPureTreeBlock(function, header, /*allowPhis=*/true)) {
        continue;
      }
      const auto& ops = function.block(header).ops;
      std::vector<xdec::il::OpId> phis;
      for (const xdec::il::OpId opId : ops) {
        if (function.op(opId).code != xdec::il::OpCode::Phi) {
          break;
        }
        phis.push_back(opId);
      }
      if (phis.size() != 1) {
        continue;  // only single-state headers can be bypassed without splitting other Phis
      }
      const xdec::il::Op& statePhiOp = function.op(phis[0]);
      const xdec::il::ValueId stateVal = statePhiOp.result;
      if (!stateVal.valid()) {
        continue;
      }
      // Require stateVal to be consumed only by pure dispatcher-tree blocks
      // (never read inside side-effecting handler bodies).
      bool stateEscapesToHandlers = false;
      for (const uint32_t uIdx : userBlocks[stateVal.index()]) {
        const xdec::il::BlockId ub{uIdx};
        if (!isPureTreeBlock(function, ub, ub == header)) {
          stateEscapesToHandlers = true;
          break;
        }
      }
      if (stateEscapesToHandlers) {
        continue;
      }
      const auto& preds = function.block(header).predecessors;
      const auto phiOperands = function.operands(statePhiOp);
      if (preds.size() < 2 || phiOperands.size() != preds.size()) {
        continue;
      }

      for (std::size_t slot = 0; slot < preds.size(); ++slot) {
        const xdec::il::BlockId pred = preds[slot];
        if (!pred.valid() || pred == header) {
          continue;
        }
        const xdec::il::OpId pTermId = function.block(pred).terminator();
        if (!pTermId.valid()) {
          continue;
        }
        const xdec::il::Op& pTerm = function.op(pTermId);
        if (pTerm.code != xdec::il::OpCode::Branch &&
            pTerm.code != xdec::il::OpCode::CondBranch) {
          continue;
        }

        const xdec::il::ExprId inExpr = phiOperands[slot];
        uint64_t k = 0;
        if (function.asConstant(inExpr, k)) {
          const auto resolved =
              resolveHandlerForState(function, reader, facts, header, stateVal, k);
          if (resolved.has_value() && resolved->target.valid() &&
              resolved->target != header &&
              !blockHasPhis(function, resolved->target) &&
              !function.block(resolved->target).external) {
            plans.push_back(RewirePlan{pred, header, resolved->target, {}, {}});
          }
        } else if (pTerm.code == xdec::il::OpCode::Branch &&
                   function.expr(inExpr).op == xdec::il::ExprOp::Select) {
          const xdec::il::Expr& sel = function.expr(inExpr);
          uint64_t k1 = 0;
          uint64_t k2 = 0;
          if (function.asConstant(sel.operands[1], k1) &&
              function.asConstant(sel.operands[2], k2)) {
            const auto r1 =
                resolveHandlerForState(function, reader, facts, header, stateVal, k1);
            const auto r2 =
                resolveHandlerForState(function, reader, facts, header, stateVal, k2);
            if (r1.has_value() && r2.has_value() && r1->target.valid() &&
                r2->target.valid() && r1->target != header &&
                r2->target != header &&
                !blockHasPhis(function, r1->target) &&
                !blockHasPhis(function, r2->target) &&
                !function.block(r1->target).external &&
                !function.block(r2->target).external) {
              plans.push_back(RewirePlan{pred, header, r1->target, r2->target,
                                         sel.operands[0]});
            }
          }
        }
        if (plans.size() >= kMaxThreadedEdges) {
          break;
        }
      }
      if (plans.size() >= kMaxThreadedEdges) {
        break;
      }
    }

    if (plans.empty()) {
      return false;
    }

    // Apply edge rewirings, updating target headers' statePhi slot lists cleanly.
    bool changed = false;
    std::unordered_set<uint32_t> affectedHeaders;
    std::unordered_map<uint32_t, std::vector<std::size_t>> prunedSlotsByHeader;
    for (const RewirePlan& plan : plans) {
      const xdec::il::OpId termId = function.block(plan.pred).terminator();
      if (!termId.valid()) {
        continue;
      }
      const xdec::il::Op& term = function.op(termId);
      if (plan.target2.valid()) {
        if (term.code != xdec::il::OpCode::Branch) {
          continue;
        }
        const uint64_t va = term.va;
        function.dropTerminator(plan.pred);
        if (plan.target1 == plan.target2) {
          (void)function.appendBranch(plan.pred, va, plan.target1);
        } else {
          (void)function.appendCondBranch(plan.pred, va, plan.selectCond,
                                          plan.target1, plan.target2);
        }
      } else {
        const auto oldTargets = function.targets(term);
        std::vector<xdec::il::BlockId> nextTargets(oldTargets.begin(),
                                                   oldTargets.end());
        bool touched = false;
        for (xdec::il::BlockId& t : nextTargets) {
          if (t == plan.header) {
            t = plan.target1;
            touched = true;
          }
        }
        if (!touched) {
          continue;
        }
        function.setTargets(termId, nextTargets);
      }

      const auto& hPreds = function.block(plan.header).predecessors;
      for (std::size_t slot = 0; slot < hPreds.size(); ++slot) {
        if (hPreds[slot] == plan.pred) {
          prunedSlotsByHeader[plan.header.index()].push_back(slot);
        }
      }
      affectedHeaders.insert(plan.header.index());
      changed = true;
    }

    if (!changed) {
      return false;
    }

    for (auto& [headerIdx, slots] : prunedSlotsByHeader) {
      const xdec::il::BlockId hId{headerIdx};
      std::sort(slots.begin(), slots.end(), std::greater<std::size_t>{});
      slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
      for (const xdec::il::OpId opId : function.block(hId).ops) {
        if (function.op(opId).code != xdec::il::OpCode::Phi) {
          break;
        }
        const auto ops = function.operands(function.op(opId));
        std::vector<xdec::il::ExprId> pruned(ops.begin(), ops.end());
        for (const std::size_t slot : slots) {
          if (slot < pruned.size()) {
            pruned.erase(pruned.begin() + static_cast<std::ptrdiff_t>(slot));
          }
        }
        function.setOperands(opId, pruned);
      }
    }

    function.rebuildEdges();
    return true;
  }
};

/// Checks if `node` references `Continue` or `Break` belonging to the enclosing
/// `while (true)` (does not recurse into nested loops/switches for `Break`, or
/// nested loops for `Continue`).
[[nodiscard]] bool hasLoopControl(const xdec::emit::Stmt* node,
                                  bool insideNestedSwitch) {
  if (node == nullptr) {
    return false;
  }
  switch (node->kind) {
    case xdec::emit::StmtKind::Continue:
      return true;
    case xdec::emit::StmtKind::Break:
      return !insideNestedSwitch;
    case xdec::emit::StmtKind::While:
    case xdec::emit::StmtKind::DoWhile:
      return false;
    case xdec::emit::StmtKind::Switch:
      for (const auto& body : node->caseBodies) {
        if (hasLoopControl(body.get(), true)) {
          return true;
        }
      }
      return hasLoopControl(node->defaultBody.get(), true) ||
             hasLoopControl(node->epilogue.get(), insideNestedSwitch);
    case xdec::emit::StmtKind::If:
      return hasLoopControl(node->thenArm.get(), insideNestedSwitch) ||
             hasLoopControl(node->elseArm.get(), insideNestedSwitch) ||
             hasLoopControl(node->epilogue.get(), insideNestedSwitch);
    case xdec::emit::StmtKind::Sequence:
      for (const auto& item : node->items) {
        if (hasLoopControl(item.get(), insideNestedSwitch)) {
          return true;
        }
      }
      return false;
    default:
      return false;
  }
}

void unwrapVacuousLoops(xdec::emit::StmtPtr& node) {
  if (!node) {
    return;
  }
  for (auto& item : node->items) {
    unwrapVacuousLoops(item);
  }
  unwrapVacuousLoops(node->thenArm);
  unwrapVacuousLoops(node->elseArm);
  unwrapVacuousLoops(node->body);
  for (auto& body : node->caseBodies) {
    unwrapVacuousLoops(body);
  }
  unwrapVacuousLoops(node->defaultBody);
  unwrapVacuousLoops(node->epilogue);

  if (node->kind == xdec::emit::StmtKind::While && !node->cond.valid() &&
      node->body && !hasLoopControl(node->body.get(), false)) {
    node = std::move(node->body);
  }
}

void ollvmAstCleanup(const xdec::il::Function&,
                     xdec::emit::StructuredFunction& structured) {
  unwrapVacuousLoops(structured.root);
}

}  // namespace

extern "C" XDEC_EXPORT std::uint32_t xdec_plugin_abi_version() {
  return XDEC_PLUGIN_ABI_VERSION;
}

extern "C" XDEC_EXPORT XdecPluginInitResult
xdec_plugin_init(const XdecPluginHost* host) {
  if (host == nullptr || host->passes == nullptr) {
    return {false, "invalid plugin host"};
  }
  const xdec::Result<void> added =
      host->passes->add(std::make_unique<OllvmStateThreadPass>());
  if (!added) {
    return {false, "failed to register ollvm-state-thread"};
  }
  if (host->astTransforms != nullptr) {
    host->astTransforms->push_back(ollvmAstCleanup);
  }
  return {true, nullptr};
}
