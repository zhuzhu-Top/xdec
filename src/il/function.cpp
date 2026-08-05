#include "xdec/il/function.h"

#include <algorithm>
#include <cstring>

#include "xdec/support/bits.h"

namespace xdec::il {

Function::Function(Arch arch, const RegisterFile& registers, uint64_t entryVa)
    : arch_(arch), registers_(&registers), entryVa_(entryVa) {
  // Pass identity zero must be the lifter, so that an op left at its default
  // origin reads correctly.
  passNames_.emplace_back("lift");
  passIndex_.emplace("lift", kPassLifter);
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

ExprId Function::intern(const Expr& expr) {
  XDEC_ASSERT(expr.operandCount <= kMaxExprOperands, "too many expression operands");
  const auto it = exprIndex_.find(expr);
  if (it != exprIndex_.end()) {
    return it->second;
  }
  const ExprId id = exprs_.emplace(expr);
  exprIndex_.emplace(expr, id);
  return id;
}

ExprId Function::constant(Type type, uint64_t value) {
  XDEC_ASSERT(type.isScalarInteger() || type.isFloat(),
              "constants are limited to scalar integers and floats");
  Expr expr;
  expr.op = ExprOp::Const;
  expr.type = type;
  // Normalise so that two spellings of the same constant intern to one node.
  expr.immediate = type.bits() >= 64 ? value : zeroExtend(value, type.bits());
  return intern(expr);
}

ExprId Function::boolean(bool value) {
  return constant(Type::boolean(), value ? 1u : 0u);
}

ExprId Function::valueRef(ValueId value) {
  XDEC_ASSERT(values_.contains(value), "value handle out of range");
  Expr expr;
  expr.op = ExprOp::Value;
  expr.type = values_[value].type;
  expr.immediate = value.index();
  return intern(expr);
}

ExprId Function::undefined(Type type) {
  Expr expr;
  expr.op = ExprOp::Undef;
  expr.type = type;
  return intern(expr);
}

ExprId Function::entryReg(RegId reg) {
  Expr expr;
  expr.op = ExprOp::EntryReg;
  expr.type = (*registers_)[reg].type();
  expr.immediate = reg.index();
  return intern(expr);
}

ExprId Function::unary(ExprOp op, ExprId operand) {
  XDEC_ASSERT(info(op).minArity <= 1 && info(op).maxArity >= 1, "op is not unary");
  Expr expr;
  expr.op = op;
  expr.type = exprs_[operand].type;
  expr.operandCount = 1;
  expr.operands[0] = operand;
  if (info(op).result == ResultRule::Boolean) {
    expr.type = Type::boolean();
  }
  return intern(expr);
}

ExprId Function::binary(ExprOp op, ExprId lhs, ExprId rhs) {
  XDEC_ASSERT(info(op).minArity <= 2 && info(op).maxArity >= 2, "op is not binary");
  Expr expr;
  expr.op = op;
  expr.operandCount = 2;
  expr.operands[0] = lhs;
  expr.operands[1] = rhs;
  expr.type = info(op).result == ResultRule::Boolean ? Type::boolean() : exprs_[lhs].type;
  return intern(expr);
}

ExprId Function::cast(ExprOp op, Type type, ExprId operand) {
  XDEC_ASSERT(info(op).result == ResultRule::Explicit, "op is not a cast");
  Expr expr;
  expr.op = op;
  expr.type = type;
  expr.operandCount = 1;
  expr.operands[0] = operand;
  return intern(expr);
}

ExprId Function::extract(Type type, ExprId operand, unsigned lowBit) {
  Expr expr;
  expr.op = ExprOp::Extract;
  expr.type = type;
  expr.operandCount = 1;
  expr.operands[0] = operand;
  expr.immediate = lowBit;
  return intern(expr);
}

ExprId Function::concat(Type type, ExprId high, ExprId low) {
  Expr expr;
  expr.op = ExprOp::Concat;
  expr.type = type;
  expr.operandCount = 2;
  expr.operands[0] = high;
  expr.operands[1] = low;
  return intern(expr);
}

ExprId Function::select(ExprId condition, ExprId ifTrue, ExprId ifFalse) {
  Expr expr;
  expr.op = ExprOp::Select;
  expr.type = exprs_[ifTrue].type;
  expr.operandCount = 3;
  expr.operands[0] = condition;
  expr.operands[1] = ifTrue;
  expr.operands[2] = ifFalse;
  return intern(expr);
}

ExprId Function::flagDef(FlagOp op, unsigned width, std::span<const ExprId> operands) {
  XDEC_ASSERT(!operands.empty() && operands.size() <= kMaxExprOperands,
              "flagdef takes one to three operands");
  Expr expr;
  expr.op = ExprOp::FlagDef;
  expr.type = Type::flags();
  expr.operandCount = static_cast<uint8_t>(operands.size());
  for (std::size_t index = 0; index < operands.size(); ++index) {
    expr.operands[index] = operands[index];
  }
  expr.immediate = packFlagDef(op, width);
  return intern(expr);
}

ExprId Function::flagCondition(ExprId flags, ConditionCode code) {
  Expr expr;
  expr.op = ExprOp::FlagCond;
  expr.type = Type::boolean();
  expr.operandCount = 1;
  expr.operands[0] = flags;
  expr.immediate = static_cast<uint64_t>(code);
  return intern(expr);
}

ExprId Function::flagBitOf(ExprId flags, FlagBitIndex bit) {
  Expr expr;
  expr.op = ExprOp::FlagBit;
  expr.type = Type::boolean();
  expr.operandCount = 1;
  expr.operands[0] = flags;
  expr.immediate = static_cast<uint64_t>(bit);
  return intern(expr);
}

bool Function::asConstant(ExprId id, uint64_t& out) const {
  if (!exprs_.contains(id)) {
    return false;
  }
  const Expr& expr = exprs_[id];
  if (expr.op != ExprOp::Const) {
    return false;
  }
  out = expr.immediate;
  return true;
}

bool Function::asConstantThroughCasts(ExprId id, uint64_t& out) const {
  // Bounded rather than recursive: a chain this long is not a value that got
  // widened twice, it is obfuscation, and following it further would not make
  // the answer more true.
  for (unsigned depth = 0; depth < 8; ++depth) {
    if (asConstant(id, out)) {
      return true;
    }
    if (!exprs_.contains(id)) {
      return false;
    }
    const Expr& expr = exprs_[id];
    switch (expr.op) {
      case ExprOp::ZExt:
      case ExprOp::SExt:
      case ExprOp::Trunc:
      case ExprOp::Bitcast:
        id = expr.operands[0];
        break;
      default:
        return false;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Blocks
// ---------------------------------------------------------------------------

BlockId Function::createBlock(uint64_t va) {
  Block block;
  block.va = va;
  block.endVa = va;
  const BlockId id = blocks_.emplace(std::move(block));
  // A second block claiming one address would make blockAt ambiguous; keep the
  // first and let the verifier report the duplicate.
  blocksByAddress_.emplace(va, id);
  if (!entryBlock_.valid()) {
    entryBlock_ = id;
  }
  return id;
}

BlockId Function::blockAt(uint64_t va) const {
  const auto it = blocksByAddress_.find(va);
  return it == blocksByAddress_.end() ? BlockId::invalid() : it->second;
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------

OpId Function::appendOp(BlockId blockId, Op op, std::span<const ExprId> operands,
                        std::span<const BlockId> targets) {
  XDEC_ASSERT(blocks_.contains(blockId), "block handle out of range");
  Block& target = blocks_[blockId];
  XDEC_ASSERT(target.ops.empty() || !ops_[target.ops.back()].isTerminator(),
              "cannot append after a block terminator");

  op.origin = currentPass_;
  op.operandStart = static_cast<uint32_t>(operandPool_.size());
  op.operandCount = static_cast<uint32_t>(operands.size());
  operandPool_.insert(operandPool_.end(), operands.begin(), operands.end());
  op.targetStart = static_cast<uint32_t>(targetPool_.size());
  op.targetCount = static_cast<uint32_t>(targets.size());
  targetPool_.insert(targetPool_.end(), targets.begin(), targets.end());

  const OpId id = ops_.emplace(op);
  target.ops.push_back(id);
  // endVa is one past the last instruction, which needs the instruction length.
  // Only the lifter knows that, so it sets the range explicitly rather than
  // having this guess from op addresses.
  return id;
}

ValueId Function::defineValue(Type type, OpId definition, BlockId block) {
  ValueInfo info;
  info.type = type;
  info.definition = definition;
  info.block = block;
  return values_.emplace(info);
}

ValueId Function::appendReadReg(BlockId block, uint64_t va, RegId reg) {
  XDEC_ASSERT(registers_->contains(reg), "unknown register");
  Op op;
  op.code = OpCode::ReadReg;
  op.va = va;
  op.type = (*registers_)[reg].type();
  op.payload = reg.index();
  const OpId id = appendOp(block, op, {}, {});
  const ValueId value = defineValue(ops_[id].type, id, block);
  ops_[id].result = value;
  return value;
}

OpId Function::appendWriteReg(BlockId block, uint64_t va, RegId reg, ExprId value) {
  XDEC_ASSERT(registers_->contains(reg), "unknown register");
  Op op;
  op.code = OpCode::WriteReg;
  op.va = va;
  op.payload = reg.index();
  const ExprId operands[] = {value};
  return appendOp(block, op, operands, {});
}

ValueId Function::appendLoad(BlockId block, uint64_t va, Type type, ExprId address) {
  Op op;
  op.code = OpCode::Load;
  op.va = va;
  op.type = type;
  const ExprId operands[] = {address};
  const OpId id = appendOp(block, op, operands, {});
  const ValueId value = defineValue(type, id, block);
  ops_[id].result = value;
  return value;
}

OpId Function::appendStore(BlockId block, uint64_t va, Type type, ExprId address, ExprId value) {
  Op op;
  op.code = OpCode::Store;
  op.va = va;
  op.type = type;
  const ExprId operands[] = {address, value};
  return appendOp(block, op, operands, {});
}

OpId Function::appendBranch(BlockId block, uint64_t va, BlockId target) {
  Op op;
  op.code = OpCode::Branch;
  op.va = va;
  const BlockId targets[] = {target};
  return appendOp(block, op, {}, targets);
}

OpId Function::appendCondBranch(BlockId block, uint64_t va, ExprId condition, BlockId ifTrue,
                                BlockId ifFalse) {
  Op op;
  op.code = OpCode::CondBranch;
  op.va = va;
  const ExprId operands[] = {condition};
  const BlockId targets[] = {ifTrue, ifFalse};
  return appendOp(block, op, operands, targets);
}

OpId Function::appendIndirectBranch(BlockId block, uint64_t va, ExprId target) {
  Op op;
  op.code = OpCode::IndirectBranch;
  op.va = va;
  const ExprId operands[] = {target};
  return appendOp(block, op, operands, {});
}

OpId Function::appendCall(BlockId block, uint64_t va, ExprId target,
                          Type resultType) {
  Op op;
  op.code = OpCode::Call;
  op.va = va;
  op.type = resultType;
  const ExprId operands[] = {target};
  const OpId id = appendOp(block, op, operands, {});
  if (!resultType.isVoid()) {
    const ValueId value = defineValue(resultType, id, block);
    ops_[id].result = value;
  }
  return id;
}

OpId Function::appendReturn(BlockId block, uint64_t va) {
  Op op;
  op.code = OpCode::Return;
  op.va = va;
  return appendOp(block, op, {}, {});
}

OpId Function::appendNop(BlockId block, uint64_t va) {
  Op op;
  op.code = OpCode::Nop;
  op.va = va;
  return appendOp(block, op, {}, {});
}

OpId Function::appendUnreachable(BlockId block, uint64_t va) {
  Op op;
  op.code = OpCode::Unreachable;
  op.va = va;
  return appendOp(block, op, {}, {});
}

OpId Function::appendUnimplemented(BlockId block, uint64_t va, std::string_view mnemonic) {
  Op op;
  op.code = OpCode::Unimplemented;
  op.va = va;
  op.payload = internName(mnemonic);
  return appendOp(block, op, {}, {});
}

OpId Function::appendIntrinsic(BlockId block, uint64_t va, std::string_view name, Type resultType,
                               std::span<const ExprId> arguments) {
  Op op;
  op.code = OpCode::Intrinsic;
  op.va = va;
  op.type = resultType;
  op.payload = internName(name);
  const OpId id = appendOp(block, op, arguments, {});
  if (!resultType.isVoid()) {
    const ValueId value = defineValue(resultType, id, block);
    ops_[id].result = value;
  }
  return id;
}

OpId Function::appendPhi(BlockId block, uint64_t va, Type type, std::span<const ExprId> incoming) {
  Op op;
  op.code = OpCode::Phi;
  op.va = va;
  op.type = type;
  const OpId id = appendOp(block, op, incoming, {});
  const ValueId value = defineValue(type, id, block);
  ops_[id].result = value;
  return id;
}

ValueId Function::prependPhi(BlockId blockId, uint64_t va, Type type) {
  XDEC_ASSERT(blocks_.contains(blockId), "block handle out of range");
  Op op;
  op.code = OpCode::Phi;
  op.va = va;
  op.type = type;
  op.origin = currentPass_;
  op.operandStart = static_cast<uint32_t>(operandPool_.size());
  op.operandCount = 0;
  op.targetStart = static_cast<uint32_t>(targetPool_.size());
  op.targetCount = 0;
  const OpId id = ops_.emplace(op);
  Block& block = blocks_[blockId];
  // Keep the phi prefix contiguous: insert after any phis already placed.
  const auto firstNonPhi =
      std::find_if(block.ops.begin(), block.ops.end(),
                   [this](OpId existing) { return ops_[existing].code != OpCode::Phi; });
  block.ops.insert(firstNonPhi, id);
  const ValueId value = defineValue(type, id, blockId);
  ops_[id].result = value;
  return value;
}

std::span<const ExprId> Function::operands(const Op& op) const {
  return std::span<const ExprId>{operandPool_}.subspan(op.operandStart, op.operandCount);
}

std::span<const BlockId> Function::targets(const Op& op) const {
  return std::span<const BlockId>{targetPool_}.subspan(op.targetStart, op.targetCount);
}

void Function::setTargets(OpId id, std::span<const BlockId> targets) {
  XDEC_ASSERT(ops_.contains(id), "op handle out of range");
  Op& op = ops_[id];
  XDEC_ASSERT(op.isTerminator(), "only a terminator has branch targets");
  // Appends a fresh range rather than editing in place, because the new list may
  // be longer than the old one. The abandoned range is left in the pool: a
  // function's IR is short-lived and arena-backed, so reclaiming it would cost
  // more than it saves.
  op.targetStart = static_cast<uint32_t>(targetPool_.size());
  op.targetCount = static_cast<uint32_t>(targets.size());
  targetPool_.insert(targetPool_.end(), targets.begin(), targets.end());
}

void Function::setOperands(OpId id, std::span<const ExprId> operands) {
  XDEC_ASSERT(ops_.contains(id), "op handle out of range");
  Op& op = ops_[id];
  op.operandStart = static_cast<uint32_t>(operandPool_.size());
  op.operandCount = static_cast<uint32_t>(operands.size());
  operandPool_.insert(operandPool_.end(), operands.begin(), operands.end());
}

ValueId Function::resultOf(OpId id) const {
  return ops_.contains(id) ? ops_[id].result : ValueId::invalid();
}

void Function::removeOp(BlockId blockId, OpId opId) {
  XDEC_ASSERT(blocks_.contains(blockId), "block handle out of range");
  XDEC_ASSERT(ops_.contains(opId), "op handle out of range");
  Block& block = blocks_[blockId];
  const auto found = std::find(block.ops.begin(), block.ops.end(), opId);
  XDEC_ASSERT(found != block.ops.end(), "op is not in the block");
  XDEC_ASSERT(!ops_[opId].isTerminator(), "cannot remove a block terminator");

  Op& op = ops_[opId];
  if (op.result.valid()) {
    // Tombstone the value: an invalid definition is the removed marker the
    // verifier recognises, while every handle stays valid.
    values_[op.result].definition = OpId::invalid();
    op.result = ValueId::invalid();
  }
  block.ops.erase(found);
  // A note describes the op, so it dies with it. Leaving it would let a later
  // op reusing the handle inherit a claim about code that is gone.
  notes_.erase(opId);
}

void Function::dropTerminator(BlockId blockId) {
  XDEC_ASSERT(blocks_.contains(blockId), "block handle out of range");
  Block& block = blocks_[blockId];
  XDEC_ASSERT(!block.ops.empty() && ops_[block.ops.back()].isTerminator(),
              "block does not end in a terminator");
  const OpId opId = block.ops.back();
  Op& op = ops_[opId];
  if (op.result.valid()) {
    values_[op.result].definition = OpId::invalid();
    op.result = ValueId::invalid();
  }
  block.ops.pop_back();
  notes_.erase(opId);
  // The successor list goes with it: whatever replaces this terminator states
  // its own edges, and a stale one would survive until the next rebuild.
  op.targetCount = 0;
}

// ---------------------------------------------------------------------------
// Annotations
// ---------------------------------------------------------------------------

void Function::annotate(OpId opId, std::string note) {
  XDEC_ASSERT(ops_.contains(opId), "op handle out of range");
  if (note.empty()) {
    notes_.erase(opId);
    return;
  }
  notes_[opId] = std::move(note);
}

void Function::appendNote(OpId opId, std::string_view note) {
  XDEC_ASSERT(ops_.contains(opId), "op handle out of range");
  if (note.empty()) {
    return;
  }
  std::string& existing = notes_[opId];
  if (!existing.empty()) {
    existing += "; ";
  }
  existing += note;
}

std::string_view Function::noteOn(OpId opId) const {
  const auto it = notes_.find(opId);
  return it == notes_.end() ? std::string_view{} : std::string_view{it->second};
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

uint32_t Function::internName(std::string_view name) {
  const std::string key{name};
  const auto it = nameIndex_.find(key);
  if (it != nameIndex_.end()) {
    return it->second;
  }
  const auto id = static_cast<uint32_t>(names_.size());
  names_.push_back(key);
  nameIndex_.emplace(key, id);
  return id;
}

std::string_view Function::nameOf(uint32_t id) const {
  return id < names_.size() ? std::string_view{names_[id]} : std::string_view{};
}

PassId Function::internPass(std::string_view name) {
  const std::string key{name};
  const auto it = passIndex_.find(key);
  if (it != passIndex_.end()) {
    return it->second;
  }
  const auto id = static_cast<PassId>(passNames_.size());
  passNames_.push_back(key);
  passIndex_.emplace(key, id);
  return id;
}

std::string_view Function::passName(PassId id) const {
  return id < passNames_.size() ? std::string_view{passNames_[id]} : std::string_view{"?"};
}

// ---------------------------------------------------------------------------
// CFG
// ---------------------------------------------------------------------------

void Function::rebuildEdges() {
  for (Block& block : blocks_) {
    block.successors.clear();
    block.predecessors.clear();
  }

  for (const BlockId blockId : blocks_.handles()) {
    Block& block = blocks_[blockId];
    if (block.ops.empty()) {
      continue;
    }
    const Op& terminator = ops_[block.ops.back()];
    if (!terminator.isTerminator()) {
      // An unterminated block has no edges to derive. The verifier reports it.
      continue;
    }
    for (const BlockId successor : targets(terminator)) {
      if (!blocks_.contains(successor)) {
        continue;
      }
      // Duplicate edges are meaningful for phi operand alignment (a conditional
      // branch whose arms coincide), so they are kept rather than deduplicated.
      block.successors.push_back(successor);
    }
  }

  for (const BlockId blockId : blocks_.handles()) {
    for (const BlockId successor : blocks_[blockId].successors) {
      blocks_[successor].predecessors.push_back(blockId);
    }
  }
}

std::vector<BlockId> Function::reversePostOrder() const {
  std::vector<BlockId> order;
  if (!blocks_.contains(entryBlock_)) {
    return order;
  }

  enum class State : uint8_t { Unvisited, Open, Done };
  std::vector<State> state(blocks_.size(), State::Unvisited);
  // Explicit stack: an obfuscated function can have thousands of blocks in one
  // strongly connected component, deep enough to overflow the call stack.
  std::vector<std::pair<BlockId, std::size_t>> stack;
  stack.emplace_back(entryBlock_, 0);
  state[entryBlock_.asSize()] = State::Open;

  while (!stack.empty()) {
    auto& [blockId, childIndex] = stack.back();
    const Block& block = blocks_[blockId];
    if (childIndex < block.successors.size()) {
      const BlockId successor = block.successors[childIndex++];
      if (state[successor.asSize()] == State::Unvisited) {
        state[successor.asSize()] = State::Open;
        stack.emplace_back(successor, 0);
      }
      continue;
    }
    state[blockId.asSize()] = State::Done;
    order.push_back(blockId);
    stack.pop_back();
  }

  std::reverse(order.begin(), order.end());
  return order;
}

}  // namespace xdec::il
