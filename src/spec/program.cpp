#include "xdec/spec/program.h"

#include <format>

namespace xdec::spec {

std::string_view toString(Opcode op) noexcept {
  switch (op) {
    case Opcode::Halt:
      return "halt";
    case Opcode::ConstInt:
      return "const";
    case Opcode::Field:
      return "field";
    case Opcode::InsnPc:
      return "insn_pc";
    case Opcode::InsnLen:
      return "insn_len";
    case Opcode::Opword:
      return "opcode";
    case Opcode::IntUnary:
      return "iunary";
    case Opcode::IntBinary:
      return "ibinary";
    case Opcode::IntSelect:
      return "iselect";
    case Opcode::SExtInt:
      return "sextint";
    case Opcode::OnesInt:
      return "ones_int";
    case Opcode::HighestBitInt:
      return "highestbit_int";
    case Opcode::RorInt:
      return "ror_int";
    case Opcode::ReplicateInt:
      return "replicate_int";
    case Opcode::Move:
      return "move";
    case Opcode::Jump:
      return "jump";
    case Opcode::JumpIfZero:
      return "jz";
    case Opcode::Return:
      return "return";
    case Opcode::CallFn:
      return "callfn";
    case Opcode::Imm:
      return "imm";
    case Opcode::Undef:
      return "undef";
    case Opcode::ExprUnary:
      return "eunary";
    case Opcode::ExprBinary:
      return "ebinary";
    case Opcode::Cast:
      return "cast";
    case Opcode::Extract:
      return "extract";
    case Opcode::Concat:
      return "concat";
    case Opcode::Select:
      return "select";
    case Opcode::FlagDef:
      return "flagdef";
    case Opcode::FlagCond:
      return "flagcond";
    case Opcode::FlagBit:
      return "flagbit";
    case Opcode::ReadReg:
      return "read";
    case Opcode::WriteReg:
      return "write";
    case Opcode::Load:
      return "load";
    case Opcode::Store:
      return "store";
    case Opcode::Branch:
      return "br";
    case Opcode::CondBranch:
      return "brc";
    case Opcode::IndirectBranch:
      return "brind";
    case Opcode::CallAddr:
      return "call";
    case Opcode::IndirectCall:
      return "callind";
    case Opcode::Ret:
      return "ret";
    case Opcode::Nop:
      return "nop";
    case Opcode::Unreachable:
      return "unreachable";
    case Opcode::Intrinsic:
      return "intrinsic";
    case Opcode::Unimplemented:
      return "unimplemented";
  }
  return "?";
}

std::string_view toString(IntOp op) noexcept {
  switch (op) {
    case IntOp::Negate:
      return "neg";
    case IntOp::Not:
      return "not";
    case IntOp::LogicalNot:
      return "lnot";
    case IntOp::Add:
      return "add";
    case IntOp::Sub:
      return "sub";
    case IntOp::Mul:
      return "mul";
    case IntOp::DivU:
      return "divu";
    case IntOp::DivS:
      return "divs";
    case IntOp::RemU:
      return "remu";
    case IntOp::RemS:
      return "rems";
    case IntOp::And:
      return "and";
    case IntOp::Or:
      return "or";
    case IntOp::Xor:
      return "xor";
    case IntOp::Shl:
      return "shl";
    case IntOp::ShrU:
      return "shru";
    case IntOp::ShrS:
      return "shrs";
    case IntOp::Equal:
      return "eq";
    case IntOp::NotEqual:
      return "ne";
    case IntOp::LessU:
      return "ltu";
    case IntOp::LessEqualU:
      return "leu";
    case IntOp::LessS:
      return "lts";
    case IntOp::LessEqualS:
      return "les";
    case IntOp::GreaterU:
      return "gtu";
    case IntOp::GreaterEqualU:
      return "geu";
    case IntOp::GreaterS:
      return "gts";
    case IntOp::GreaterEqualS:
      return "ges";
    case IntOp::LogicalAnd:
      return "land";
    case IntOp::LogicalOr:
      return "lor";
  }
  return "?";
}

std::string_view toString(AsmStyle style) noexcept {
  switch (style) {
    case AsmStyle::Plain:
      return "plain";
    case AsmStyle::Register:
      return "reg";
    case AsmStyle::RegisterSp:
      return "regsp";
    case AsmStyle::VectorRegister:
      return "vreg";
    case AsmStyle::Extend:
      return "extend";
    case AsmStyle::Hex:
      return "hex";
    case AsmStyle::Dec:
      return "dec";
    case AsmStyle::Condition:
      return "cond";
    case AsmStyle::Label:
      return "label";
    case AsmStyle::Shift:
      return "shift";
  }
  return "plain";
}

void SpecProgram::rebuildDecoder() { decoder = buildDecisionTree(patterns, insnWidth); }

std::string_view SpecProgram::string(uint32_t index) const {
  return index < strings.size() ? std::string_view{strings[index]} : std::string_view{};
}

const ProgramRegFile* SpecProgram::findRegFile(std::string_view wanted) const {
  for (const ProgramRegFile& file : regFiles) {
    if (file.name == wanted) {
      return &file;
    }
  }
  return nullptr;
}

int SpecProgram::findInsn(std::string_view wanted) const {
  for (std::size_t index = 0; index < instructions.size(); ++index) {
    if (instructions[index].name == wanted) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

std::string SpecProgram::summary() const {
  return std::format(
      "{}: {} instructions, {} functions, {} registers, {} bytecode ops, decoder {} nodes "
      "depth {} worst leaf {}",
      name, instructions.size(), functions.size(), registers.size(), code.size(),
      decoder.nodeCount(), decoder.depth(), decoder.worstLeaf());
}

}  // namespace xdec::spec
