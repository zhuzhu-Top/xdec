// matchJumpTable (see the header for the families).
#include "xdec/analysis/jump_table.h"

namespace xdec::analysis {

namespace {

/// A table address: base constant plus index times stride, in the shapes the
/// lifter and simplifier actually produce. The index is kept verbatim for
/// emission; resolution deliberately never analyses it.
struct TableAddress {
  uint64_t base = 0;
  uint32_t stride = 0;
  il::ExprId index{};
};

/// The single value `id` has: as written, or failing that as the caller's
/// resolver evaluates it. Literal first, always — reassociation normalises a
/// base into that position, so a literal is both the common case and the one
/// that costs nothing to check.
[[nodiscard]] bool singleValueOf(const il::Function& function,
                                 const ConstantResolver& resolve, il::ExprId id,
                                 uint64_t& out) {
  if (function.asConstant(id, out)) {
    return true;
  }
  if (!resolve) {
    return false;
  }
  const std::optional<uint64_t> resolved = resolve(id);
  if (!resolved) {
    return false;
  }
  out = *resolved;
  return true;
}

/// Matches `base + index*stride`, `base + (index << log2stride)`, `base +
/// index`, or a bare `base`. Any operand may be absent, but base is required and
/// must have one value — spelled as a constant, or evaluating to one.
[[nodiscard]] std::optional<TableAddress> matchTableAddress(const il::Function& function,
                                                            il::ExprId address,
                                                            uint32_t defaultStride,
                                                            const ConstantResolver& resolve) {
  uint64_t constant = 0;
  const il::Expr& expr = function.expr(address);

  if (function.asConstant(address, constant)) {
    return TableAddress{constant, defaultStride, il::ExprId{}};
  }
  if (expr.op != il::ExprOp::Add) {
    return std::nullopt;
  }
  // One side is the base; the other is the scaled index. The base is whichever
  // side has a single value, tried in written order — and the index is not
  // consulted about whether it has one, so a dispatcher whose state happens to
  // be known on this path is still matched as the table it is.
  const bool leftBase = singleValueOf(function, resolve, expr.operands[0], constant);
  const il::ExprId scaled = expr.operands[leftBase ? 1 : 0];
  if (!leftBase && !singleValueOf(function, resolve, expr.operands[1], constant)) {
    return std::nullopt;
  }

  const il::Expr& scaledExpr = function.expr(scaled);
  uint64_t amount = 0;
  if (scaledExpr.op == il::ExprOp::Shl &&
      function.asConstant(scaledExpr.operands[1], amount) && amount <= 3) {
    return TableAddress{constant, uint32_t{1} << amount, scaledExpr.operands[0]};
  }
  if (scaledExpr.op == il::ExprOp::Mul &&
      function.asConstant(scaledExpr.operands[1], amount) &&
      (amount == 1 || amount == 2 || amount == 4 || amount == 8)) {
    return TableAddress{constant, static_cast<uint32_t>(amount), scaledExpr.operands[0]};
  }
  if (scaledExpr.op == il::ExprOp::ZExt || scaledExpr.op == il::ExprOp::SExt ||
      scaledExpr.op == il::ExprOp::And) {
    // Unscaled, or scaled by an obfuscation no-op the rules kept honest:
    // entries sit back to back.
    return TableAddress{constant, defaultStride, scaled};
  }
  return std::nullopt;
}

/// The load underneath a branch target, through the redundant-cast chains the
/// lifter leaves (`zext:i64(zext:i32(load))`) and the load-address arithmetic
/// the simplifier leaves (`load(base + 0)` is the canonical non-form).
[[nodiscard]] std::optional<TableAddress> loadTable(const il::Function& function,
                                                    il::ExprId loaded,
                                                    uint32_t& entryBits,
                                                    bool& signedEntries,
                                                    const ConstantResolver& resolve) {
  const il::Expr* expr = &function.expr(loaded);
  signedEntries = false;
  while ((expr->op == il::ExprOp::ZExt || expr->op == il::ExprOp::SExt ||
          expr->op == il::ExprOp::Trunc) &&
         expr->operandCount == 1) {
    signedEntries |= expr->op == il::ExprOp::SExt;
    expr = &function.expr(expr->operands[0]);
  }
  if (expr->op != il::ExprOp::Value) {
    return std::nullopt;
  }
  const il::ValueId value{static_cast<uint32_t>(expr->immediate)};
  if (!function.hasValue(value)) {
    return std::nullopt;
  }
  const il::ValueInfo& info = function.value(value);
  if (!function.hasOp(info.definition)) {
    return std::nullopt;
  }
  const il::Op& load = function.op(info.definition);
  if (load.code != il::OpCode::Load || !info.type.isScalarInteger()) {
    return std::nullopt;
  }
  entryBits = info.type.bits();
  const auto operands = function.operands(load);
  return matchTableAddress(function, operands[0], entryBits / 8, resolve);
}

}  // namespace

std::optional<JumpTable> matchJumpTable(const il::Function& function, il::ExprId target,
                                        const ConstantResolver& resolve) {
  const il::Expr& expr = function.expr(target);

  // Offset families first, because they wrap the pointer one:
  // anchor + offset(load).
  if (expr.op == il::ExprOp::Add) {
    uint64_t anchor = 0;
    for (const int arm : {0, 1}) {
      // The anchor may be a merged value like the base (an `adr` in a block the
      // branch is reached from, rather than in the branch's own block), so it
      // gets the same treatment. Mistaking the offset arm for the anchor is not
      // a risk it introduces: the other arm still has to match a table load,
      // and the offset arm never does.
      if (!singleValueOf(function, resolve, expr.operands[arm], anchor)) {
        continue;
      }
      const il::ExprId offset = expr.operands[arm ^ 1];
      const il::Expr& offsetExpr = function.expr(offset);

      // anchor + sext(load32(..)): signed entries, unshifted.
      if (offsetExpr.op == il::ExprOp::SExt && offsetExpr.operandCount == 1) {
        uint32_t entryBits = 0;
        bool signedEntries = false;
        if (auto address =
                loadTable(function, offsetExpr.operands[0], entryBits, signedEntries, resolve)) {
          if (entryBits == 32) {
            return JumpTable{address->base, address->stride, entryBits,
                             /*relative=*/true, anchor, /*signedOffsets=*/true,
                             /*offsetShift=*/0, address->index};
          }
        }
      }

      // anchor + (extend(loadW(..)) << k): packed small entries.
      if (offsetExpr.op == il::ExprOp::Shl && offsetExpr.operandCount == 2) {
        uint64_t shift = 0;
        if (function.asConstant(offsetExpr.operands[1], shift) && shift <= 4) {
          uint32_t entryBits = 0;
          bool signedEntries = false;
          if (auto address =
                  loadTable(function, offsetExpr.operands[0], entryBits, signedEntries, resolve)) {
            if (entryBits == 8 || entryBits == 16 || entryBits == 32) {
              return JumpTable{address->base, address->stride, entryBits,
                               /*relative=*/true, anchor, signedEntries,
                               static_cast<uint32_t>(shift), address->index};
            }
          }
        }
      }
    }
  }

  // Family one: the branch target is the load itself.
  uint32_t entryBits = 0;
  bool signedEntries = false;
  if (auto address = loadTable(function, target, entryBits, signedEntries, resolve)) {
    if (entryBits == 64) {
      return JumpTable{address->base, address->stride, entryBits,
                       /*relative=*/false, /*anchor=*/0, /*signedOffsets=*/false,
                       /*offsetShift=*/0, address->index};
    }
  }
  return std::nullopt;
}

}  // namespace xdec::analysis
