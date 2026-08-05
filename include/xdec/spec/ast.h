// Abstract syntax for the instruction semantics DSL.
//
// A spec file declares an architecture's registers, a set of helper functions,
// and one `insn` per encoding. Each `insn` states three things in one place: the
// bit pattern that identifies it, the disassembly text, and the semantics as IL
// construction. Keeping them together is the whole point -- a decoder that has
// drifted from its semantics is the classic source of silent wrongness.
//
// The AST is deliberately untyped: the parser accepts anything syntactically
// well formed, and `check.h` is the single place that decides what is
// meaningful. That split keeps parse errors about syntax and semantic errors
// about meaning.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "xdec/support/target.h"

namespace xdec::spec {

struct SourceLoc {
  uint32_t line = 0;
  uint32_t column = 0;

  [[nodiscard]] std::string toString() const;
};

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

enum class UnaryOp : uint8_t {
  Negate,      // -x
  BitNot,      // ~x
  LogicalNot,  // !x
};

enum class BinaryOp : uint8_t {
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
  ShrU,  // >>
  ShrS,  // >>>
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

[[nodiscard]] std::string_view toString(UnaryOp op) noexcept;
[[nodiscard]] std::string_view toString(BinaryOp op) noexcept;

enum class ExprKind : uint8_t {
  /// A literal integer. Always compile-time.
  Integer,
  /// A literal string. Only legal where a builtin expects a name, such as an
  /// intrinsic's identity.
  String,
  /// An identifier: a decoded field, a parameter, a let binding, or a register.
  Name,
  Unary,
  Binary,
  /// `f(a, b)`: a builtin or a user-declared function.
  Call,
  /// `gpr[n]`: an element of a register file.
  Index,
  /// `gpr[n].w`: a narrower view of a register.
  Member,
  /// `c ? a : b`. The condition must be compile-time; runtime selection is the
  /// `select` builtin, which becomes an IL node.
  Conditional,
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
  ExprKind kind = ExprKind::Integer;
  SourceLoc loc;
  /// Integer literal value.
  uint64_t integer = 0;
  /// Name, callee, or member name depending on kind.
  std::string name;
  UnaryOp unaryOp = UnaryOp::Negate;
  BinaryOp binaryOp = BinaryOp::Add;
  std::vector<ExprPtr> args;

  [[nodiscard]] static ExprPtr makeInteger(SourceLoc loc, uint64_t value);
  [[nodiscard]] static ExprPtr makeName(SourceLoc loc, std::string name);
};

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

enum class TypeKind : uint8_t {
  /// A compile-time integer: a decoded field, a shift amount, a register index.
  /// Never appears in the emitted IL.
  Int,
  /// An IL integer expression whose width is given by a compile-time
  /// expression, so that one rule can cover both the 32- and 64-bit forms of an
  /// instruction.
  Bits,
  /// An IL float expression.
  Float,
  /// An IL lazy flag bundle.
  Flags,
  /// A statement-only function's result.
  Void,
};

struct TypeExpr {
  TypeKind kind = TypeKind::Int;
  SourceLoc loc;
  /// Width for Bits and Float. Null for the others.
  ExprPtr width;
  /// Optional `int(lo..hi)` bound. A bound with exactly two values is what lets
  /// the checker prove the else branch of `if sf == 1`, which is how a rule that
  /// covers both the 32- and 64-bit forms of an instruction type-checks. Decoded
  /// fields get their bound from their bit width automatically.
  bool hasRange = false;
  uint64_t rangeLow = 0;
  uint64_t rangeHigh = 0;

  [[nodiscard]] TypeExpr clone() const;
};

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

enum class StmtKind : uint8_t {
  /// `let x = expr;`
  Let,
  /// `reg = expr;` or `gpr[n] = expr;`
  Assign,
  /// A call evaluated for its effect: `store(...)`, `branch(...)`.
  Effect,
  /// Compile-time branch on a decoded field. Runtime branches use `cbranch`.
  If,
  /// `return expr;`
  Return,
};

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

struct Stmt {
  StmtKind kind = StmtKind::Effect;
  SourceLoc loc;
  /// Binding name for Let.
  std::string name;
  /// Assignment destination.
  ExprPtr target;
  /// Bound value, assigned value, condition, or returned value.
  ExprPtr value;
  std::vector<StmtPtr> thenBody;
  std::vector<StmtPtr> elseBody;
};

// ---------------------------------------------------------------------------
// Encodings
// ---------------------------------------------------------------------------

struct EncodingItem {
  SourceLoc loc;
  /// Empty for a literal bit run.
  std::string field;
  /// Width in bits.
  unsigned bits = 0;
  /// For a literal run, the required bits, most significant first.
  uint64_t literal = 0;
  bool isLiteral = false;
  /// `_:5` matches anything and binds nothing.
  bool isWildcard = false;
};

struct EncodingDecl {
  SourceLoc loc;
  std::vector<EncodingItem> items;
  /// Total width, which must equal the architecture's instruction width.
  unsigned width = 0;
  /// Bits the encoding constrains, and the values it requires there.
  uint64_t mask = 0;
  uint64_t value = 0;
};

// ---------------------------------------------------------------------------
// Disassembly templates
// ---------------------------------------------------------------------------

enum class AsmPieceKind : uint8_t {
  /// Literal text.
  Text,
  /// `{expr}` or `{expr:style}`, rendered from a field or expression.
  Substitution,
  /// `{, lsl #imm}`: emitted only when the enclosed substitutions are
  /// non-default. Used for the optional operands ARM syntax is full of.
  OptionalGroup,
};

struct AsmPiece;
using AsmPiecePtr = std::unique_ptr<AsmPiece>;

struct AsmPiece {
  AsmPieceKind kind = AsmPieceKind::Text;
  SourceLoc loc;
  std::string text;
  /// Expression rendered for a substitution.
  ExprPtr value;
  /// Rendering style: `gpr`, `hex`, `dec`, `cond`, `label`.
  std::string style;
  /// Width selector for register styles, e.g. `{Rd:gpr(sf)}`.
  ExprPtr styleArgument;
  std::vector<AsmPiecePtr> group;
};

struct AsmTemplate {
  SourceLoc loc;
  std::vector<AsmPiecePtr> pieces;
};

// ---------------------------------------------------------------------------
// Declarations
// ---------------------------------------------------------------------------

struct ParamDecl {
  SourceLoc loc;
  std::string name;
  TypeExpr type;
};

struct FnDecl {
  SourceLoc loc;
  std::string name;
  std::vector<ParamDecl> params;
  TypeExpr result;
  std::vector<StmtPtr> body;
};

enum class RegRole : uint8_t { General, Float, Vector, Flags, StackPointer, ProgramCounter, Special };

[[nodiscard]] std::string_view toString(RegRole role) noexcept;
[[nodiscard]] bool parseRegRole(std::string_view text, RegRole& out) noexcept;

/// A narrower view of every register in a file, such as w0 over x0.
struct RegViewDecl {
  SourceLoc loc;
  /// Accessed as `gpr[n].w`.
  std::string name;
  unsigned bits = 0;
  unsigned offset = 0;
  /// Writing the view zeroes the rest of the parent, as AArch64 w-registers do.
  /// Getting this wrong makes a 32-bit write followed by a 64-bit read produce a
  /// stale value, which is why it is declared rather than inferred.
  bool zeroExtends = false;
  /// Printed name prefix, e.g. `w` for w0..w30.
  std::string prefix;
  /// Name of this view of the file's zero register, e.g. `wzr`. Required when
  /// the file declares a zero index, because the prefix rule would otherwise
  /// produce `w31`, a name no disassembler prints.
  std::string zeroName;
};

struct RegFileDecl {
  SourceLoc loc;
  std::string name;
  unsigned bits = 0;
  unsigned count = 0;
  std::string prefix;
  std::vector<RegViewDecl> views;
  /// Index whose register reads as zero and discards writes, if any.
  std::optional<unsigned> zeroIndex;
  /// Printed name of the zero register, e.g. `xzr`.
  std::string zeroName;
  /// What the file holds. Later analyses ask this to tell a vector register from
  /// an integer one, so a file of V registers has to say so rather than inherit
  /// the general-purpose default.
  RegRole role = RegRole::General;
};

struct RegDecl {
  SourceLoc loc;
  std::string name;
  /// Zero for a flags register, which carries the IL flags type.
  unsigned bits = 0;
  RegRole role = RegRole::Special;
};

struct ArchDecl {
  SourceLoc loc;
  std::string name;
  Arch arch = Arch::Unknown;
  Endian endian = Endian::Little;
  /// Fixed instruction width in bits. Zero means variable length, which the v1
  /// language does not yet support.
  unsigned insnWidth = 0;
  unsigned pointerBits = 0;
  std::vector<RegFileDecl> regFiles;
  std::vector<RegDecl> regs;
};

struct InsnDecl {
  SourceLoc loc;
  std::string name;
  EncodingDecl encoding;
  std::optional<AsmTemplate> asmTemplate;
  /// Extra conditions on decoded fields, for encodings a bit pattern alone
  /// cannot separate, such as an alias that requires Rn == 31.
  std::vector<ExprPtr> requires_;
  /// Higher wins when two encodings overlap. Defaults to zero.
  int priority = 0;
  std::vector<StmtPtr> semantics;
};

/// `include "arm64/branch.xspec"`, resolved against the directory of the file
/// that names it.
struct IncludeDecl {
  SourceLoc loc;
  std::string path;
};

struct Module {
  std::string sourceName;
  ArchDecl arch;
  std::vector<IncludeDecl> includes;
  std::vector<FnDecl> functions;
  std::vector<InsnDecl> instructions;
};

}  // namespace xdec::spec
