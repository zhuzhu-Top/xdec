#include "xdec/spec/compile.h"

#include <algorithm>
#include <format>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "xdec/spec/parse.h"
#include "xdec/support/bits.h"

namespace xdec::spec {
namespace {

/// Maps a builtin's name to the opcode that implements it. The checker has
/// already verified arity and types, so this is a pure translation table.
struct BuiltinLowering {
  Opcode op;
  /// Sub-operation, for the opcodes that need one.
  uint8_t aux;
  /// The builtin produces no value.
  bool isEffect;
};

using il::ExprOp;
using il::FlagOp;

const std::unordered_map<std::string_view, BuiltinLowering>& loweringTable() {
  static const std::unordered_map<std::string_view, BuiltinLowering> kTable = {
      {"imm", {Opcode::Imm, 0, false}},
      {"undef", {Opcode::Undef, 0, false}},
      {"zext", {Opcode::Cast, static_cast<uint8_t>(ExprOp::ZExt), false}},
      {"sext", {Opcode::Cast, static_cast<uint8_t>(ExprOp::SExt), false}},
      {"trunc", {Opcode::Cast, static_cast<uint8_t>(ExprOp::Trunc), false}},
      {"bitcast_int", {Opcode::Cast, static_cast<uint8_t>(ExprOp::Bitcast), false}},
      // The 0x80 bit says the result is a float; the ExprOp is the same either
      // way, and a bitcast is exactly the case where the operand cannot say.
      {"bitcast_float", {Opcode::Cast, static_cast<uint8_t>(ExprOp::Bitcast) | 0x80, false}},
      {"extract", {Opcode::Extract, 0, false}},
      {"concat", {Opcode::Concat, 0, false}},
      {"select", {Opcode::Select, 0, false}},
      {"select_flags", {Opcode::Select, 0, false}},
      {"clz", {Opcode::ExprUnary, static_cast<uint8_t>(ExprOp::Clz), false}},
      {"ctz", {Opcode::ExprUnary, static_cast<uint8_t>(ExprOp::Ctz), false}},
      {"popcount", {Opcode::ExprUnary, static_cast<uint8_t>(ExprOp::PopCount), false}},
      {"bswap", {Opcode::ExprUnary, static_cast<uint8_t>(ExprOp::ByteSwap), false}},
      {"brev", {Opcode::ExprUnary, static_cast<uint8_t>(ExprOp::BitReverse), false}},
      {"rotr", {Opcode::ExprBinary, static_cast<uint8_t>(ExprOp::RotR), false}},
      {"rotl", {Opcode::ExprBinary, static_cast<uint8_t>(ExprOp::RotL), false}},
      {"mulhi_u", {Opcode::ExprBinary, static_cast<uint8_t>(ExprOp::MulHiU), false}},
      {"mulhi_s", {Opcode::ExprBinary, static_cast<uint8_t>(ExprOp::MulHiS), false}},
      {"flagdef_add", {Opcode::FlagDef, static_cast<uint8_t>(FlagOp::Add), false}},
      {"flagdef_sub", {Opcode::FlagDef, static_cast<uint8_t>(FlagOp::Sub), false}},
      {"flagdef_adc", {Opcode::FlagDef, static_cast<uint8_t>(FlagOp::AddCarry), false}},
      {"flagdef_sbc", {Opcode::FlagDef, static_cast<uint8_t>(FlagOp::SubCarry), false}},
      {"flagdef_logic", {Opcode::FlagDef, static_cast<uint8_t>(FlagOp::Logical), false}},
      {"flagconst", {Opcode::FlagDef, static_cast<uint8_t>(FlagOp::Const), false}},
      {"cond", {Opcode::FlagCond, 0, false}},
      {"flagbit", {Opcode::FlagBit, 0, false}},
      {"load", {Opcode::Load, 0, false}},
      {"fadd", {Opcode::ExprBinary, static_cast<uint8_t>(ExprOp::FAdd), false}},
      {"fsub", {Opcode::ExprBinary, static_cast<uint8_t>(ExprOp::FSub), false}},
      {"fmul", {Opcode::ExprBinary, static_cast<uint8_t>(ExprOp::FMul), false}},
      {"fdiv", {Opcode::ExprBinary, static_cast<uint8_t>(ExprOp::FDiv), false}},
      {"fneg", {Opcode::ExprUnary, static_cast<uint8_t>(ExprOp::FNeg), false}},
      {"fabs", {Opcode::ExprUnary, static_cast<uint8_t>(ExprOp::FAbs), false}},
      {"fsqrt", {Opcode::ExprUnary, static_cast<uint8_t>(ExprOp::FSqrt), false}},
      {"fcmp_eq", {Opcode::ExprBinary, static_cast<uint8_t>(ExprOp::FCmpEq), false}},
      {"fcmp_lt", {Opcode::ExprBinary, static_cast<uint8_t>(ExprOp::FCmpLt), false}},
      {"fcmp_le", {Opcode::ExprBinary, static_cast<uint8_t>(ExprOp::FCmpLe), false}},
      {"fcmp_uno", {Opcode::ExprBinary, static_cast<uint8_t>(ExprOp::FCmpUnordered), false}},
      {"inttofp_s", {Opcode::Cast, static_cast<uint8_t>(ExprOp::IntToFpS), false}},
      {"inttofp_u", {Opcode::Cast, static_cast<uint8_t>(ExprOp::IntToFpU), false}},
      {"fptoint_s", {Opcode::Cast, static_cast<uint8_t>(ExprOp::FpToIntS), false}},
      {"fptoint_u", {Opcode::Cast, static_cast<uint8_t>(ExprOp::FpToIntU), false}},
      {"fpconvert", {Opcode::Cast, static_cast<uint8_t>(ExprOp::FpConvert), false}},
      {"sextint", {Opcode::SExtInt, 0, false}},
      {"ones_int", {Opcode::OnesInt, 0, false}},
      {"highestbit_int", {Opcode::HighestBitInt, 0, false}},
      {"ror_int", {Opcode::RorInt, 0, false}},
      {"replicate_int", {Opcode::ReplicateInt, 0, false}},
      {"store", {Opcode::Store, 0, true}},
      {"branch", {Opcode::Branch, 0, true}},
      {"cbranch", {Opcode::CondBranch, 0, true}},
      {"brind", {Opcode::IndirectBranch, 0, true}},
      {"call", {Opcode::CallAddr, 0, true}},
      {"callind", {Opcode::IndirectCall, 0, true}},
      {"ret", {Opcode::Ret, 0, true}},
      {"nop", {Opcode::Nop, 0, true}},
      {"unreachable", {Opcode::Unreachable, 0, true}},
      {"intrinsic", {Opcode::Intrinsic, 0, true}},
      {"intrinsic_value", {Opcode::Intrinsic, 0, false}},
      {"unimplemented", {Opcode::Unimplemented, 0, true}},
  };
  return kTable;
}

[[nodiscard]] IntOp toIntOp(UnaryOp op) noexcept {
  switch (op) {
    case UnaryOp::Negate:
      return IntOp::Negate;
    case UnaryOp::BitNot:
      return IntOp::Not;
    case UnaryOp::LogicalNot:
      return IntOp::LogicalNot;
  }
  return IntOp::Not;
}

[[nodiscard]] IntOp toIntOp(BinaryOp op) noexcept {
  switch (op) {
    case BinaryOp::Add:
      return IntOp::Add;
    case BinaryOp::Sub:
      return IntOp::Sub;
    case BinaryOp::Mul:
      return IntOp::Mul;
    case BinaryOp::DivU:
      return IntOp::DivU;
    case BinaryOp::DivS:
      return IntOp::DivS;
    case BinaryOp::RemU:
      return IntOp::RemU;
    case BinaryOp::RemS:
      return IntOp::RemS;
    case BinaryOp::And:
      return IntOp::And;
    case BinaryOp::Or:
      return IntOp::Or;
    case BinaryOp::Xor:
      return IntOp::Xor;
    case BinaryOp::Shl:
      return IntOp::Shl;
    case BinaryOp::ShrU:
      return IntOp::ShrU;
    case BinaryOp::ShrS:
      return IntOp::ShrS;
    case BinaryOp::Equal:
      return IntOp::Equal;
    case BinaryOp::NotEqual:
      return IntOp::NotEqual;
    case BinaryOp::LessU:
      return IntOp::LessU;
    case BinaryOp::LessEqualU:
      return IntOp::LessEqualU;
    case BinaryOp::LessS:
      return IntOp::LessS;
    case BinaryOp::LessEqualS:
      return IntOp::LessEqualS;
    case BinaryOp::GreaterU:
      return IntOp::GreaterU;
    case BinaryOp::GreaterEqualU:
      return IntOp::GreaterEqualU;
    case BinaryOp::GreaterS:
      return IntOp::GreaterS;
    case BinaryOp::GreaterEqualS:
      return IntOp::GreaterEqualS;
    case BinaryOp::LogicalAnd:
      return IntOp::LogicalAnd;
    case BinaryOp::LogicalOr:
      return IntOp::LogicalOr;
  }
  return IntOp::Add;
}

/// The IL expression operator an infix operator lowers to, for `bits` operands.
[[nodiscard]] bool toExprOp(BinaryOp op, ExprOp& out) noexcept {
  switch (op) {
    case BinaryOp::Add:
      out = ExprOp::Add;
      return true;
    case BinaryOp::Sub:
      out = ExprOp::Sub;
      return true;
    case BinaryOp::Mul:
      out = ExprOp::Mul;
      return true;
    case BinaryOp::DivU:
      out = ExprOp::DivU;
      return true;
    case BinaryOp::DivS:
      out = ExprOp::DivS;
      return true;
    case BinaryOp::RemU:
      out = ExprOp::RemU;
      return true;
    case BinaryOp::RemS:
      out = ExprOp::RemS;
      return true;
    case BinaryOp::And:
      out = ExprOp::And;
      return true;
    case BinaryOp::Or:
      out = ExprOp::Or;
      return true;
    case BinaryOp::Xor:
      out = ExprOp::Xor;
      return true;
    case BinaryOp::Shl:
      out = ExprOp::Shl;
      return true;
    case BinaryOp::ShrU:
      out = ExprOp::ShrU;
      return true;
    case BinaryOp::ShrS:
      out = ExprOp::ShrS;
      return true;
    case BinaryOp::Equal:
      out = ExprOp::CmpEq;
      return true;
    case BinaryOp::NotEqual:
      out = ExprOp::CmpNe;
      return true;
    case BinaryOp::LessU:
      out = ExprOp::CmpLtU;
      return true;
    case BinaryOp::LessEqualU:
      out = ExprOp::CmpLeU;
      return true;
    case BinaryOp::LessS:
      out = ExprOp::CmpLtS;
      return true;
    case BinaryOp::LessEqualS:
      out = ExprOp::CmpLeS;
      return true;
    default:
      // The reversed comparisons are lowered by swapping operands, and the
      // logical connectives never reach here: the checker restricts them to
      // compile-time integers.
      return false;
  }
}

[[nodiscard]] bool isReversedComparison(BinaryOp op) noexcept {
  return op == BinaryOp::GreaterU || op == BinaryOp::GreaterEqualU ||
         op == BinaryOp::GreaterS || op == BinaryOp::GreaterEqualS;
}

[[nodiscard]] BinaryOp reverseComparison(BinaryOp op) noexcept {
  switch (op) {
    case BinaryOp::GreaterU:
      return BinaryOp::LessU;
    case BinaryOp::GreaterEqualU:
      return BinaryOp::LessEqualU;
    case BinaryOp::GreaterS:
      return BinaryOp::LessS;
    case BinaryOp::GreaterEqualS:
      return BinaryOp::LessEqualS;
    default:
      return op;
  }
}

[[nodiscard]] bool parseAsmStyle(std::string_view text, AsmStyle& out) noexcept {
  if (text.empty()) {
    out = AsmStyle::Plain;
  } else if (text == "reg") {
    out = AsmStyle::Register;
  } else if (text == "regsp") {
    out = AsmStyle::RegisterSp;
  } else if (text == "vreg") {
    out = AsmStyle::VectorRegister;
  } else if (text == "hex") {
    out = AsmStyle::Hex;
  } else if (text == "dec") {
    out = AsmStyle::Dec;
  } else if (text == "cond") {
    out = AsmStyle::Condition;
  } else if (text == "label") {
    out = AsmStyle::Label;
  } else if (text == "shift") {
    out = AsmStyle::Shift;
  } else if (text == "extend") {
    out = AsmStyle::Extend;
  } else {
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Compiler
// ---------------------------------------------------------------------------

class Compiler {
 public:
  Compiler(const Module& module, const CheckedModule& checked)
      : module_(module), checked_(checked) {
    program_ = std::make_unique<SpecProgram>();
  }

  Result<std::unique_ptr<SpecProgram>> run() {
    program_->name = module_.sourceName;
    program_->arch = module_.arch.arch;
    program_->endian = module_.arch.endian;
    program_->insnWidth = module_.arch.insnWidth;
    program_->pointerBits = module_.arch.pointerBits;
    program_->registers = checked_.registers;
    program_->patterns = checked_.patterns;

    for (const RegFileBinding& file : checked_.regFiles) {
      ProgramRegFile compiled;
      compiled.name = file.name;
      compiled.base = file.base;
      compiled.count = file.count;
      compiled.bits = file.bits;
      compiled.viewBase = file.viewBase;
      compiled.role = file.role;
      for (const unsigned bits : file.viewBits) {
        compiled.viewBits.push_back(bits);
      }
      program_->regFiles.push_back(std::move(compiled));
    }

    // Function indices have to exist before any body is compiled, because a body
    // may call a function declared later in the file.
    for (std::size_t index = 0; index < module_.functions.size(); ++index) {
      functionIndex_[module_.functions[index].name] = static_cast<uint32_t>(index);
      ProgramFn compiled;
      compiled.name = module_.functions[index].name;
      compiled.paramCount = static_cast<uint16_t>(module_.functions[index].params.size());
      program_->functions.push_back(std::move(compiled));
    }

    for (std::size_t index = 0; index < module_.functions.size(); ++index) {
      XDEC_TRY(const Body body, compileFunction(module_.functions[index]));
      program_->functions[index].body = body;
    }

    for (const InsnDecl& insn : module_.instructions) {
      XDEC_TRY(ProgramInsn compiled, compileInsn(insn));
      program_->instructions.push_back(std::move(compiled));
    }

    program_->rebuildDecoder();
    return std::move(program_);
  }

 private:
  [[nodiscard]] Unexpected fail(SourceLoc loc, std::string message) const {
    return err(Diag{DiagCode::Internal, std::move(message)}.note(
        std::format("{}:{}", module_.sourceName, loc.toString())));
  }

  uint32_t internString(std::string_view text) {
    const auto it = stringIndex_.find(std::string{text});
    if (it != stringIndex_.end()) {
      return it->second;
    }
    const auto index = static_cast<uint32_t>(program_->strings.size());
    program_->strings.emplace_back(text);
    stringIndex_.emplace(text, index);
    return index;
  }

  // -- body emission --------------------------------------------------------

  /// Slots are allocated as a stack. A block scope restores the high-water mark
  /// on exit, which is correct because a binding made inside a branch cannot be
  /// referred to after it.
  struct Scope {
    explicit Scope(Compiler& compiler)
        : compiler_(compiler),
          savedTop_(compiler.slotTop_),
          savedNames_(compiler.names_),
          savedIntSlots_(compiler.intSlots_) {}
    ~Scope() {
      compiler_.slotTop_ = savedTop_;
      compiler_.names_ = std::move(savedNames_);
      // Slot numbers are reused across sibling scopes, so the record of which
      // ones hold integers has to be unwound with them.
      compiler_.intSlots_ = std::move(savedIntSlots_);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    Compiler& compiler_;
    uint16_t savedTop_;
    std::unordered_map<std::string, uint16_t> savedNames_;
    std::unordered_set<uint16_t> savedIntSlots_;
  };

  uint16_t allocSlot() {
    const uint16_t slot = slotTop_++;
    slotHighWater_ = std::max(slotHighWater_, slotTop_);
    return slot;
  }

  uint32_t emit(Insn insn) {
    const auto index = static_cast<uint32_t>(program_->code.size());
    program_->code.push_back(insn);
    return index;
  }

  /// `site` indexes the shared code array; `target` is already body-relative,
  /// which is how the interpreter reads it.
  void patchJump(uint32_t site, uint32_t target) { program_->code[site].imm = target; }

  [[nodiscard]] uint32_t here() const {
    return static_cast<uint32_t>(program_->code.size()) - bodyStart_;
  }

  void beginBody() {
    bodyStart_ = static_cast<uint32_t>(program_->code.size());
    slotTop_ = 0;
    slotHighWater_ = 0;
    names_.clear();
    intSlots_.clear();
  }

  [[nodiscard]] Body endBody() {
    Body body;
    body.start = bodyStart_;
    body.length = static_cast<uint32_t>(program_->code.size()) - bodyStart_;
    body.slots = slotHighWater_;
    return body;
  }

  // -- functions and instructions -------------------------------------------

  Result<Body> compileFunction(const FnDecl& function) {
    beginBody();
    // Parameters occupy the first slots, in order, so a call can copy arguments
    // into a contiguous run and the callee finds them where it expects.
    for (const ParamDecl& param : function.params) {
      const uint16_t slot = allocSlot();
      names_[param.name] = slot;
      if (param.type.kind == TypeKind::Int) {
        intSlots_.insert(slot);
      }
    }
    XDEC_TRY_VOID(compileStatements(function.body));
    emit(Insn{Opcode::Halt, 0, kNoSlot, kNoSlot, kNoSlot, kNoSlot, 0});
    return endBody();
  }

  Result<ProgramInsn> compileInsn(const InsnDecl& insn) {
    ProgramInsn compiled;
    compiled.name = insn.name;

    unsigned offset = insn.encoding.width;
    for (const EncodingItem& item : insn.encoding.items) {
      offset -= item.bits;
      if (item.isLiteral || item.isWildcard) {
        continue;
      }
      compiled.fields.push_back(ProgramField{item.field, static_cast<uint8_t>(offset),
                                             static_cast<uint8_t>(item.bits)});
    }

    currentFields_ = &compiled.fields;

    for (const ExprPtr& guard : insn.requires_) {
      beginBody();
      bindFields(compiled.fields);
      XDEC_TRY(const uint16_t slot, compileExpr(*guard));
      emit(Insn{Opcode::Return, 0, kNoSlot, slot, kNoSlot, kNoSlot, 0});
      compiled.guards.push_back(endBody());
    }

    if (insn.asmTemplate.has_value()) {
      XDEC_TRY(compiled.disassembly, compileAsm(*insn.asmTemplate, compiled.fields));
    }

    beginBody();
    bindFields(compiled.fields);
    XDEC_TRY_VOID(compileStatements(insn.semantics));
    emit(Insn{Opcode::Halt, 0, kNoSlot, kNoSlot, kNoSlot, kNoSlot, 0});
    compiled.semantics = endBody();

    currentFields_ = nullptr;
    return compiled;
  }

  /// Fields are read on demand rather than pre-loaded into slots: most rules use
  /// only a few of them, and an unused load is an unused slot.
  void bindFields(const std::vector<ProgramField>& fields) {
    fieldIndex_.clear();
    for (std::size_t index = 0; index < fields.size(); ++index) {
      fieldIndex_[fields[index].name] = static_cast<uint32_t>(index);
    }
  }

  Result<ProgramAsm> compileAsm(const AsmTemplate& tmpl,
                                const std::vector<ProgramField>& fields) {
    ProgramAsm compiled;
    compiled.start = static_cast<uint32_t>(program_->asmPieces.size());
    XDEC_TRY_VOID(compileAsmPieces(tmpl.pieces, fields));
    compiled.length = static_cast<uint32_t>(program_->asmPieces.size()) - compiled.start;
    return compiled;
  }

  Result<void> compileAsmPieces(const std::vector<AsmPiecePtr>& pieces,
                                const std::vector<ProgramField>& fields) {
    for (const AsmPiecePtr& piece : pieces) {
      switch (piece->kind) {
        case AsmPieceKind::Text: {
          ProgramAsmPiece compiled;
          compiled.op = AsmPieceOp::Text;
          compiled.text = internString(piece->text);
          program_->asmPieces.push_back(std::move(compiled));
          break;
        }
        case AsmPieceKind::Substitution: {
          ProgramAsmPiece compiled;
          compiled.op = AsmPieceOp::Substitution;
          if (!parseAsmStyle(piece->style, compiled.style)) {
            return fail(piece->loc, std::format("unknown asm style '{}'", piece->style));
          }
          XDEC_TRY(compiled.value, compileValueBody(*piece->value, fields));
          if (piece->styleArgument != nullptr) {
            XDEC_TRY(compiled.styleArgument, compileValueBody(*piece->styleArgument, fields));
          } else if (compiled.style == AsmStyle::Label) {
            // A branch displacement is signed, and the width it is signed at is
            // the field's, which only the encoding knows. Without this the
            // renderer would print backward branches as enormous forward ones.
            XDEC_TRY(const uint32_t bits, labelFieldBits(*piece->value, fields));
            XDEC_TRY(compiled.styleArgument, compileConstantBody(bits));
          }
          program_->asmPieces.push_back(std::move(compiled));
          break;
        }
        case AsmPieceKind::OptionalGroup: {
          ProgramAsmPiece begin;
          begin.op = AsmPieceOp::GroupBegin;
          program_->asmPieces.push_back(std::move(begin));
          XDEC_TRY_VOID(compileAsmPieces(piece->group, fields));
          ProgramAsmPiece end;
          end.op = AsmPieceOp::GroupEnd;
          program_->asmPieces.push_back(std::move(end));
          break;
        }
      }
    }
    return ok();
  }

  /// The width a `:label` substitution should be sign-extended at. Only a bare
  /// field name is supported: anything else has already been widened by the
  /// expression that produced it, so signing it again would be wrong.
  Result<uint32_t> labelFieldBits(const Expr& expr, const std::vector<ProgramField>& fields) {
    if (expr.kind == ExprKind::Name) {
      for (const ProgramField& field : fields) {
        if (field.name == expr.name) {
          return field.bits;
        }
      }
    }
    return fail(expr.loc, "a label operand must be an encoding field");
  }

  Result<Body> compileConstantBody(uint64_t value) {
    beginBody();
    Body body;
    body.start = static_cast<uint32_t>(program_->code.size());
    Insn insn;
    insn.op = Opcode::ConstInt;
    insn.dest = allocSlot();
    insn.imm = value;
    program_->code.push_back(insn);
    Insn ret;
    ret.op = Opcode::Return;
    ret.a = insn.dest;
    program_->code.push_back(ret);
    body.length = static_cast<uint32_t>(program_->code.size()) - body.start;
    body.slots = slotHighWater_;
    return body;
  }

  Result<Body> compileValueBody(const Expr& expr, const std::vector<ProgramField>& fields) {
    beginBody();
    bindFields(fields);
    XDEC_TRY(const uint16_t slot, compileExpr(expr));
    emit(Insn{Opcode::Return, 0, kNoSlot, slot, kNoSlot, kNoSlot, 0});
    return endBody();
  }

  // -- statements -----------------------------------------------------------

  Result<void> compileStatements(const std::vector<StmtPtr>& body) {
    for (const StmtPtr& statement : body) {
      XDEC_TRY_VOID(compileStatement(*statement));
    }
    return ok();
  }

  Result<void> compileStatement(const Stmt& statement) {
    switch (statement.kind) {
      case StmtKind::Let: {
        const bool isInt = isIntExpr(*statement.value);
        XDEC_TRY(const uint16_t slot, compileExpr(*statement.value));
        // A fresh slot rather than an alias, so that a later rebinding of the
        // source cannot change what this name means.
        const uint16_t bound = allocSlot();
        emit(Insn{Opcode::Move, 0, bound, slot, kNoSlot, kNoSlot, 0});
        names_[statement.name] = bound;
        if (isInt) {
          intSlots_.insert(bound);
        } else {
          intSlots_.erase(bound);
        }
        return ok();
      }

      case StmtKind::Assign:
        return compileAssign(statement);

      case StmtKind::Effect: {
        XDEC_TRY(const uint16_t slot, compileExpr(*statement.value));
        (void)slot;
        return ok();
      }

      case StmtKind::If: {
        XDEC_TRY(const uint16_t condition, compileExpr(*statement.value));
        const uint32_t branchToElse =
            emit(Insn{Opcode::JumpIfZero, 0, kNoSlot, condition, kNoSlot, kNoSlot, 0});
        {
          Scope scope{*this};
          XDEC_TRY_VOID(compileStatements(statement.thenBody));
        }
        if (statement.elseBody.empty()) {
          patchJump(branchToElse, here());
          return ok();
        }
        const uint32_t branchToEnd =
            emit(Insn{Opcode::Jump, 0, kNoSlot, kNoSlot, kNoSlot, kNoSlot, 0});
        patchJump(branchToElse, here());
        {
          Scope scope{*this};
          XDEC_TRY_VOID(compileStatements(statement.elseBody));
        }
        patchJump(branchToEnd, here());
        return ok();
      }

      case StmtKind::Return: {
        uint16_t slot = kNoSlot;
        if (statement.value != nullptr) {
          XDEC_TRY(slot, compileExpr(*statement.value));
        }
        emit(Insn{Opcode::Return, 0, kNoSlot, slot, kNoSlot, kNoSlot, 0});
        return ok();
      }
    }
    return fail(statement.loc, "unhandled statement kind");
  }

  Result<void> compileAssign(const Stmt& statement) {
    XDEC_TRY(const uint16_t value, compileExpr(*statement.value));
    const Expr& target = *statement.target;

    if (target.kind == ExprKind::Name) {
      const auto named = checked_.namedRegs.find(target.name);
      if (named == checked_.namedRegs.end()) {
        return fail(target.loc, std::format("'{}' is not a register", target.name));
      }
      const uint16_t zero = allocSlot();
      emit(Insn{Opcode::ConstInt, 0, zero, kNoSlot, kNoSlot, kNoSlot, 0});
      emit(Insn{Opcode::WriteReg, 0, kNoSlot, zero, value, kNoSlot, named->second.index()});
      return ok();
    }

    XDEC_TRY(const RegAccess access, resolveRegAccess(target));
    emit(Insn{Opcode::WriteReg, 0, kNoSlot, access.index, value, kNoSlot, access.base});
    return ok();
  }

  struct RegAccess {
    /// Register id of element zero.
    uint64_t base = 0;
    /// Slot holding the element index.
    uint16_t index = kNoSlot;
  };

  Result<RegAccess> resolveRegAccess(const Expr& expr) {
    const Expr* indexExpr = &expr;
    std::string viewName;
    if (expr.kind == ExprKind::Member) {
      viewName = expr.name;
      indexExpr = expr.args[0].get();
    }
    if (indexExpr->kind != ExprKind::Index || indexExpr->args[0]->kind != ExprKind::Name) {
      return fail(expr.loc, "expected a register file element");
    }

    const std::string& fileName = indexExpr->args[0]->name;
    const ProgramRegFile* file = program_->findRegFile(fileName);
    if (file == nullptr) {
      return fail(expr.loc, std::format("unknown register file '{}'", fileName));
    }

    RegAccess access;
    access.base = file->base.index();
    if (!viewName.empty()) {
      const RegFileBinding* binding = checked_.findRegFile(fileName);
      if (binding == nullptr) {
        return fail(expr.loc, "register file lost between checking and compiling");
      }
      bool found = false;
      for (std::size_t index = 0; index < binding->viewNames.size(); ++index) {
        if (binding->viewNames[index] == viewName) {
          access.base = file->viewBase[index].index();
          found = true;
          break;
        }
      }
      if (!found) {
        return fail(expr.loc, std::format("unknown view '{}'", viewName));
      }
    }
    XDEC_TRY(access.index, compileExpr(*indexExpr->args[1]));
    return access;
  }

  // -- expressions ----------------------------------------------------------

  Result<uint16_t> compileExpr(const Expr& expr) {
    switch (expr.kind) {
      case ExprKind::Integer: {
        const uint16_t dest = allocSlot();
        emit(Insn{Opcode::ConstInt, 0, dest, kNoSlot, kNoSlot, kNoSlot, expr.integer});
        return dest;
      }

      case ExprKind::String:
        // Strings only reach the compiler as a builtin's first argument, which
        // reads them directly rather than through a slot.
        return fail(expr.loc, "a string is not a value");

      case ExprKind::Name:
        return compileName(expr);

      case ExprKind::Unary:
        return compileUnary(expr);

      case ExprKind::Binary:
        return compileBinary(expr);

      case ExprKind::Conditional: {
        XDEC_TRY(const uint16_t condition, compileExpr(*expr.args[0]));
        XDEC_TRY(const uint16_t ifTrue, compileExpr(*expr.args[1]));
        XDEC_TRY(const uint16_t ifFalse, compileExpr(*expr.args[2]));
        const uint16_t dest = allocSlot();
        // Both arms are evaluated: they are pure by construction, since the
        // checker only allows a compile-time condition here and effects are
        // statements.
        emit(Insn{Opcode::IntSelect, 0, dest, condition, ifTrue, ifFalse, 0});
        return dest;
      }

      case ExprKind::Index:
      case ExprKind::Member: {
        XDEC_TRY(const RegAccess access, resolveRegAccess(expr));
        const uint16_t dest = allocSlot();
        emit(Insn{Opcode::ReadReg, 0, dest, access.index, kNoSlot, kNoSlot, access.base});
        return dest;
      }

      case ExprKind::Call:
        return compileCall(expr);
    }
    return fail(expr.loc, "unhandled expression kind");
  }

  Result<uint16_t> compileName(const Expr& expr) {
    const auto bound = names_.find(expr.name);
    if (bound != names_.end()) {
      return bound->second;
    }

    if (currentFields_ != nullptr) {
      const auto field = fieldIndex_.find(expr.name);
      if (field != fieldIndex_.end()) {
        const uint16_t dest = allocSlot();
        emit(Insn{Opcode::Field, 0, dest, static_cast<uint16_t>(field->second), kNoSlot,
                  kNoSlot, 0});
        return dest;
      }
      if (expr.name == "insn_pc") {
        const uint16_t dest = allocSlot();
        emit(Insn{Opcode::InsnPc, 0, dest, kNoSlot, kNoSlot, kNoSlot, 0});
        return dest;
      }
      if (expr.name == "insn_len") {
        const uint16_t dest = allocSlot();
        emit(Insn{Opcode::InsnLen, 0, dest, kNoSlot, kNoSlot, kNoSlot, 0});
        return dest;
      }
      if (expr.name == "opcode") {
        const uint16_t dest = allocSlot();
        emit(Insn{Opcode::Opword, 0, dest, kNoSlot, kNoSlot, kNoSlot, 0});
        return dest;
      }
    }

    const auto named = checked_.namedRegs.find(expr.name);
    if (named != checked_.namedRegs.end()) {
      const uint16_t zero = allocSlot();
      emit(Insn{Opcode::ConstInt, 0, zero, kNoSlot, kNoSlot, kNoSlot, 0});
      const uint16_t dest = allocSlot();
      emit(Insn{Opcode::ReadReg, 0, dest, zero, kNoSlot, kNoSlot, named->second.index()});
      return dest;
    }

    return fail(expr.loc, std::format("unresolved name '{}'", expr.name));
  }

  Result<uint16_t> compileUnary(const Expr& expr) {
    XDEC_TRY(const uint16_t operand, compileExpr(*expr.args[0]));
    const uint16_t dest = allocSlot();

    if (isIntExpr(*expr.args[0])) {
      emit(Insn{Opcode::IntUnary, static_cast<uint8_t>(toIntOp(expr.unaryOp)), dest, operand,
                kNoSlot, kNoSlot, 0});
      return dest;
    }

    ExprOp op = ExprOp::Neg;
    switch (expr.unaryOp) {
      case UnaryOp::Negate:
        op = ExprOp::Neg;
        break;
      case UnaryOp::BitNot:
      case UnaryOp::LogicalNot:
        // On a one-bit value these are the same operation, and the checker only
        // allows `!` on bits(1).
        op = ExprOp::Not;
        break;
    }
    emit(Insn{Opcode::ExprUnary, static_cast<uint8_t>(op), dest, operand, kNoSlot, kNoSlot, 0});
    return dest;
  }

  Result<uint16_t> compileBinary(const Expr& expr) {
    const bool isInt = isIntExpr(*expr.args[0]) && isIntExpr(*expr.args[1]);

    if (isInt) {
      XDEC_TRY(const uint16_t lhs, compileExpr(*expr.args[0]));
      XDEC_TRY(const uint16_t rhs, compileExpr(*expr.args[1]));
      const uint16_t dest = allocSlot();
      emit(Insn{Opcode::IntBinary, static_cast<uint8_t>(toIntOp(expr.binaryOp)), dest, lhs, rhs,
                kNoSlot, 0});
      return dest;
    }

    // The IL has no reversed comparisons, so `a > b` is emitted as `b < a`.
    // Doing it here rather than in the IL keeps one spelling per relation.
    const bool reversed = isReversedComparison(expr.binaryOp);
    const Expr& first = reversed ? *expr.args[1] : *expr.args[0];
    const Expr& second = reversed ? *expr.args[0] : *expr.args[1];
    ExprOp op = ExprOp::Add;
    if (!toExprOp(reversed ? reverseComparison(expr.binaryOp) : expr.binaryOp, op)) {
      return fail(expr.loc,
                  std::format("operator '{}' has no IL form", toString(expr.binaryOp)));
    }

    XDEC_TRY(const uint16_t lhs, compileExpr(first));
    XDEC_TRY(const uint16_t rhs, compileExpr(second));
    const uint16_t dest = allocSlot();
    emit(Insn{Opcode::ExprBinary, static_cast<uint8_t>(op), dest, lhs, rhs, kNoSlot, 0});
    return dest;
  }

  Result<uint16_t> compileCall(const Expr& expr) {
    const auto lowering = loweringTable().find(expr.name);
    if (lowering != loweringTable().end()) {
      return compileBuiltin(expr, lowering->second);
    }

    const auto function = functionIndex_.find(expr.name);
    if (function == functionIndex_.end()) {
      return fail(expr.loc, std::format("unresolved function '{}'", expr.name));
    }

    XDEC_TRY(const uint16_t base, gatherArguments(expr.args, 0));
    const uint16_t dest = allocSlot();
    emit(Insn{Opcode::CallFn, 0, dest, base, static_cast<uint16_t>(expr.args.size()), kNoSlot,
              function->second});
    return dest;
  }

  /// Evaluates arguments and copies them into a contiguous run, which is what a
  /// callee's parameter slots are. Evaluating straight into the run would not
  /// work: an argument's own temporaries would land in the middle of it.
  Result<uint16_t> gatherArguments(const std::vector<ExprPtr>& args, std::size_t from) {
    std::vector<uint16_t> evaluated;
    evaluated.reserve(args.size() - from);
    for (std::size_t index = from; index < args.size(); ++index) {
      XDEC_TRY(const uint16_t slot, compileExpr(*args[index]));
      evaluated.push_back(slot);
    }
    const uint16_t base = slotTop_;
    for (const uint16_t slot : evaluated) {
      const uint16_t target = allocSlot();
      emit(Insn{Opcode::Move, 0, target, slot, kNoSlot, kNoSlot, 0});
    }
    if (evaluated.empty()) {
      // Still allocate one slot so that the base is a valid index.
      (void)allocSlot();
    }
    return base;
  }

  Result<uint16_t> compileBuiltin(const Expr& expr, const BuiltinLowering& lowering) {
    // The two variadic builtins take a name first and their arguments after, so
    // they are handled before the uniform path.
    if (lowering.op == Opcode::Intrinsic) {
      const bool hasResult = expr.name == "intrinsic_value";
      const uint32_t name = internString(expr.args[0]->name);
      uint16_t width = kNoSlot;
      std::size_t firstArgument = 1;
      if (hasResult) {
        XDEC_TRY(width, compileExpr(*expr.args[1]));
        firstArgument = 2;
      }
      XDEC_TRY(const uint16_t base, gatherArguments(expr.args, firstArgument));
      const uint16_t dest = hasResult ? allocSlot() : kNoSlot;
      emit(Insn{Opcode::Intrinsic, 0, dest, base,
                static_cast<uint16_t>(expr.args.size() - firstArgument), width, name});
      return dest;
    }

    if (lowering.op == Opcode::Unimplemented) {
      const uint32_t name = internString(expr.args[0]->name);
      emit(Insn{Opcode::Unimplemented, 0, kNoSlot, kNoSlot, kNoSlot, kNoSlot, name});
      return kNoSlot;
    }

    uint16_t operands[3] = {kNoSlot, kNoSlot, kNoSlot};
    for (std::size_t index = 0; index < expr.args.size() && index < 3; ++index) {
      XDEC_TRY(operands[index], compileExpr(*expr.args[index]));
    }

    const uint16_t dest = lowering.isEffect ? kNoSlot : allocSlot();
    emit(Insn{lowering.op, lowering.aux, dest, operands[0], operands[1], operands[2], 0});
    return dest;
  }

  /// Whether an expression produces a compile-time integer. Recomputed here
  /// rather than carried over from the checker, because the two need different
  /// things and threading a type map through would couple them.
  [[nodiscard]] bool isIntExpr(const Expr& expr) const {
    switch (expr.kind) {
      case ExprKind::Integer:
        return true;
      case ExprKind::String:
        return false;
      case ExprKind::Name: {
        if (names_.contains(expr.name)) {
          return intSlots_.contains(names_.at(expr.name));
        }
        if (fieldIndex_.contains(expr.name)) {
          return true;
        }
        return expr.name == "insn_pc" || expr.name == "insn_len" || expr.name == "opcode";
      }
      case ExprKind::Unary:
        return isIntExpr(*expr.args[0]);
      case ExprKind::Binary:
        return isIntExpr(*expr.args[0]) && isIntExpr(*expr.args[1]);
      case ExprKind::Conditional:
        return isIntExpr(*expr.args[1]);
      case ExprKind::Index:
      case ExprKind::Member:
        return false;
      case ExprKind::Call:
        return isIntCall(expr);
    }
    return false;
  }

  [[nodiscard]] bool isIntCall(const Expr& expr) const {
    static const std::unordered_set<std::string_view> kIntBuiltins = {
        "sextint", "ones_int", "highestbit_int", "ror_int", "replicate_int"};
    if (kIntBuiltins.contains(expr.name)) {
      return true;
    }
    if (loweringTable().contains(expr.name)) {
      return false;
    }
    const auto it = functionIndex_.find(expr.name);
    if (it == functionIndex_.end()) {
      return false;
    }
    return module_.functions[it->second].result.kind == TypeKind::Int;
  }

  const Module& module_;
  const CheckedModule& checked_;
  std::unique_ptr<SpecProgram> program_;

  std::unordered_map<std::string, uint32_t> stringIndex_;
  std::unordered_map<std::string, uint32_t> functionIndex_;
  std::unordered_map<std::string, uint32_t> fieldIndex_;
  std::unordered_map<std::string, uint16_t> names_;
  std::unordered_set<uint16_t> intSlots_;
  const std::vector<ProgramField>* currentFields_ = nullptr;

  uint32_t bodyStart_ = 0;
  uint16_t slotTop_ = 0;
  uint16_t slotHighWater_ = 0;
};

}  // namespace

Result<std::unique_ptr<SpecProgram>> compile(const Module& module,
                                             const CheckedModule& checked) {
  Compiler compiler{module, checked};
  return compiler.run();
}

namespace {

Result<std::unique_ptr<SpecProgram>> checkAndCompile(const Module& module,
                                                    std::string_view sourceName) {
  CheckResult checked = check(module);
  if (!checked.report.ok()) {
    Diag diag{DiagCode::ParseError,
              std::format("spec '{}' has {} error(s)", sourceName, checked.report.errors.size())};
    for (const Diag& entry : checked.report.errors) {
      diag.note(entry.format());
    }
    return err(std::move(diag));
  }
  return compile(module, *checked.module);
}

}  // namespace

Result<std::unique_ptr<SpecProgram>> compileSource(std::string_view text,
                                                   std::string_view sourceName) {
  XDEC_TRY(std::unique_ptr<Module> module, parseModule(text, sourceName));
  return checkAndCompile(*module, sourceName);
}

Result<std::unique_ptr<SpecProgram>> compileFile(const std::filesystem::path& path) {
  XDEC_TRY(std::unique_ptr<Module> module, parseSpecFile(path));
  return checkAndCompile(*module, path.filename().string());
}

}  // namespace xdec::spec
