// Safe, ergonomic builder for constructing `il::Function`s from custom virtual
// ISAs, partial evaluators, or tests, with built-in edge rebuilding and
// IL verification on `finalize()`.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "xdec/il/function.h"
#include "xdec/il/verify.h"
#include "xdec/support/result.h"

namespace xdec::il {

class FunctionBuilder {
 public:
  FunctionBuilder(Arch arch, RegisterFile registers, uint64_t entryVa,
                  std::string name = {});

  [[nodiscard]] BlockId createBlock(uint64_t va);
  void setInsertPoint(BlockId block) noexcept { insertBlock_ = block; }
  [[nodiscard]] BlockId insertPoint() const noexcept { return insertBlock_; }
  void setCurrentVa(uint64_t va) noexcept { currentVa_ = va; }
  [[nodiscard]] uint64_t currentVa() const noexcept { return currentVa_; }

  [[nodiscard]] const RegisterFile& registers() const noexcept {
    return function_->registers();
  }
  [[nodiscard]] il::Function& function() noexcept { return *function_; }

  // Constants & leaves
  [[nodiscard]] ExprId constant(unsigned bits, uint64_t value);
  [[nodiscard]] ExprId boolean(bool value);
  [[nodiscard]] ExprId i8(uint8_t v) { return constant(8, v); }
  [[nodiscard]] ExprId i16(uint16_t v) { return constant(16, v); }
  [[nodiscard]] ExprId i32(uint32_t v) { return constant(32, v); }
  [[nodiscard]] ExprId i64(uint64_t v) { return constant(64, v); }
  [[nodiscard]] ExprId undef(Type type);
  [[nodiscard]] ExprId entryReg(RegId reg);

  // Pure expressions
  [[nodiscard]] ExprId unary(ExprOp op, ExprId operand);
  [[nodiscard]] ExprId binary(ExprOp op, ExprId lhs, ExprId rhs);
  [[nodiscard]] ExprId add(ExprId lhs, ExprId rhs) { return binary(ExprOp::Add, lhs, rhs); }
  [[nodiscard]] ExprId sub(ExprId lhs, ExprId rhs) { return binary(ExprOp::Sub, lhs, rhs); }
  [[nodiscard]] ExprId mul(ExprId lhs, ExprId rhs) { return binary(ExprOp::Mul, lhs, rhs); }
  [[nodiscard]] ExprId and_(ExprId lhs, ExprId rhs) { return binary(ExprOp::And, lhs, rhs); }
  [[nodiscard]] ExprId or_(ExprId lhs, ExprId rhs) { return binary(ExprOp::Or, lhs, rhs); }
  [[nodiscard]] ExprId xor_(ExprId lhs, ExprId rhs) { return binary(ExprOp::Xor, lhs, rhs); }
  [[nodiscard]] ExprId shl(ExprId lhs, ExprId rhs) { return binary(ExprOp::Shl, lhs, rhs); }
  [[nodiscard]] ExprId lshr(ExprId lhs, ExprId rhs) { return binary(ExprOp::ShrU, lhs, rhs); }
  [[nodiscard]] ExprId ashr(ExprId lhs, ExprId rhs) { return binary(ExprOp::ShrS, lhs, rhs); }
  [[nodiscard]] ExprId rotr(ExprId lhs, ExprId rhs) { return binary(ExprOp::RotR, lhs, rhs); }
  [[nodiscard]] ExprId rotl(ExprId lhs, ExprId rhs) { return binary(ExprOp::RotL, lhs, rhs); }
  [[nodiscard]] ExprId cmp(ExprOp cmpOp, ExprId lhs, ExprId rhs);
  [[nodiscard]] ExprId zext(unsigned toBits, ExprId operand);
  [[nodiscard]] ExprId sext(unsigned toBits, ExprId operand);
  [[nodiscard]] ExprId trunc(unsigned toBits, ExprId operand);
  [[nodiscard]] ExprId bitcast(Type toType, ExprId operand);
  [[nodiscard]] ExprId extract(unsigned toBits, ExprId operand, unsigned offsetBits);
  [[nodiscard]] ExprId concat(ExprId hi, ExprId lo);
  [[nodiscard]] ExprId select(ExprId cond, ExprId thenVal, ExprId elseVal);

  // Operations appended at current insertBlock_ and currentVa_
  [[nodiscard]] ExprId readReg(RegId reg);
  OpId writeReg(RegId reg, ExprId value);
  [[nodiscard]] ExprId load(unsigned widthBits, ExprId address);
  OpId store(unsigned widthBits, ExprId address, ExprId value);
  OpId branch(BlockId target);
  OpId condBranch(ExprId condition, BlockId trueTarget, BlockId falseTarget);
  OpId indirectBranch(ExprId target, std::span<const BlockId> targets = {});
  [[nodiscard]] ExprId call(ExprId target, std::span<const ExprId> args = {},
                            Type resultType = Type::integer(64));
  OpId callVoid(ExprId target, std::span<const ExprId> args = {});
  OpId ret(ExprId value = {});

  /// Rebuilds CFG edges, sets `maturity`, and runs `il::verify(*fn, maturity)`.
  /// Returns a diagnostic error if the constructed IL violates any invariant.
  [[nodiscard]] Result<std::unique_ptr<Function>> finalize(
      Maturity maturity = Maturity::Cfg);

 private:
  std::unique_ptr<Function> function_;
  BlockId insertBlock_{};
  uint64_t currentVa_ = 0;
};

}  // namespace xdec::il
