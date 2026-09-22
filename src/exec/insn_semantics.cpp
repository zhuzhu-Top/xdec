#include "xdec/exec/insn_semantics.h"

#include <algorithm>
#include <span>
#include <unordered_map>

#include "xdec/exec/exec_ir.h"
#include "xdec/il/register_file.h"

namespace xdec::exec {
namespace {

/// Walks one instruction's ops, turning the expressions they hang off into a
/// flat graph. Everything it cannot represent faithfully clears `complete`
/// instead of being approximated, because the consumer's use for a summary is
/// checking a recorded execution against it: a plausible-looking guess would
/// turn into a reported mismatch in code that is actually correct.
class Summarizer {
 public:
  Summarizer(const il::Function& function, const il::RegisterFile& registers)
      : function_(function), registers_(registers) {}

  InstructionSemantics run(const ExecInstruction& instruction) {
    for (const il::OpId opId : instruction.ops) {
      visit(function_.op(opId));
      if (overflowed_) {
        out_.nodes.clear();
        out_.effects.clear();
        out_.complete = false;
        return std::move(out_);
      }
    }
    return std::move(out_);
  }

 private:
  void visit(const il::Op& op) {
    switch (op.code) {
      case il::OpCode::ReadReg:
        visitReadReg(op);
        return;
      case il::OpCode::Load:
        bind(op.result, addNode(il::ExprOp::Value, op.type.bits(), {},
                                packLeaf(LeafKind::Load, loads_++)));
        return;
      case il::OpCode::WriteReg:
        visitWriteReg(op);
        return;
      case il::OpCode::Store:
        visitStore(op);
        return;

      // Control flow is described by ExecInstruction::flow, in the terms a
      // consumer building a call tree wants. Repeating it here as an effect
      // would be a second encoding of the same fact.
      case il::OpCode::Branch:
      case il::OpCode::CondBranch:
      case il::OpCode::IndirectBranch:
      case il::OpCode::Return:
      case il::OpCode::Nop:
        return;

      // A call's effect is whatever the callee does, which is unbounded. An
      // intrinsic is by definition an operation the IL declines to model.
      // Either way the instruction's result is not reconstructible from this
      // summary, and saying so is the point of the flag.
      default:
        out_.complete = false;
        if (op.definesValue() && op.result.valid()) {
          bind(op.result, addNode(il::ExprOp::Undef, op.type.bits(), {}, 0));
        }
        return;
    }
  }

  void visitReadReg(const il::Op& op) {
    // A read of a register this instruction has already written would mean the
    // leaf denotes the mid-instruction value, not the one the trace captured
    // before the instruction ran. Nothing in the AArch64 rules does this today;
    // if something starts to, the summary stops claiming to be complete rather
    // than quietly meaning the wrong register state.
    if (isWritten(op.reg())) {
      out_.complete = false;
    }
    bind(op.result, addNode(il::ExprOp::Value, op.type.bits(), {},
                            packLeaf(LeafKind::Register, op.reg().index())));
  }

  void visitWriteReg(const il::Op& op) {
    const std::span<const il::ExprId> operands = function_.operands(op);
    if (operands.size() != 1) {
      out_.complete = false;
      return;
    }
    SemanticEffect effect;
    effect.kind = SemanticEffect::Kind::WriteReg;
    effect.reg = op.reg();
    effect.valueNode = convert(operands[0]);
    out_.effects.push_back(effect);
    markWritten(op.reg());
  }

  void visitStore(const il::Op& op) {
    const std::span<const il::ExprId> operands = function_.operands(op);
    if (operands.size() != 2) {
      out_.complete = false;
      return;
    }
    SemanticEffect effect;
    effect.kind = SemanticEffect::Kind::Store;
    effect.addressNode = convert(operands[0]);
    effect.valueNode = convert(operands[1]);
    effect.width = static_cast<uint16_t>(op.type.bits());
    out_.effects.push_back(effect);
  }

  /// Emits `id` and everything it depends on, operands first. The expression
  /// pool is hash-consed, so memoising on ExprId reproduces the sharing rather
  /// than expanding a DAG into a tree.
  uint16_t convert(il::ExprId id) {
    if (const auto found = byExpr_.find(id); found != byExpr_.end()) {
      return found->second;
    }
    if (overflowed_) {
      return 0;
    }
    const il::Expr& expr = function_.expr(id);

    uint16_t operands[il::kMaxExprOperands] = {};
    if (expr.op == il::ExprOp::Value) {
      // Refers to a value defined by an op. It is this instruction's own read or
      // load when the lifter is behaving; anything else is out of scope and
      // becomes an honest unknown.
      const il::ValueId value{static_cast<uint32_t>(expr.immediate)};
      const auto found = byValue_.find(value);
      const uint16_t node = found != byValue_.end()
                                ? found->second
                                : unknown(expr.type.bits());
      byExpr_.emplace(id, node);
      return node;
    }
    if (expr.op == il::ExprOp::Undef || expr.op == il::ExprOp::EntryReg) {
      const uint16_t node = unknown(expr.type.bits());
      byExpr_.emplace(id, node);
      return node;
    }
    for (unsigned index = 0; index < expr.operandCount; ++index) {
      operands[index] = convert(expr.operand(index));
    }
    const uint16_t node =
        addNode(expr.op, expr.type.bits(), {operands, expr.operandCount}, expr.immediate);
    byExpr_.emplace(id, node);
    return node;
  }

  uint16_t unknown(unsigned width) {
    out_.complete = false;
    return addNode(il::ExprOp::Undef, width, {}, 0);
  }

  uint16_t addNode(il::ExprOp op, unsigned width, std::span<const uint16_t> operands,
                   uint64_t immediate) {
    if (out_.nodes.size() >= kMaxSemanticNodes) {
      overflowed_ = true;
      return 0;
    }
    SemanticNode node;
    node.op = op;
    node.width = static_cast<uint16_t>(width);
    node.operandCount = static_cast<uint8_t>(operands.size());
    std::ranges::copy(operands, std::begin(node.operands));
    node.immediate = immediate;
    out_.nodes.push_back(node);
    return static_cast<uint16_t>(out_.nodes.size() - 1);
  }

  void bind(il::ValueId value, uint16_t node) {
    if (value.valid()) {
      byValue_.insert_or_assign(value, node);
    }
  }

  void markWritten(il::RegId reg) {
    const il::RegId root = registers_.rootOf(reg);
    if (std::ranges::find(written_, root) == written_.end()) {
      written_.push_back(root);
    }
  }

  [[nodiscard]] bool isWritten(il::RegId reg) const {
    return std::ranges::find(written_, registers_.rootOf(reg)) != written_.end();
  }

  const il::Function& function_;
  const il::RegisterFile& registers_;
  InstructionSemantics out_{.nodes = {}, .effects = {}, .complete = true};
  std::unordered_map<il::ValueId, uint16_t> byValue_;
  std::unordered_map<il::ExprId, uint16_t> byExpr_;
  std::vector<il::RegId> written_;
  uint32_t loads_ = 0;
  bool overflowed_ = false;
};

}  // namespace

InstructionSemantics summarize(const il::Function& function,
                               const ExecInstruction& instruction) {
  return Summarizer(function, function.registers()).run(instruction);
}

}  // namespace xdec::exec
