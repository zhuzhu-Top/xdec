#include "xdec/spec/engine.h"

#include <format>

#include "xdec/spec/compile.h"
#include "xdec/support/bits.h"
#include "xdec/support/log.h"

namespace xdec::spec {

XDEC_DEFINE_LOG_CATEGORY(specLog, "spec");

namespace {

using il::ExprId;
using il::ExprOp;
using il::FlagOp;
using il::Type;

/// A slot holds one of the two kinds of value a spec computes. Which kind is
/// fixed by the type checker, so no tag is carried and none is checked.
struct Slot {
  uint64_t integer = 0;
  ExprId expr;
};

/// How deep helper calls may nest. Recursion is rejected at check time, so this
/// only guards against a corrupt blob.
constexpr unsigned kMaxCallDepth = 64;

[[nodiscard]] uint64_t applyIntUnary(IntOp op, uint64_t a) noexcept {
  switch (op) {
    case IntOp::Negate:
      return ~a + 1;
    case IntOp::Not:
      return ~a;
    case IntOp::LogicalNot:
      return a == 0 ? 1 : 0;
    default:
      return a;
  }
}

[[nodiscard]] uint64_t applyIntBinary(IntOp op, uint64_t a, uint64_t b) noexcept {
  const auto sa = static_cast<int64_t>(a);
  const auto sb = static_cast<int64_t>(b);
  switch (op) {
    case IntOp::Add:
      return a + b;
    case IntOp::Sub:
      return a - b;
    case IntOp::Mul:
      return a * b;
    // Division by zero yields zero rather than trapping. A spec should not
    // divide by a decoded field without guarding it, and the checker cannot
    // prove that in general; crashing the decompiler over it would be worse.
    case IntOp::DivU:
      return b == 0 ? 0 : a / b;
    case IntOp::DivS:
      return sb == 0 ? 0 : static_cast<uint64_t>(sa / sb);
    case IntOp::RemU:
      return b == 0 ? 0 : a % b;
    case IntOp::RemS:
      return sb == 0 ? 0 : static_cast<uint64_t>(sa % sb);
    case IntOp::And:
      return a & b;
    case IntOp::Or:
      return a | b;
    case IntOp::Xor:
      return a ^ b;
    case IntOp::Shl:
      return b >= 64 ? 0 : a << b;
    case IntOp::ShrU:
      return b >= 64 ? 0 : a >> b;
    case IntOp::ShrS:
      return static_cast<uint64_t>(b >= 64 ? (sa < 0 ? -1 : 0) : sa >> b);
    case IntOp::Equal:
      return a == b ? 1 : 0;
    case IntOp::NotEqual:
      return a != b ? 1 : 0;
    case IntOp::LessU:
      return a < b ? 1 : 0;
    case IntOp::LessEqualU:
      return a <= b ? 1 : 0;
    case IntOp::LessS:
      return sa < sb ? 1 : 0;
    case IntOp::LessEqualS:
      return sa <= sb ? 1 : 0;
    case IntOp::GreaterU:
      return a > b ? 1 : 0;
    case IntOp::GreaterEqualU:
      return a >= b ? 1 : 0;
    case IntOp::GreaterS:
      return sa > sb ? 1 : 0;
    case IntOp::GreaterEqualS:
      return sa >= sb ? 1 : 0;
    case IntOp::LogicalAnd:
      return (a != 0 && b != 0) ? 1 : 0;
    case IntOp::LogicalOr:
      return (a != 0 || b != 0) ? 1 : 0;
    default:
      return 0;
  }
}

/// The result type of a cast, which the ExprOp determines except for a bitcast.
[[nodiscard]] Type castResultType(ExprOp op, unsigned width, bool floatResult) noexcept {
  switch (op) {
    case ExprOp::IntToFpS:
    case ExprOp::IntToFpU:
    case ExprOp::FpConvert:
      return Type::floating(width);
    case ExprOp::Bitcast:
      return floatResult ? Type::floating(width) : Type::integer(width);
    default:
      return Type::integer(width);
  }
}

/// Executes one compiled body.
///
/// The same interpreter serves both probing and elaboration. In probe mode the
/// IL-building opcodes are skipped and their results left invalid, which is safe
/// because the type system guarantees no integer ever depends on an IL value --
/// and integers are all that control flow is computed from.
class Interpreter {
 public:
  Interpreter(const SpecProgram& program, const DecodedInsn& insn, const LiftSite* site)
      : program_(program), insn_(insn), site_(site) {}

  [[nodiscard]] InsnFlow flow() const noexcept { return flow_; }
  [[nodiscard]] const Diag* error() const noexcept { return error_.get(); }

  /// Runs a body and returns the value its `return` produced, if any.
  bool run(const Body& body, std::span<const Slot> arguments, Slot& result) {
    if (depth_ >= kMaxCallDepth) {
      setError("spec helper calls nested too deeply");
      return false;
    }
    const std::size_t base = slots_.size();
    slots_.resize(base + std::max<std::size_t>(body.slots, arguments.size()));
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      slots_[base + index] = arguments[index];
    }

    ++depth_;
    const bool okay = execute(body, base, result);
    --depth_;
    slots_.resize(base);
    return okay;
  }

 private:
  void setError(std::string message) {
    if (error_ == nullptr) {
      error_ = std::make_unique<Diag>(DiagCode::Internal, std::move(message));
      error_->at(insn_.address);
    }
  }

  [[nodiscard]] Slot& slot(std::size_t base, uint16_t index) {
    // A slot index past the body's declared frame means the blob disagrees with
    // its own compiler; a scratch slot keeps the interpreter memory-safe while
    // the error propagates.
    if (index == kNoSlot || base + index >= slots_.size()) {
      setError("spec bytecode names a slot outside its frame");
      return scratch_;
    }
    return slots_[base + index];
  }

  [[nodiscard]] uint64_t intAt(std::size_t base, uint16_t index) {
    return slot(base, index).integer;
  }

  [[nodiscard]] ExprId exprAt(std::size_t base, uint16_t index) {
    return slot(base, index).expr;
  }

  [[nodiscard]] bool building() const noexcept { return site_ != nullptr; }

  /// The type a call's result value carries: the width of the architecture's
  /// result register (x0 by the usual name), or void when the register file
  /// has no such name — calls then define nothing, the pre-SSA shape.
  [[nodiscard]] Type callResultType() const {
    if (!building()) {
      return Type::voidType();
    }
    if (const il::RegId x0 = function().registers().find("x0"); x0.valid()) {
      return function().registers()[function().registers().rootOf(x0)].type();
    }
    return Type::voidType();
  }

  [[nodiscard]] il::Function& function() const { return *site_->function; }

  [[nodiscard]] unsigned widthOf(ExprId expr) const {
    return building() ? function().expr(expr).type.bits() : 0;
  }

  /// Records a branch target and returns the block it names.
  [[nodiscard]] il::BlockId blockFor(uint64_t address) {
    if (!site_->blockAt) {
      setError("lifting a branch without a block resolver");
      return il::BlockId{};
    }
    const il::BlockId block = site_->blockAt(address);
    if (!block.valid()) {
      setError(std::format("branch target 0x{:x} is not inside this function", address));
    }
    return block;
  }

  bool execute(const Body& body, std::size_t base, Slot& result) {
    uint32_t pc = 0;
    while (pc < body.length) {
      if (error_ != nullptr) {
        return false;
      }
      const Insn& insn = program_.code[body.start + pc];
      ++pc;

      switch (insn.op) {
        case Opcode::Halt:
          return true;

        case Opcode::Jump:
          pc = static_cast<uint32_t>(insn.imm);
          continue;

        case Opcode::JumpIfZero:
          if (intAt(base, insn.a) == 0) {
            pc = static_cast<uint32_t>(insn.imm);
          }
          continue;

        case Opcode::Return:
          if (insn.a != kNoSlot) {
            result = slot(base, insn.a);
          }
          return true;

        case Opcode::ConstInt:
          slot(base, insn.dest).integer = insn.imm;
          continue;

        case Opcode::Field: {
          const uint64_t value = insn.a < insn_.fieldCount ? insn_.fields[insn.a] : 0;
          slot(base, insn.dest).integer = value;
          continue;
        }

        case Opcode::InsnPc:
          slot(base, insn.dest).integer = insn_.address;
          continue;

        case Opcode::InsnLen:
          slot(base, insn.dest).integer = insn_.length;
          continue;

        case Opcode::Opword:
          slot(base, insn.dest).integer = insn_.word;
          continue;

        case Opcode::Move:
          slot(base, insn.dest) = slot(base, insn.a);
          continue;

        case Opcode::IntUnary:
          slot(base, insn.dest).integer =
              applyIntUnary(static_cast<IntOp>(insn.aux), intAt(base, insn.a));
          continue;

        case Opcode::IntBinary:
          slot(base, insn.dest).integer = applyIntBinary(
              static_cast<IntOp>(insn.aux), intAt(base, insn.a), intAt(base, insn.b));
          continue;

        case Opcode::IntSelect:
          slot(base, insn.dest) =
              intAt(base, insn.a) != 0 ? slot(base, insn.b) : slot(base, insn.c);
          continue;

        case Opcode::SExtInt: {
          const unsigned bits = static_cast<unsigned>(intAt(base, insn.b));
          slot(base, insn.dest).integer =
              bits == 0 || bits >= 64
                  ? intAt(base, insn.a)
                  : static_cast<uint64_t>(signExtend(intAt(base, insn.a), bits));
          continue;
        }

        case Opcode::OnesInt:
          slot(base, insn.dest).integer =
              lowMask(static_cast<unsigned>(intAt(base, insn.a)));
          continue;

        case Opcode::HighestBitInt: {
          const uint64_t value = intAt(base, insn.a);
          // -1 for zero, matching the manual's HighestSetBit, so a spec can test
          // for it rather than special-casing the argument beforehand.
          slot(base, insn.dest).integer =
              value == 0 ? ~uint64_t{0} : static_cast<uint64_t>(63 - countLeadingZeros(value, 64));
          continue;
        }

        case Opcode::RorInt: {
          const uint64_t value = intAt(base, insn.a);
          const unsigned amount = static_cast<unsigned>(intAt(base, insn.b));
          const unsigned width = static_cast<unsigned>(intAt(base, insn.c));
          slot(base, insn.dest).integer =
              width == 0 || width > 64 ? value : rotateRight(value, width, amount);
          continue;
        }

        case Opcode::ReplicateInt: {
          const uint64_t pattern = intAt(base, insn.a);
          const unsigned from = static_cast<unsigned>(intAt(base, insn.b));
          const unsigned to = static_cast<unsigned>(intAt(base, insn.c));
          slot(base, insn.dest).integer = replicate(pattern, from, to);
          continue;
        }

        case Opcode::CallFn: {
          if (insn.imm >= program_.functions.size()) {
            setError("spec bytecode calls a function that does not exist");
            return false;
          }
          const ProgramFn& callee = program_.functions[insn.imm];
          std::vector<Slot> arguments;
          arguments.reserve(insn.b);
          for (uint16_t index = 0; index < insn.b; ++index) {
            arguments.push_back(slot(base, static_cast<uint16_t>(insn.a + index)));
          }
          Slot returned;
          if (!run(callee.body, arguments, returned)) {
            return false;
          }
          slot(base, insn.dest) = returned;
          continue;
        }

        default:
          break;
      }

      // Everything below either builds IL or records an effect.
      if (!executeEffect(insn, base)) {
        return false;
      }
    }
    return true;
  }

  bool executeEffect(const Insn& insn, std::size_t base) {
    switch (insn.op) {
      // -- control flow, recorded in both modes -----------------------------
      case Opcode::Branch:
        flow_.kind = FlowKind::Branch;
        flow_.target = intAt(base, insn.a);
        if (building()) {
          const il::BlockId target = blockFor(flow_.target);
          if (!target.valid()) {
            return false;
          }
          (void)function().appendBranch(site_->block, insn_.address, target);
        }
        return true;

      case Opcode::CondBranch:
        flow_.kind = FlowKind::CondBranch;
        flow_.target = intAt(base, insn.b);
        flow_.fallthrough = intAt(base, insn.c);
        if (building()) {
          const il::BlockId taken = blockFor(flow_.target);
          const il::BlockId notTaken = blockFor(flow_.fallthrough);
          if (!taken.valid() || !notTaken.valid()) {
            return false;
          }
          (void)function().appendCondBranch(site_->block, insn_.address, exprAt(base, insn.a),
                                            taken, notTaken);
        }
        return true;

      case Opcode::IndirectBranch:
        flow_.kind = FlowKind::IndirectBranch;
        if (building()) {
          (void)function().appendIndirectBranch(site_->block, insn_.address,
                                                exprAt(base, insn.a));
        }
        return true;

      case Opcode::Ret:
        flow_.kind = FlowKind::Return;
        if (building()) {
          (void)function().appendReturn(site_->block, insn_.address);
        }
        return true;

      case Opcode::Unreachable:
        flow_.kind = FlowKind::Unreachable;
        if (building()) {
          (void)function().appendUnreachable(site_->block, insn_.address);
        }
        return true;

      case Opcode::CallAddr:
        flow_.calls = true;
        flow_.callTargetKnown = true;
        flow_.callTarget = intAt(base, insn.a);
        if (building()) {
          const ExprId target =
              function().constant(Type::integer(program_.pointerBits), flow_.callTarget);
          (void)function().appendCall(site_->block, insn_.address, target,
                                      callResultType());
        }
        return true;

      case Opcode::IndirectCall:
        flow_.calls = true;
        if (building()) {
          (void)function().appendCall(site_->block, insn_.address, exprAt(base, insn.a),
                                      callResultType());
        }
        return true;

      case Opcode::Unimplemented:
        // A terminator: nothing after an instruction that could not be lifted is
        // trustworthy.
        flow_.kind = FlowKind::Unknown;
        if (building()) {
          (void)function().appendUnimplemented(site_->block, insn_.address,
                                               program_.string(static_cast<uint32_t>(insn.imm)));
        }
        return true;

      default:
        break;
    }

    // -- pure IL construction and non-control effects ------------------------
    if (!building()) {
      return true;
    }

    il::Function& fn = function();
    switch (insn.op) {
      case Opcode::Imm:
        slot(base, insn.dest).expr = fn.constant(
            Type::integer(static_cast<unsigned>(intAt(base, insn.b))), intAt(base, insn.a));
        return true;

      case Opcode::Undef:
        slot(base, insn.dest).expr =
            fn.undefined(Type::integer(static_cast<unsigned>(intAt(base, insn.a))));
        return true;

      case Opcode::ExprUnary:
        slot(base, insn.dest).expr =
            fn.unary(static_cast<ExprOp>(insn.aux), exprAt(base, insn.a));
        return true;

      case Opcode::ExprBinary:
        slot(base, insn.dest).expr = fn.binary(static_cast<ExprOp>(insn.aux),
                                               exprAt(base, insn.a), exprAt(base, insn.b));
        return true;

      case Opcode::Cast: {
        const auto op = static_cast<ExprOp>(insn.aux & 0x7F);
        const auto width = static_cast<unsigned>(intAt(base, insn.b));
        slot(base, insn.dest).expr =
            fn.cast(op, castResultType(op, width, (insn.aux & 0x80) != 0), exprAt(base, insn.a));
        return true;
      }

      case Opcode::Extract:
        slot(base, insn.dest).expr =
            fn.extract(Type::integer(static_cast<unsigned>(intAt(base, insn.c))),
                       exprAt(base, insn.a), static_cast<unsigned>(intAt(base, insn.b)));
        return true;

      case Opcode::Concat: {
        const ExprId high = exprAt(base, insn.a);
        const ExprId low = exprAt(base, insn.b);
        slot(base, insn.dest).expr =
            fn.concat(Type::integer(widthOf(high) + widthOf(low)), high, low);
        return true;
      }

      case Opcode::Select:
        slot(base, insn.dest).expr =
            fn.select(exprAt(base, insn.a), exprAt(base, insn.b), exprAt(base, insn.c));
        return true;

      case Opcode::FlagDef: {
        ExprId operands[3];
        unsigned count = 0;
        for (const uint16_t index : {insn.a, insn.b, insn.c}) {
          if (index == kNoSlot) {
            break;
          }
          operands[count++] = exprAt(base, index);
        }
        if (count == 0) {
          setError("flagdef with no operands");
          return false;
        }
        slot(base, insn.dest).expr =
            fn.flagDef(static_cast<FlagOp>(insn.aux), widthOf(operands[0]),
                       std::span<const ExprId>{operands, count});
        return true;
      }

      case Opcode::FlagCond:
        slot(base, insn.dest).expr = fn.flagCondition(
            exprAt(base, insn.a), static_cast<il::ConditionCode>(intAt(base, insn.b)));
        return true;

      case Opcode::FlagBit:
        slot(base, insn.dest).expr = fn.flagBitOf(
            exprAt(base, insn.a), static_cast<il::FlagBitIndex>(intAt(base, insn.b)));
        return true;

      case Opcode::ReadReg: {
        const il::RegId reg{static_cast<uint32_t>(insn.imm + intAt(base, insn.a))};
        if (!program_.registers.contains(reg)) {
          setError("spec bytecode reads a register that does not exist");
          return false;
        }
        const il::ValueId value = fn.appendReadReg(site_->block, insn_.address, reg);
        slot(base, insn.dest).expr = fn.valueRef(value);
        return true;
      }

      case Opcode::WriteReg: {
        const il::RegId reg{static_cast<uint32_t>(insn.imm + intAt(base, insn.a))};
        if (!program_.registers.contains(reg)) {
          setError("spec bytecode writes a register that does not exist");
          return false;
        }
        // A write to the zero register is discarded, which is what makes an
        // alias like `cmp` fall out of `subs` for free.
        if (program_.registers[reg].regClass == il::RegClass::Zero) {
          return true;
        }
        (void)fn.appendWriteReg(site_->block, insn_.address, reg, exprAt(base, insn.b));
        return true;
      }

      case Opcode::Load: {
        const Type type = Type::integer(static_cast<unsigned>(intAt(base, insn.b)));
        const il::ValueId value =
            fn.appendLoad(site_->block, insn_.address, type, exprAt(base, insn.a));
        slot(base, insn.dest).expr = fn.valueRef(value);
        return true;
      }

      case Opcode::Store: {
        const ExprId value = exprAt(base, insn.b);
        (void)fn.appendStore(site_->block, insn_.address, fn.expr(value).type,
                             exprAt(base, insn.a), value);
        return true;
      }

      case Opcode::Nop:
        (void)fn.appendNop(site_->block, insn_.address);
        return true;

      case Opcode::Intrinsic: {
        std::vector<ExprId> arguments;
        arguments.reserve(insn.b);
        for (uint16_t index = 0; index < insn.b; ++index) {
          arguments.push_back(exprAt(base, static_cast<uint16_t>(insn.a + index)));
        }
        const Type resultType =
            insn.c == kNoSlot ? Type::voidType()
                              : Type::integer(static_cast<unsigned>(intAt(base, insn.c)));
        const il::OpId op =
            fn.appendIntrinsic(site_->block, insn_.address,
                               program_.string(static_cast<uint32_t>(insn.imm)), resultType,
                               arguments);
        if (insn.dest != kNoSlot) {
          slot(base, insn.dest).expr = fn.valueRef(fn.resultOf(op));
        }
        return true;
      }

      default:
        setError(std::format("spec bytecode has an opcode the engine does not implement: {}",
                             toString(insn.op)));
        return false;
    }
  }

  const SpecProgram& program_;
  const DecodedInsn& insn_;
  const LiftSite* site_;
  std::vector<Slot> slots_;
  Slot scratch_;
  InsnFlow flow_;
  std::unique_ptr<Diag> error_;
  unsigned depth_ = 0;
};

}  // namespace

std::string_view toString(FlowKind kind) noexcept {
  switch (kind) {
    case FlowKind::Fallthrough:
      return "fallthrough";
    case FlowKind::Branch:
      return "branch";
    case FlowKind::CondBranch:
      return "cond-branch";
    case FlowKind::IndirectBranch:
      return "indirect-branch";
    case FlowKind::Return:
      return "return";
    case FlowKind::Unreachable:
      return "unreachable";
    case FlowKind::Unknown:
      return "unknown";
  }
  return "unknown";
}

SpecEngine::SpecEngine(std::unique_ptr<SpecProgram> program) : program_(std::move(program)) {}

DecodedInsn SpecEngine::decode(std::span<const std::byte> bytes, uint64_t address) const {
  DecodedInsn decoded;
  decoded.address = address;
  decoded.length = program_->insnWidth / 8;

  if (bytes.size() < decoded.length) {
    return decoded;
  }

  uint64_t word = 0;
  for (unsigned index = 0; index < decoded.length; ++index) {
    const auto byte = static_cast<uint64_t>(bytes[index]);
    if (program_->endian == Endian::Little) {
      word |= byte << (index * 8);
    } else {
      word = (word << 8) | byte;
    }
  }
  decoded.word = word;

  for (const uint32_t candidate : program_->decoder.lookup(word)) {
    const EncodingPattern& pattern = program_->patterns[candidate];
    if ((word & pattern.mask) != pattern.value) {
      continue;
    }

    const ProgramInsn& rule = program_->instructions[pattern.instruction];
    DecodedInsn trial = decoded;
    trial.instruction = pattern.instruction;
    trial.fieldCount = static_cast<unsigned>(std::min<std::size_t>(rule.fields.size(),
                                                                   kMaxFields));
    for (unsigned index = 0; index < trial.fieldCount; ++index) {
      const ProgramField& field = rule.fields[index];
      trial.fields[index] = (word >> field.shift) & lowMask(field.bits);
    }

    // Guards run against the decoded fields, which is what lets an alias be
    // separated from its base encoding by more than the bit pattern.
    bool accepted = true;
    for (const Body& guard : rule.guards) {
      Interpreter interpreter{*program_, trial, nullptr};
      Slot result;
      if (!interpreter.run(guard, {}, result) || result.integer == 0) {
        accepted = false;
        break;
      }
    }
    if (!accepted) {
      continue;
    }

    trial.valid = true;
    return trial;
  }

  return decoded;
}

InsnFlow SpecEngine::probe(const DecodedInsn& insn) const {
  InsnFlow flow;
  flow.fallthrough = insn.next();
  if (!insn.valid) {
    flow.kind = FlowKind::Unknown;
    return flow;
  }

  const ProgramInsn& rule = program_->instructions[insn.instruction];
  Interpreter interpreter{*program_, insn, nullptr};
  Slot result;
  if (!interpreter.run(rule.semantics, {}, result)) {
    flow.kind = FlowKind::Unknown;
    return flow;
  }

  InsnFlow observed = interpreter.flow();
  if (observed.kind == FlowKind::Fallthrough) {
    observed.fallthrough = insn.next();
  }
  return observed;
}

Result<void> SpecEngine::elaborate(const DecodedInsn& insn, const LiftSite& site) const {
  if (site.function == nullptr) {
    return err(Diag{DiagCode::Internal, "elaborating without a function"});
  }
  if (!insn.valid) {
    (void)site.function->appendUnimplemented(site.block, insn.address, "undecodable");
    return ok();
  }

  const ProgramInsn& rule = program_->instructions[insn.instruction];
  Interpreter interpreter{*program_, insn, &site};
  Slot result;
  if (!interpreter.run(rule.semantics, {}, result)) {
    const Diag* diag = interpreter.error();
    return err(diag != nullptr ? *diag
                               : Diag{DiagCode::Internal, "spec semantics failed to run"});
  }
  return ok();
}

std::string SpecEngine::disassemble(const DecodedInsn& insn) const {
  if (!insn.valid) {
    return std::format(".word 0x{:08x}", insn.word);
  }
  const ProgramInsn& rule = program_->instructions[insn.instruction];
  if (rule.disassembly.length == 0) {
    return rule.name;
  }

  std::string text;
  // An optional group is emitted only when something inside it is non-zero,
  // which is how `sub x0, x1, x2` avoids printing a `, lsl #0` that no
  // assembler would write.
  //
  // Groups nest, because the optional parts of an operand do: the extend of an
  // indexed load is printed only when it is not the default, and its shift only
  // when that is not zero either, giving `[x0, x1]`, `[x0, w1, uxtw]` and
  // `[x0, w1, uxtw #2]` from one template. An inner group that survives keeps
  // its parents alive, since a printed part cannot lose its punctuation.
  std::vector<std::string> groups;
  std::vector<bool> isDefault;

  const auto append = [&](std::string_view piece) {
    (groups.empty() ? text : groups.back()) += piece;
  };

  for (uint32_t index = 0; index < rule.disassembly.length; ++index) {
    const ProgramAsmPiece& piece = program_->asmPieces[rule.disassembly.start + index];
    switch (piece.op) {
      case AsmPieceOp::GroupBegin:
        groups.emplace_back();
        isDefault.push_back(true);
        continue;
      case AsmPieceOp::GroupEnd: {
        if (groups.empty()) {
          continue;
        }
        const std::string finished = std::move(groups.back());
        const bool keep = !isDefault.back();
        groups.pop_back();
        isDefault.pop_back();
        if (keep) {
          append(finished);
          if (!isDefault.empty()) {
            isDefault.back() = false;
          }
        }
        continue;
      }
      case AsmPieceOp::Text:
        append(program_->string(piece.text));
        continue;
      case AsmPieceOp::Substitution:
        break;
    }

    Interpreter interpreter{*program_, insn, nullptr};
    Slot value;
    if (!interpreter.run(piece.value, {}, value)) {
      append("?");
      continue;
    }
    if (value.integer != 0 && !isDefault.empty()) {
      isDefault.back() = false;
    }

    uint64_t styleArgument = 0;
    if (piece.styleArgument.length != 0) {
      Interpreter argument{*program_, insn, nullptr};
      Slot slot;
      if (argument.run(piece.styleArgument, {}, slot)) {
        styleArgument = slot.integer;
      }
    }

    append(renderOperand(piece.style, value.integer, styleArgument, insn));
  }

  return text;
}

namespace {

// `hs` and `lo` rather than `cs` and `cc`: the encodings are the same and both
// spellings assemble, but AArch64 documents these as the preferred forms and
// every disassembler prints them, so matching costs nothing and differing would
// show up as a disagreement in every differential run.
constexpr std::string_view kConditionNames[] = {"eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc",
                                                "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"};
constexpr std::string_view kShiftNames[] = {"lsl", "lsr", "asr", "ror"};
// One-based, so that zero can mean "print nothing" and an optional group holding
// an extend disappears the way the convention expects. A zero-based table cannot
// do that: `uxtb` is option zero, and an operand that prints has to be non-zero.
//
// Index four is `lsl`, not `uxtx`. The two are the same encoding and assemblers
// take either, but inside a memory operand or an add against the stack pointer
// only `lsl` is written, and those are the only places the encoding appears.
constexpr std::string_view kExtendNames[] = {"",     "uxtb", "uxth", "uxtw", "lsl",
                                             "sxtb", "sxth", "sxtw", "sxtx"};

}  // namespace

std::string SpecEngine::renderOperand(AsmStyle style, uint64_t value, uint64_t styleArgument,
                                      const DecodedInsn& insn) const {
  switch (style) {
    case AsmStyle::RegisterSp:
      // Only index 31 differs, and it differs by name rather than by register
      // file entry, so the spelling is fixed here rather than looked up.
      if (value == 31) {
        return styleArgument == 0 ? "wsp" : "sp";
      }
      [[fallthrough]];
    case AsmStyle::Register: {
      // The style argument selects which view of the register file to name, so
      // that one template covers both widths.
      const ProgramRegFile* file = program_->regFiles.empty() ? nullptr : &program_->regFiles[0];
      if (file == nullptr || value >= file->count) {
        return std::format("r{}", value);
      }
      il::RegId reg{static_cast<uint32_t>(file->base.index() + value)};
      if (styleArgument == 0 && !file->viewBase.empty()) {
        reg = il::RegId{static_cast<uint32_t>(file->viewBase[0].index() + value)};
      }
      return std::string{program_->registers.nameOf(reg)};
    }
    case AsmStyle::VectorRegister: {
      // A V register is named after the width the instruction touches it at, so
      // the argument is a width in bits and the view is looked up by it rather
      // than by position. The parent covers the widest case.
      const ProgramRegFile* file = nullptr;
      for (const ProgramRegFile& candidate : program_->regFiles) {
        if (candidate.role == il::RegClass::Vector) {
          file = &candidate;
          break;
        }
      }
      if (file == nullptr || value >= file->count) {
        return std::format("v{}", value);
      }
      il::RegId reg{static_cast<uint32_t>(file->base.index() + value)};
      if (styleArgument != file->bits) {
        for (std::size_t index = 0; index < file->viewBits.size(); ++index) {
          if (file->viewBits[index] == styleArgument) {
            reg = il::RegId{static_cast<uint32_t>(file->viewBase[index].index() + value)};
            break;
          }
        }
      }
      return std::string{program_->registers.nameOf(reg)};
    }
    case AsmStyle::Condition:
      return std::string{value < std::size(kConditionNames) ? kConditionNames[value] : "??"};
    case AsmStyle::Shift:
      return std::string{value < std::size(kShiftNames) ? kShiftNames[value] : "??"};
    case AsmStyle::Extend:
      return std::string{value < std::size(kExtendNames) ? kExtendNames[value] : "??"};
    case AsmStyle::Label: {
      // A label is a signed displacement in instruction units, resolved against
      // this instruction's address. The style argument carries the field width
      // the displacement was encoded at.
      const int64_t displacement =
          styleArgument == 0 ? static_cast<int64_t>(value)
                             : signExtend(value, static_cast<unsigned>(styleArgument));
      const uint64_t target =
          insn.address + static_cast<uint64_t>(displacement * (program_->insnWidth / 8));
      return std::format("0x{:x}", target);
    }
    case AsmStyle::Hex:
      return std::format("0x{:x}", value);
    case AsmStyle::Dec:
      // Signed, because the only things printed this way are displacements, and
      // a pre-indexed `stp x29, x30, [sp, #-0x20]!` printed unsigned reads as
      // #18446744073709551584, which is true and useless.
      return std::format("{}", static_cast<int64_t>(value));
    case AsmStyle::Plain:
      break;
  }
  return std::format("{}", value);
}

Result<std::unique_ptr<SpecEngine>> loadSpecSource(std::string_view text,
                                                   std::string_view name) {
  XDEC_TRY(std::unique_ptr<SpecProgram> program, compileSource(text, name));
  XDEC_LOG_INFO(specLog(), "{}", program->summary());
  return std::make_unique<SpecEngine>(std::move(program));
}

Result<std::unique_ptr<SpecEngine>> loadSpecFile(const std::filesystem::path& path) {
  XDEC_TRY(std::unique_ptr<SpecProgram> program, compileFile(path));
  XDEC_LOG_INFO(specLog(), "{}", program->summary());
  return std::make_unique<SpecEngine>(std::move(program));
}

}  // namespace xdec::spec
