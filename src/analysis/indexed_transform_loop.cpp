// matchIndexedTransformLoop (see the header for the pattern).
#include "xdec/analysis/indexed_transform_loop.h"

#include <unordered_map>
#include <vector>

namespace xdec::analysis {

namespace {

[[nodiscard]] il::ExprId stripCasts(const il::Function& function, il::ExprId id) {
  for (;;) {
    const il::Expr& expr = function.expr(id);
    if (expr.op != il::ExprOp::ZExt && expr.op != il::ExprOp::SExt &&
        expr.op != il::ExprOp::Trunc && expr.op != il::ExprOp::Bitcast) {
      return id;
    }
    id = expr.operand(0);
  }
}

/// Whether `id`, once any widening/narrowing around it is stripped, is
/// exactly the leaf referring to `value`.
[[nodiscard]] bool isValueLeaf(const il::Function& function, il::ExprId id, il::ValueId value) {
  const il::Expr& expr = function.expr(stripCasts(function, id));
  return expr.op == il::ExprOp::Value && expr.immediate == value.index();
}

/// Whether `id`'s expression tree reads `value` anywhere in it. Memoized
/// per call: the pool is hash-consed, so a shared subexpression is common
/// and would otherwise be re-walked once per parent.
[[nodiscard]] bool exprDependsOnValue(const il::Function& function, il::ExprId id,
                                      il::ValueId value,
                                      std::unordered_map<uint32_t, bool>& memo) {
  if (const auto it = memo.find(id.index()); it != memo.end()) {
    return it->second;
  }
  const il::Expr& expr = function.expr(id);
  bool result = false;
  if (expr.op == il::ExprOp::Value) {
    result = expr.immediate == value.index();
  } else {
    for (unsigned i = 0; i < expr.operandCount && !result; ++i) {
      result = exprDependsOnValue(function, expr.operand(i), value, memo);
    }
  }
  memo[id.index()] = result;
  return result;
}

/// An induction phi candidate at a loop header: `value = phi(start, ...,
/// value + stride, ...)`, every loop-carried edge agreeing on `stride`.
struct InductionCandidate {
  il::ValueId value;
  int64_t stride = 0;
  std::optional<uint64_t> start;
};

/// The net change `operand` makes to `value` in one iteration, when
/// `operand` is exactly `value + const` or `value - const` (in either
/// operand order for `+`). Nullopt for anything else, including a bare
/// re-read of `value` (stride zero is not an induction variable this
/// analysis names).
[[nodiscard]] std::optional<int64_t> matchSelfIncrement(const il::Function& function,
                                                         il::ExprId operand, il::ValueId value) {
  const il::Expr& expr = function.expr(operand);
  uint64_t amount = 0;
  if (expr.op == il::ExprOp::Add) {
    if (isValueLeaf(function, expr.operand(0), value) &&
        function.asConstant(expr.operand(1), amount)) {
      return static_cast<int64_t>(amount);
    }
    if (isValueLeaf(function, expr.operand(1), value) &&
        function.asConstant(expr.operand(0), amount)) {
      return static_cast<int64_t>(amount);
    }
  } else if (expr.op == il::ExprOp::Sub && isValueLeaf(function, expr.operand(0), value) &&
             function.asConstant(expr.operand(1), amount)) {
    return -static_cast<int64_t>(amount);
  }
  return std::nullopt;
}

[[nodiscard]] std::vector<InductionCandidate> inductionCandidates(const il::Function& function,
                                                                   const NaturalLoop& loop) {
  std::vector<InductionCandidate> candidates;
  const il::Block& header = function.block(loop.header);
  for (const il::OpId opId : header.ops) {
    const il::Op& op = function.op(opId);
    if (op.code != il::OpCode::Phi) {
      continue;
    }
    const auto operands = function.operands(op);
    if (operands.size() != header.predecessors.size()) {
      continue;  // malformed for this walk; stay conservative
    }
    std::optional<int64_t> stride;
    std::optional<uint64_t> start;
    bool startInconsistent = false;
    bool ok = true;
    for (std::size_t i = 0; ok && i < operands.size(); ++i) {
      const il::BlockId pred = header.predecessors[i];
      const il::ExprId operand = operands[i];
      if (loop.blocks.contains(pred)) {
        const std::optional<int64_t> thisStride =
            matchSelfIncrement(function, operand, op.result);
        if (!thisStride || (stride && *stride != *thisStride)) {
          ok = false;
        } else {
          stride = thisStride;
        }
      } else if (!startInconsistent) {
        uint64_t constant = 0;
        if (!function.asConstant(operand, constant)) {
          start.reset();
          startInconsistent = true;
        } else if (!start) {
          start = constant;
        } else if (*start != constant) {
          start.reset();
          startInconsistent = true;
        }
      }
    }
    if (ok && stride) {
      candidates.push_back({op.result, *stride, start});
    }
  }
  return candidates;
}

/// `base + index*scale`, `base + (index << k)`, or `base + index` (either
/// operand order), with `index` traced through `matchIndexTerm` and `base`
/// required not to itself read `value`.
struct ScaledIndex {
  il::ExprId base;
  uint64_t scale;
};

[[nodiscard]] std::optional<uint64_t> matchIndexTerm(const il::Function& function,
                                                      il::ExprId term, il::ValueId index) {
  if (isValueLeaf(function, term, index)) {
    return uint64_t{1};
  }
  const il::Expr& expr = function.expr(stripCasts(function, term));
  uint64_t amount = 0;
  if (expr.op == il::ExprOp::Mul) {
    if (isValueLeaf(function, expr.operand(0), index) &&
        function.asConstant(expr.operand(1), amount)) {
      return amount;
    }
    if (isValueLeaf(function, expr.operand(1), index) &&
        function.asConstant(expr.operand(0), amount)) {
      return amount;
    }
  } else if (expr.op == il::ExprOp::Shl && isValueLeaf(function, expr.operand(0), index) &&
             function.asConstant(expr.operand(1), amount) && amount < 64) {
    return uint64_t{1} << amount;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<ScaledIndex> matchScaledIndexAddress(const il::Function& function,
                                                                  il::ExprId address,
                                                                  il::ValueId index) {
  const il::Expr& expr = function.expr(address);
  if (expr.op != il::ExprOp::Add) {
    return std::nullopt;
  }
  for (const unsigned indexSide : {0u, 1u}) {
    const il::ExprId candidate = expr.operand(indexSide);
    const il::ExprId other = expr.operand(1 - indexSide);
    const std::optional<uint64_t> scale = matchIndexTerm(function, candidate, index);
    if (!scale) {
      continue;
    }
    std::unordered_map<uint32_t, bool> memo;
    if (!exprDependsOnValue(function, other, index, memo)) {
      return ScaledIndex{other, *scale};
    }
  }
  return std::nullopt;
}

struct Transform {
  TransformOp op;
  il::ExprId key;
  bool loadIsFirstOperand;
};

/// Whether `stored` (a store's value operand) computes `op(loaded, key)` or
/// `op(key, loaded)` for one of the invertible ops this pattern recognizes.
[[nodiscard]] std::optional<Transform> matchTransform(const il::Function& function,
                                                       il::ExprId stored, il::ValueId loaded) {
  const il::Expr& expr = function.expr(stripCasts(function, stored));
  TransformOp op;
  switch (expr.op) {
    case il::ExprOp::Xor:
      op = TransformOp::Xor;
      break;
    case il::ExprOp::Add:
      op = TransformOp::Add;
      break;
    case il::ExprOp::Sub:
      op = TransformOp::Sub;
      break;
    case il::ExprOp::RotL:
      op = TransformOp::RotateLeft;
      break;
    case il::ExprOp::RotR:
      op = TransformOp::RotateRight;
      break;
    default:
      return std::nullopt;
  }
  const bool firstIsLoad = isValueLeaf(function, expr.operand(0), loaded);
  const bool secondIsLoad = isValueLeaf(function, expr.operand(1), loaded);
  if (op == TransformOp::RotateLeft || op == TransformOp::RotateRight) {
    // Operand 0 is the rotated value; a loaded rotate *amount* is not this
    // pattern.
    if (!firstIsLoad || secondIsLoad) {
      return std::nullopt;
    }
    return Transform{op, expr.operand(1), true};
  }
  if (firstIsLoad && !secondIsLoad) {
    return Transform{op, expr.operand(1), true};
  }
  if (secondIsLoad && !firstIsLoad) {
    return Transform{op, expr.operand(0), false};
  }
  return std::nullopt;
}

}  // namespace

std::string_view toString(TransformOp op) noexcept {
  switch (op) {
    case TransformOp::Xor:
      return "^";
    case TransformOp::Add:
      return "+";
    case TransformOp::Sub:
      return "-";
    case TransformOp::RotateLeft:
      return "rotl";
    case TransformOp::RotateRight:
      return "rotr";
  }
  return "?";
}

std::optional<IndexedTransformLoop> matchIndexedTransformLoop(const il::Function& function,
                                                                const NaturalLoop& loop) {
  const std::vector<InductionCandidate> candidates = inductionCandidates(function, loop);
  for (const InductionCandidate& candidate : candidates) {
    for (const il::BlockId blockId : loop.blocks) {
      const il::Block& block = function.block(blockId);
      for (std::size_t i = 0; i < block.ops.size(); ++i) {
        const il::Op& loadOpRef = function.op(block.ops[i]);
        if (loadOpRef.code != il::OpCode::Load) {
          continue;
        }
        const std::optional<ScaledIndex> src = matchScaledIndexAddress(
            function, function.operands(loadOpRef)[0], candidate.value);
        if (!src) {
          continue;
        }
        const il::ValueId loadedValue = loadOpRef.result;
        for (std::size_t j = i + 1; j < block.ops.size(); ++j) {
          const il::Op& storeOpRef = function.op(block.ops[j]);
          if (storeOpRef.code != il::OpCode::Store) {
            continue;
          }
          const auto storeOperands = function.operands(storeOpRef);
          const std::optional<Transform> transform =
              matchTransform(function, storeOperands[1], loadedValue);
          if (!transform) {
            continue;
          }
          const std::optional<ScaledIndex> dst =
              matchScaledIndexAddress(function, storeOperands[0], candidate.value);
          if (!dst || dst->scale != src->scale) {
            continue;
          }
          IndexedTransformLoop result;
          result.header = loop.header;
          result.index = candidate.value;
          result.indexStart = candidate.start;
          result.indexStride = candidate.stride;
          result.srcBase = src->base;
          result.dstBase = dst->base;
          result.elementScale = src->scale;
          result.op = transform->op;
          result.key = transform->key;
          result.loadIsFirstOperand = transform->loadIsFirstOperand;
          result.block = blockId;
          result.loadOp = block.ops[i];
          result.storeOp = block.ops[j];
          return result;
        }
      }
    }
  }
  return std::nullopt;
}

}  // namespace xdec::analysis
