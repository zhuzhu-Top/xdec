// The compiled form of a spec.
//
// A checked module is a tree of AST nodes with names in it. A program is flat,
// index-addressed and free of strings in its hot paths, which is what makes it
// serialisable to a blob and cheap to execute once per lifted instruction.
//
// The execution model is the point of the design. Everything a spec computes
// falls into two categories: values known once the instruction word is decoded,
// and values that only exist at run time on the target. The first kind are plain
// integers the engine computes directly; the second kind are IL nodes the engine
// builds. The type checker has already proved which is which, so the bytecode
// carries no tags and the interpreter never has to ask.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xdec/il/expr.h"
#include "xdec/il/register_file.h"
#include "xdec/spec/encoding.h"
#include "xdec/support/target.h"

namespace xdec::spec {

/// An invalid slot, used where an operand is absent.
inline constexpr uint16_t kNoSlot = 0xFFFF;

enum class Opcode : uint8_t {
  /// Ends a body that fell off its end. Only legal in a void body.
  Halt,

  // -- values known at decode time ------------------------------------------
  ConstInt,       ///< imm -> d
  Field,          ///< field `a` of the current instruction -> d
  InsnPc,         ///< -> d
  InsnLen,        ///< -> d
  Opword,         ///< the raw instruction word -> d
  IntUnary,       ///< aux, a -> d
  IntBinary,      ///< aux, a, b -> d
  IntSelect,      ///< a ? b : c -> d
  SExtInt,        ///< sign-extend a from b bits -> d
  OnesInt,        ///< low a bits set -> d
  HighestBitInt,  ///< index of the highest set bit of a, or -1 when a is zero -> d
  RorInt,         ///< rotate a right by b within c bits -> d
  ReplicateInt,   ///< repeat the low b bits of a up to c bits -> d
  Move,           ///< a -> d, used to gather call arguments into one run

  // -- control, resolved while elaborating ----------------------------------
  Jump,           ///< imm is the target offset in this body
  JumpIfZero,     ///< a == 0 jumps to imm
  Return,         ///< a, or kNoSlot for a void body
  CallFn,         ///< function imm, arguments in slots a..a+b -> d

  // -- IL expressions -------------------------------------------------------
  Imm,            ///< constant of value a and width b -> d
  Undef,          ///< undefined value of width a -> d
  ExprUnary,      ///< aux is the ExprOp, a -> d
  ExprBinary,     ///< aux is the ExprOp, a, b -> d
  /// aux is the ExprOp, a resized to width b -> d. The 0x80 bit of aux marks a
  /// bitcast whose result is a float, which the ExprOp alone cannot say.
  Cast,
  Extract,        ///< c bits of a starting at bit b -> d
  Concat,         ///< a as the high half, b as the low half -> d
  Select,         ///< a ? b : c, all at run time -> d
  FlagDef,        ///< aux is the FlagOp; a, b, c are its operands -> d
  FlagCond,       ///< condition b of flag bundle a -> d
  FlagBit,        ///< bit b of flag bundle a -> d

  // -- IL effects -----------------------------------------------------------
  ReadReg,        ///< register imm + slot a -> d
  WriteReg,       ///< register imm + slot a takes the value in b
  Load,           ///< b bits at address a -> d
  Store,          ///< the value in b at address a
  Branch,         ///< to the address in a
  CondBranch,     ///< a ? address b : address c
  IndirectBranch, ///< to the computed target in a
  CallAddr,       ///< the address in a
  IndirectCall,   ///< the computed target in a
  Ret,
  Nop,
  Unreachable,
  Intrinsic,      ///< string imm, arguments a..a+b, result width c or kNoSlot
  Unimplemented,  ///< string imm
};

[[nodiscard]] std::string_view toString(Opcode op) noexcept;

/// Sub-operations for IntUnary and IntBinary. Signedness lives on the operation
/// rather than on the value, because a machine integer has no signedness.
enum class IntOp : uint8_t {
  Negate,
  Not,
  LogicalNot,
  Add,
  Sub,
  Mul,
  DivU,
  DivS,
  RemU,
  RemS,
  And,
  Or,
  Xor,
  Shl,
  ShrU,
  ShrS,
  Equal,
  NotEqual,
  LessU,
  LessEqualU,
  LessS,
  LessEqualS,
  GreaterU,
  GreaterEqualU,
  GreaterS,
  GreaterEqualS,
  LogicalAnd,
  LogicalOr,
};

[[nodiscard]] std::string_view toString(IntOp op) noexcept;

/// One bytecode instruction. Fixed size so that a body is a flat array and a
/// jump is an index.
struct Insn {
  Opcode op = Opcode::Halt;
  /// Sub-operation: an IntOp, an il::ExprOp, or an il::FlagOp.
  uint8_t aux = 0;
  uint16_t dest = kNoSlot;
  uint16_t a = kNoSlot;
  uint16_t b = kNoSlot;
  uint16_t c = kNoSlot;
  /// Literal, jump target, function index, register base, or string index.
  uint64_t imm = 0;
};

/// A compiled body: a run of instructions plus how many slots it needs.
struct Body {
  uint32_t start = 0;
  uint32_t length = 0;
  uint16_t slots = 0;

  [[nodiscard]] bool empty() const noexcept { return length == 0; }
};

struct ProgramFn {
  std::string name;
  uint16_t paramCount = 0;
  Body body;
};

/// A decoded field's position in the instruction word.
struct ProgramField {
  std::string name;
  uint8_t shift = 0;
  uint8_t bits = 0;
};

enum class AsmPieceOp : uint8_t { Text, Substitution, GroupBegin, GroupEnd };

/// RegisterSp differs from Register only at index 31, which the encodings that
/// use it read as the stack pointer rather than as the zero register.
enum class AsmStyle : uint8_t {
  Plain,
  Register,
  RegisterSp,
  /// Names the architecture's vector file. Unlike `Register`, whose argument
  /// picks a view by index, this one takes the width in bits, because a V
  /// register is named after the width the instruction accesses it at.
  VectorRegister,
  Hex,
  Dec,
  Condition,
  Label,
  Shift,
  /// The extend of an indexed memory operand. Spelled apart from `Shift`
  /// because the same encoding is printed differently there: option 3 is `uxtx`
  /// as a register extend but `lsl` inside brackets, which is what assemblers
  /// accept and what every disassembler prints.
  Extend
};

[[nodiscard]] std::string_view toString(AsmStyle style) noexcept;

/// Disassembly is a flat instruction list rather than a tree, so that rendering
/// is a single pass and the whole template serialises as an array.
struct ProgramAsmPiece {
  AsmPieceOp op = AsmPieceOp::Text;
  AsmStyle style = AsmStyle::Plain;
  /// Literal text, for Text.
  uint32_t text = 0;
  /// Body producing the value, for Substitution.
  Body value;
  /// Body producing the style argument, if any.
  Body styleArgument;
};

struct ProgramAsm {
  uint32_t start = 0;
  uint32_t length = 0;
};

struct ProgramInsn {
  std::string name;
  std::vector<ProgramField> fields;
  /// Guards that must all be non-zero for this rule to match.
  std::vector<Body> guards;
  Body semantics;
  ProgramAsm disassembly;
};

/// Where a register file's registers landed, so that `gpr[n]` is an index.
struct ProgramRegFile {
  std::string name;
  il::RegId base;
  uint32_t count = 0;
  uint32_t bits = 0;
  std::vector<il::RegId> viewBase;
  std::vector<uint32_t> viewBits;
  il::RegClass role = il::RegClass::General;
};

/// Everything the engine needs, and nothing that only the checker needed.
struct SpecProgram {
  static constexpr uint32_t kMagic = 0x43455053;  // "SPEC"
  static constexpr uint32_t kVersion = 1;

  std::string name;
  Arch arch = Arch::Unknown;
  Endian endian = Endian::Little;
  unsigned insnWidth = 0;
  unsigned pointerBits = 0;

  il::RegisterFile registers;
  std::vector<ProgramRegFile> regFiles;
  std::vector<std::string> strings;

  std::vector<Insn> code;
  std::vector<ProgramAsmPiece> asmPieces;
  std::vector<ProgramFn> functions;
  std::vector<ProgramInsn> instructions;

  std::vector<EncodingPattern> patterns;
  /// Rebuilt on load rather than serialised: the construction is deterministic
  /// and takes microseconds, so storing it would only add a way to be stale.
  DecisionTree decoder;

  void rebuildDecoder();
  [[nodiscard]] std::string_view string(uint32_t index) const;
  [[nodiscard]] const ProgramRegFile* findRegFile(std::string_view name) const;
  [[nodiscard]] int findInsn(std::string_view name) const;
  /// A one-line summary, for `xdec spec`.
  [[nodiscard]] std::string summary() const;
};

}  // namespace xdec::spec
