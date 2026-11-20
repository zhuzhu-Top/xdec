#include "xdec/il/builder.h"

namespace xdec::il {

FunctionBuilder::FunctionBuilder(Arch arch, RegisterFile registers, uint64_t entryVa,
                                 std::string name)
    : function_(std::make_unique<Function>(
          arch, std::make_shared<const RegisterFile>(std::move(registers)), entryVa)),
      currentVa_(entryVa) {
  if (!name.empty()) {
    function_->setName(std::move(name));
  }
}

BlockId FunctionBuilder::createBlock(uint64_t va) {
  const BlockId b = function_->createBlock(va);
  if (!insertBlock_.valid()) {
    insertBlock_ = b;
    currentVa_ = va;
  }
  return b;
}

ExprId FunctionBuilder::constant(unsigned bits, uint64_t value) {
  return function_->constant(Type::integer(bits), value);
}

ExprId FunctionBuilder::boolean(bool value) { return function_->boolean(value); }

ExprId FunctionBuilder::undef(Type type) { return function_->undefined(type); }

ExprId FunctionBuilder::entryReg(RegId reg) { return function_->entryReg(reg); }

ExprId FunctionBuilder::unary(ExprOp op, ExprId operand) {
  return function_->unary(op, operand);
}

ExprId FunctionBuilder::binary(ExprOp op, ExprId lhs, ExprId rhs) {
  return function_->binary(op, lhs, rhs);
}

ExprId FunctionBuilder::cmp(ExprOp cmpOp, ExprId lhs, ExprId rhs) {
  return function_->binary(cmpOp, lhs, rhs);
}

ExprId FunctionBuilder::zext(unsigned toBits, ExprId operand) {
  return function_->cast(ExprOp::ZExt, Type::integer(toBits), operand);
}

ExprId FunctionBuilder::sext(unsigned toBits, ExprId operand) {
  return function_->cast(ExprOp::SExt, Type::integer(toBits), operand);
}

ExprId FunctionBuilder::trunc(unsigned toBits, ExprId operand) {
  return function_->cast(ExprOp::Trunc, Type::integer(toBits), operand);
}

ExprId FunctionBuilder::bitcast(Type toType, ExprId operand) {
  return function_->cast(ExprOp::Bitcast, toType, operand);
}

ExprId FunctionBuilder::extract(unsigned toBits, ExprId operand, unsigned offsetBits) {
  return function_->extract(Type::integer(toBits), operand, offsetBits);
}

ExprId FunctionBuilder::concat(ExprId hi, ExprId lo) {
  const unsigned bits = function_->expr(hi).type.bits() + function_->expr(lo).type.bits();
  return function_->concat(Type::integer(bits), hi, lo);
}

ExprId FunctionBuilder::select(ExprId cond, ExprId thenVal, ExprId elseVal) {
  return function_->select(cond, thenVal, elseVal);
}

ExprId FunctionBuilder::readReg(RegId reg) {
  return function_->valueRef(function_->appendReadReg(insertBlock_, currentVa_, reg));
}

OpId FunctionBuilder::writeReg(RegId reg, ExprId value) {
  return function_->appendWriteReg(insertBlock_, currentVa_, reg, value);
}

ExprId FunctionBuilder::load(unsigned widthBits, ExprId address) {
  return function_->valueRef(
      function_->appendLoad(insertBlock_, currentVa_, Type::integer(widthBits), address));
}

OpId FunctionBuilder::store(unsigned widthBits, ExprId address, ExprId value) {
  return function_->appendStore(insertBlock_, currentVa_, Type::integer(widthBits), address,
                                value);
}

OpId FunctionBuilder::branch(BlockId target) {
  return function_->appendBranch(insertBlock_, currentVa_, target);
}

OpId FunctionBuilder::condBranch(ExprId condition, BlockId trueTarget, BlockId falseTarget) {
  return function_->appendCondBranch(insertBlock_, currentVa_, condition, trueTarget,
                                     falseTarget);
}

OpId FunctionBuilder::indirectBranch(ExprId target, std::span<const BlockId> targets) {
  const OpId op = function_->appendIndirectBranch(insertBlock_, currentVa_, target);
  if (!targets.empty()) {
    function_->setTargets(op, targets);
  }
  return op;
}

ExprId FunctionBuilder::call(ExprId target, std::span<const ExprId> args, Type resultType) {
  const OpId op = function_->appendCall(insertBlock_, currentVa_, target, resultType);
  if (!args.empty()) {
    std::vector<ExprId> all;
    all.reserve(1 + args.size());
    all.push_back(target);
    all.insert(all.end(), args.begin(), args.end());
    function_->setOperands(op, all);
  }
  return function_->valueRef(function_->op(op).result);
}

OpId FunctionBuilder::callVoid(ExprId target, std::span<const ExprId> args) {
  const OpId op = function_->appendCall(insertBlock_, currentVa_, target, Type{});
  if (!args.empty()) {
    std::vector<ExprId> all;
    all.reserve(1 + args.size());
    all.push_back(target);
    all.insert(all.end(), args.begin(), args.end());
    function_->setOperands(op, all);
  }
  return op;
}

OpId FunctionBuilder::ret(ExprId value) {
  const OpId op = function_->appendReturn(insertBlock_, currentVa_);
  if (value.valid()) {
    const ExprId operands[] = {value};
    function_->setOperands(op, operands);
  }
  return op;
}

Result<std::unique_ptr<Function>> FunctionBuilder::finalize(Maturity maturity) {
  function_->rebuildEdges();
  function_->setMaturity(maturity);
  const VerifyReport report = verify(*function_, maturity);
  if (!report.ok()) {
    return err(DiagCode::VerifyFailure, "FunctionBuilder IL verification failed at {}:\n{}",
               toString(maturity), report.format());
  }
  return std::move(function_);
}

}  // namespace xdec::il
