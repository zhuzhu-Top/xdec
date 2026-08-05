#include "xdec/spec/check.h"

#include <algorithm>
#include <format>
#include <unordered_set>

#include "xdec/support/bits.h"

namespace xdec::spec {
namespace {

// ---------------------------------------------------------------------------
// Builtins
// ---------------------------------------------------------------------------

// name, minimum arity, maximum arity (0xFF for variadic)
//
// The signatures below are the IL's op signatures. Keeping them in one table is
// what makes "the spec type-checks" mean "the spec cannot build malformed IL".
#define XDEC_SPEC_BUILTINS(X)                     \
  /* casts and bit surgery */                     \
  X(ZExt, "zext", 2, 2)                           \
  X(SExt, "sext", 2, 2)                           \
  X(Trunc, "trunc", 2, 2)                         \
  X(BitcastInt, "bitcast_int", 2, 2)              \
  X(BitcastFloat, "bitcast_float", 2, 2)          \
  X(Extract, "extract", 3, 3)                     \
  X(Concat, "concat", 2, 2)                       \
  /* arithmetic that has no operator spelling */  \
  X(Select, "select", 3, 3)                       \
  X(Clz, "clz", 1, 1)                             \
  X(Ctz, "ctz", 1, 1)                             \
  X(PopCount, "popcount", 1, 1)                   \
  X(ByteSwap, "bswap", 1, 1)                      \
  X(BitReverse, "brev", 1, 1)                     \
  X(RotR, "rotr", 2, 2)                           \
  X(RotL, "rotl", 2, 2)                           \
  X(MulHiU, "mulhi_u", 2, 2)                      \
  X(MulHiS, "mulhi_s", 2, 2)                      \
  /* lazy flags */                                \
  X(FlagDefAdd, "flagdef_add", 2, 2)              \
  X(FlagDefSub, "flagdef_sub", 2, 2)              \
  X(FlagDefAdc, "flagdef_adc", 3, 3)              \
  X(FlagDefSbc, "flagdef_sbc", 3, 3)              \
  X(FlagDefLogic, "flagdef_logic", 1, 1)          \
  X(FlagConst, "flagconst", 1, 1)                 \
  X(SelectFlags, "select_flags", 3, 3)            \
  X(Cond, "cond", 2, 2)                           \
  X(FlagBit, "flagbit", 2, 2)                     \
  /* constants, memory and unknown values */      \
  X(Imm, "imm", 2, 2)                             \
  X(Undef, "undef", 1, 1)                         \
  X(Load, "load", 2, 2)                           \
  /* floating point */                            \
  X(FAdd, "fadd", 2, 2)                           \
  X(FSub, "fsub", 2, 2)                           \
  X(FMul, "fmul", 2, 2)                           \
  X(FDiv, "fdiv", 2, 2)                           \
  X(FNeg, "fneg", 1, 1)                           \
  X(FAbs, "fabs", 1, 1)                           \
  X(FSqrt, "fsqrt", 1, 1)                         \
  X(FCmpEq, "fcmp_eq", 2, 2)                      \
  X(FCmpLt, "fcmp_lt", 2, 2)                      \
  X(FCmpLe, "fcmp_le", 2, 2)                      \
  X(FCmpUno, "fcmp_uno", 2, 2)                    \
  X(IntToFpS, "inttofp_s", 2, 2)                  \
  X(IntToFpU, "inttofp_u", 2, 2)                  \
  X(FpToIntS, "fptoint_s", 2, 2)                  \
  X(FpToIntU, "fptoint_u", 2, 2)                  \
  X(FpConvert, "fpconvert", 2, 2)                 \
  /* compile-time integer helpers */              \
  X(SExtInt, "sextint", 2, 2)                     \
  X(OnesInt, "ones_int", 1, 1)                    \
  X(HighestBitInt, "highestbit_int", 1, 1)        \
  X(RorInt, "ror_int", 3, 3)                      \
  X(ReplicateInt, "replicate_int", 3, 3)          \
  /* effects */                                   \
  X(Store, "store", 2, 2)                         \
  X(Branch, "branch", 1, 1)                       \
  X(CondBranch, "cbranch", 3, 3)                  \
  X(IndirectBranch, "brind", 1, 1)                \
  X(Call, "call", 1, 1)                           \
  X(IndirectCall, "callind", 1, 1)                \
  X(Return, "ret", 0, 0)                          \
  X(Nop, "nop", 0, 0)                             \
  X(Unreachable, "unreachable", 0, 0)             \
  X(Intrinsic, "intrinsic", 1, 0xFF)              \
  X(IntrinsicValue, "intrinsic_value", 2, 0xFF)   \
  X(Unimplemented, "unimplemented", 1, 1)

enum class Builtin : uint8_t {
#define XDEC_SPEC_BUILTIN_ENUM(name, text, minArity, maxArity) name,
  XDEC_SPEC_BUILTINS(XDEC_SPEC_BUILTIN_ENUM)
#undef XDEC_SPEC_BUILTIN_ENUM
      Count
};

struct BuiltinInfo {
  std::string_view text;
  uint8_t minArity;
  uint8_t maxArity;
};

const BuiltinInfo& builtinInfo(Builtin builtin) {
  static constexpr BuiltinInfo kTable[] = {
#define XDEC_SPEC_BUILTIN_INFO(name, text, minArity, maxArity) {text, minArity, maxArity},
      XDEC_SPEC_BUILTINS(XDEC_SPEC_BUILTIN_INFO)
#undef XDEC_SPEC_BUILTIN_INFO
  };
  return kTable[static_cast<std::size_t>(builtin)];
}

bool lookupBuiltin(std::string_view name, Builtin& out) {
  static const std::unordered_map<std::string_view, Builtin> kByName = [] {
    std::unordered_map<std::string_view, Builtin> map;
    for (unsigned index = 0; index < static_cast<unsigned>(Builtin::Count); ++index) {
      map.emplace(builtinInfo(static_cast<Builtin>(index)).text, static_cast<Builtin>(index));
    }
    return map;
  }();
  const auto it = kByName.find(name);
  if (it == kByName.end()) {
    return false;
  }
  out = it->second;
  return true;
}

// ---------------------------------------------------------------------------
// Semantic types
// ---------------------------------------------------------------------------

enum class SemKind : uint8_t {
  /// A compile-time integer.
  Int,
  /// An IL integer expression of a symbolic width.
  Bits,
  /// An IL float expression.
  Float,
  /// An IL lazy flag bundle.
  Flags,
  /// No value.
  Void,
  /// A string literal, legal only where a builtin names something.
  Str,
  /// Already reported. Compatible with everything, so one mistake produces one
  /// error rather than a cascade.
  Error,
};

struct SemType {
  SemKind kind = SemKind::Void;
  SymId width;

  [[nodiscard]] static SemType integer() { return SemType{SemKind::Int, SymId::invalid()}; }
  [[nodiscard]] static SemType bits(SymId width) { return SemType{SemKind::Bits, width}; }
  [[nodiscard]] static SemType floating(SymId width) { return SemType{SemKind::Float, width}; }
  [[nodiscard]] static SemType flags() { return SemType{SemKind::Flags, SymId::invalid()}; }
  [[nodiscard]] static SemType voidType() { return SemType{SemKind::Void, SymId::invalid()}; }
  [[nodiscard]] static SemType str() { return SemType{SemKind::Str, SymId::invalid()}; }
  [[nodiscard]] static SemType error() { return SemType{SemKind::Error, SymId::invalid()}; }

  [[nodiscard]] bool isError() const noexcept { return kind == SemKind::Error; }
  [[nodiscard]] bool isSized() const noexcept {
    return kind == SemKind::Bits || kind == SemKind::Float;
  }
};

struct Binding {
  SemType type;
  /// The symbolic value, for Int bindings.
  SymId value;
  /// Known bound on an Int binding: from a decoded field's width, or from an
  /// `int(lo..hi)` parameter declaration.
  bool hasRange = false;
  uint64_t rangeLow = 0;
  uint64_t rangeHigh = 0;
};

using Env = std::unordered_map<std::string, Binding>;

il::RegClass toRegClass(RegRole role) {
  switch (role) {
    case RegRole::General:
      return il::RegClass::General;
    case RegRole::Float:
      return il::RegClass::Float;
    case RegRole::Vector:
      return il::RegClass::Vector;
    case RegRole::Flags:
      return il::RegClass::Flags;
    case RegRole::StackPointer:
      return il::RegClass::StackPointer;
    case RegRole::ProgramCounter:
      return il::RegClass::ProgramCounter;
    case RegRole::Special:
      return il::RegClass::Special;
  }
  return il::RegClass::Special;
}

// ---------------------------------------------------------------------------
// Checker
// ---------------------------------------------------------------------------

class Checker {
 public:
  explicit Checker(const Module& module) : module_(module) {
    checked_ = std::make_unique<CheckedModule>();
    checked_->ast = &module;
  }

  CheckResult run() {
    buildRegisters();
    collectFunctions();
    checkForRecursion();
    for (const FnDecl& function : module_.functions) {
      checkFunction(function);
    }
    for (std::size_t index = 0; index < module_.instructions.size(); ++index) {
      checkInstruction(static_cast<uint32_t>(index), module_.instructions[index]);
    }
    buildDecoder();
    return CheckResult{std::move(checked_), std::move(report_)};
  }

 private:
  // -- diagnostics ----------------------------------------------------------

  void error(SourceLoc loc, std::string message) {
    report_.errors.push_back(Diag{DiagCode::ParseError, std::move(message)}.note(
        std::format("{}:{}", module_.sourceName, loc.toString())));
  }

  void warn(SourceLoc loc, std::string message) {
    report_.warnings.push_back(Diag{DiagCode::ParseError, std::move(message)}.note(
        std::format("{}:{}", module_.sourceName, loc.toString())));
  }

  [[nodiscard]] std::string describe(const SemType& type) const {
    switch (type.kind) {
      case SemKind::Int:
        return "int";
      case SemKind::Bits:
        return std::format("bits({})", syms_.toString(type.width));
      case SemKind::Float:
        return std::format("float({})", syms_.toString(type.width));
      case SemKind::Flags:
        return "flags";
      case SemKind::Void:
        return "void";
      case SemKind::Str:
        return "string";
      case SemKind::Error:
        return "<error>";
    }
    return "?";
  }

  // -- registers ------------------------------------------------------------

  void buildRegisters() {
    std::unordered_set<std::string> names;
    const auto claim = [&](SourceLoc loc, const std::string& name) {
      if (!names.insert(name).second) {
        error(loc, std::format("register name '{}' is declared more than once", name));
        return false;
      }
      return true;
    };

    for (const RegFileDecl& file : module_.arch.regFiles) {
      RegFileBinding binding;
      binding.name = file.name;
      binding.count = file.count;
      binding.bits = file.bits;
      binding.role = toRegClass(file.role);

      for (unsigned index = 0; index < file.count; ++index) {
        const bool isZero = file.zeroIndex.has_value() && *file.zeroIndex == index;
        const std::string name =
            isZero ? file.zeroName : std::format("{}{}", file.prefix, index);
        (void)claim(file.loc, name);
        const il::RegId id = checked_->registers.add(
            name, file.bits, isZero ? il::RegClass::Zero : toRegClass(file.role));
        if (index == 0) {
          binding.base = id;
        }
      }

      for (const RegViewDecl& view : file.views) {
        if (file.zeroIndex.has_value() && view.zeroName.empty()) {
          error(view.loc,
                std::format("regfile '{}' declares a zero register, so view '{}' must name its "
                            "own view of it with `zero \"...\"`",
                            file.name, view.name));
        }
        il::RegId viewBase;
        for (unsigned index = 0; index < file.count; ++index) {
          const bool isZero = file.zeroIndex.has_value() && *file.zeroIndex == index;
          const std::string name = isZero && !view.zeroName.empty()
                                       ? view.zeroName
                                       : std::format("{}{}", view.prefix, index);
          (void)claim(view.loc, name);
          const il::RegId parent{binding.base.index() + index};
          const il::RegId id = checked_->registers.addSubRegister(name, parent, view.offset,
                                                                 view.bits, view.zeroExtends);
          if (index == 0) {
            viewBase = id;
          }
        }
        binding.viewBase.push_back(viewBase);
        binding.viewBits.push_back(view.bits);
        binding.viewNames.push_back(view.name);
      }

      if (checked_->findRegFile(file.name) != nullptr) {
        error(file.loc, std::format("register file '{}' is declared more than once", file.name));
      }
      checked_->regFiles.push_back(std::move(binding));
    }

    for (const RegDecl& reg : module_.arch.regs) {
      (void)claim(reg.loc, reg.name);
      if (reg.role == RegRole::Flags && reg.bits != 0) {
        error(reg.loc, "a flags register carries the IL flags type and has no width");
      }
      const il::RegId id =
          checked_->registers.add(reg.name, reg.bits, toRegClass(reg.role));
      if (!checked_->namedRegs.emplace(reg.name, id).second) {
        error(reg.loc, std::format("register '{}' is declared more than once", reg.name));
      }
    }
  }

  // -- functions ------------------------------------------------------------

  void collectFunctions() {
    for (const FnDecl& function : module_.functions) {
      if (Builtin builtin = Builtin::Count; lookupBuiltin(function.name, builtin)) {
        error(function.loc,
              std::format("'{}' is a builtin and cannot be redefined", function.name));
        continue;
      }
      if (!functions_.emplace(function.name, &function).second) {
        error(function.loc, std::format("function '{}' is declared more than once",
                                        function.name));
      }
    }
  }

  /// A recursive helper could not terminate during elaboration: there is no loop
  /// construct and no bound on the recursion, so a spec that recurses would hang
  /// the lifter on some instruction rather than fail visibly.
  void checkForRecursion() {
    std::unordered_map<std::string, unsigned> state;  // 0 unvisited, 1 open, 2 done
    for (const FnDecl& function : module_.functions) {
      if (state[function.name] == 0) {
        visitForRecursion(function, state);
      }
    }
  }

  void visitForRecursion(const FnDecl& function, std::unordered_map<std::string, unsigned>& state) {
    state[function.name] = 1;
    std::vector<std::string> callees;
    for (const StmtPtr& statement : function.body) {
      collectCalls(statement.get(), callees);
    }
    for (const std::string& callee : callees) {
      const auto it = functions_.find(callee);
      if (it == functions_.end()) {
        continue;
      }
      const unsigned calleeState = state[callee];
      if (calleeState == 1) {
        error(function.loc,
              std::format("function '{}' takes part in a call cycle through '{}'; the DSL has "
                          "no loops, so recursion could not terminate",
                          function.name, callee));
        continue;
      }
      if (calleeState == 0) {
        visitForRecursion(*it->second, state);
      }
    }
    state[function.name] = 2;
  }

  void collectCalls(const Stmt* statement, std::vector<std::string>& out) {
    if (statement == nullptr) {
      return;
    }
    collectCalls(statement->target.get(), out);
    collectCalls(statement->value.get(), out);
    for (const StmtPtr& nested : statement->thenBody) {
      collectCalls(nested.get(), out);
    }
    for (const StmtPtr& nested : statement->elseBody) {
      collectCalls(nested.get(), out);
    }
  }

  void collectCalls(const Expr* expr, std::vector<std::string>& out) {
    if (expr == nullptr) {
      return;
    }
    if (expr->kind == ExprKind::Call) {
      out.push_back(expr->name);
    }
    for (const ExprPtr& arg : expr->args) {
      collectCalls(arg.get(), out);
    }
  }

  void checkFunction(const FnDecl& function) {
    Env env;
    std::unordered_set<std::string> paramNames;
    for (const ParamDecl& param : function.params) {
      if (!paramNames.insert(param.name).second) {
        error(param.loc, std::format("parameter '{}' is declared more than once", param.name));
      }
      // Parameters become fresh symbols so that the body is checked once,
      // generically, rather than per call site. The name is qualified because
      // symbols are shared across the whole module.
      const std::string symbolName = std::format("{}.{}", function.name, param.name);
      const SemType type = evalType(param.type, env);
      Binding binding;
      binding.type = type;
      if (type.kind == SemKind::Int) {
        binding.value = syms_.symbol(symbolName);
        binding.hasRange = param.type.hasRange;
        binding.rangeLow = param.type.rangeLow;
        binding.rangeHigh = param.type.rangeHigh;
      }
      env[param.name] = binding;
    }

    currentFunction_ = &function;
    currentReturn_ = &function.result;
    checkBody(function.body, env);
    currentFunction_ = nullptr;
    currentReturn_ = nullptr;

    const bool wantsValue = function.result.kind != TypeKind::Void;
    if (wantsValue && !bodyAlwaysReturns(function.body)) {
      error(function.loc,
            std::format("function '{}' can fall off its end without returning a value",
                        function.name));
    }
  }

  [[nodiscard]] static bool bodyAlwaysReturns(const std::vector<StmtPtr>& body) {
    for (const StmtPtr& statement : body) {
      if (statement->kind == StmtKind::Return) {
        return true;
      }
      if (statement->kind == StmtKind::If && !statement->elseBody.empty() &&
          bodyAlwaysReturns(statement->thenBody) && bodyAlwaysReturns(statement->elseBody)) {
        return true;
      }
    }
    return false;
  }

  // -- instructions ---------------------------------------------------------

  void checkInstruction(uint32_t index, const InsnDecl& insn) {
    CheckedInsn record;
    record.index = index;
    record.name = insn.name;

    if (!insnNames_.insert(insn.name).second) {
      error(insn.loc, std::format("instruction '{}' is declared more than once", insn.name));
    }

    if (insn.encoding.width != module_.arch.insnWidth) {
      error(insn.encoding.loc,
            std::format("encoding is {} bits but the architecture's instructions are {} bits",
                        insn.encoding.width, module_.arch.insnWidth));
    }

    Env env;
    fieldBits_.clear();

    // Fields are written most significant first, matching how manuals draw them.
    unsigned offset = insn.encoding.width;
    std::unordered_set<std::string> fieldNames;
    for (const EncodingItem& item : insn.encoding.items) {
      offset -= item.bits;
      if (item.isLiteral || item.isWildcard) {
        continue;
      }
      if (!fieldNames.insert(item.field).second) {
        error(item.loc,
              std::format("field '{}' appears twice in one encoding; give the halves distinct "
                          "names and combine them in the semantics",
                          item.field));
        continue;
      }
      if (reservedNames().contains(item.field)) {
        error(item.loc, std::format("'{}' is a reserved name", item.field));
        continue;
      }
      record.fields.push_back(FieldBinding{item.field, offset, item.bits});
      Binding binding;
      binding.type = SemType::integer();
      binding.value = syms_.symbol(item.field);
      // A field's width is its range, which is what lets `if sf == 1` prove that
      // sf is 0 in the else branch.
      binding.hasRange = true;
      binding.rangeLow = 0;
      binding.rangeHigh = lowMask(item.bits);
      env[item.field] = binding;
      fieldBits_[item.field] = item.bits;
    }

    // The instruction's own address and length, which is what a pc-relative
    // branch target is computed from.
    env["insn_pc"] = Binding{SemType::integer(), syms_.symbol("insn_pc")};
    env["insn_len"] = Binding{SemType::integer(),
                              syms_.constant(module_.arch.insnWidth / 8)};
    env["opcode"] = Binding{SemType::integer(), syms_.symbol("opcode")};

    for (const ExprPtr& condition : insn.requires_) {
      const SemType type = checkExpr(*condition, env);
      if (!type.isError() && type.kind != SemKind::Int) {
        error(condition->loc,
              std::format("a require clause must be a compile-time condition but is {}",
                          describe(type)));
      }
    }

    if (insn.asmTemplate.has_value()) {
      checkAsmTemplate(*insn.asmTemplate, env);
    }

    currentFunction_ = nullptr;
    currentReturn_ = nullptr;
    checkBody(insn.semantics, env);

    EncodingPattern pattern;
    pattern.instruction = index;
    pattern.name = insn.name;
    pattern.mask = insn.encoding.mask;
    pattern.value = insn.encoding.value;
    pattern.priority = insn.priority;
    pattern.hasGuards = !insn.requires_.empty();
    checked_->patterns.push_back(std::move(pattern));
    checked_->instructions.push_back(std::move(record));
  }

  void checkAsmTemplate(const AsmTemplate& tmpl, Env& env) {
    for (const AsmPiecePtr& piece : tmpl.pieces) {
      checkAsmPiece(*piece, env);
    }
  }

  void checkAsmPiece(const AsmPiece& piece, Env& env) {
    switch (piece.kind) {
      case AsmPieceKind::Text:
        break;
      case AsmPieceKind::Substitution: {
        const SemType type = checkExpr(*piece.value, env);
        if (!type.isError() && type.kind != SemKind::Int) {
          error(piece.loc,
                std::format("an asm substitution must be a compile-time integer but is {}",
                            describe(type)));
        }
        if (piece.styleArgument != nullptr) {
          const SemType argument = checkExpr(*piece.styleArgument, env);
          if (!argument.isError() && argument.kind != SemKind::Int) {
            error(piece.loc, "an asm style argument must be a compile-time integer");
          }
        }
        if (!piece.style.empty() && !asmStyles().contains(piece.style)) {
          error(piece.loc, std::format("unknown asm style '{}'", piece.style));
        }
        break;
      }
      case AsmPieceKind::OptionalGroup:
        for (const AsmPiecePtr& nested : piece.group) {
          checkAsmPiece(*nested, env);
        }
        break;
    }
  }

  // -- statements -----------------------------------------------------------

  void checkBody(const std::vector<StmtPtr>& body, Env& env) {
    for (const StmtPtr& statement : body) {
      checkStmt(*statement, env);
    }
  }

  void checkStmt(const Stmt& statement, Env& env) {
    switch (statement.kind) {
      case StmtKind::Let: {
        const SemType type = checkExpr(*statement.value, env);
        if (type.kind == SemKind::Void) {
          error(statement.loc, "cannot bind the result of something that produces no value");
        }
        if (reservedNames().contains(statement.name)) {
          error(statement.loc, std::format("'{}' is a reserved name", statement.name));
        }
        Binding binding;
        binding.type = type;
        if (type.kind == SemKind::Int) {
          binding.value = intValueOf(*statement.value, env);
        }
        // Shadowing is allowed and useful (`let a = ...; let a = trunc(a, 32);`)
        // but a rebind of a decoded field would silently change what the asm
        // template prints, so that is refused.
        if (fieldBits_.contains(statement.name)) {
          error(statement.loc,
                std::format("'{}' is a decoded field and cannot be rebound", statement.name));
        }
        env[statement.name] = binding;
        break;
      }

      case StmtKind::Assign: {
        const SemType valueType = checkExpr(*statement.value, env);
        checkAssignTarget(*statement.target, valueType, env);
        break;
      }

      case StmtKind::Effect: {
        if (statement.value->kind != ExprKind::Call) {
          error(statement.loc, "a statement must be an assignment, a let, or a call");
          break;
        }
        const SemType type = checkExpr(*statement.value, env);
        if (!type.isError() && type.kind != SemKind::Void) {
          // Discarding a value silently is how a semantics rule ends up doing
          // nothing at all.
          warn(statement.loc,
               std::format("the result of '{}' is discarded", statement.value->name));
        }
        break;
      }

      case StmtKind::If: {
        const SemType conditionType = checkExpr(*statement.value, env);
        if (!conditionType.isError() && conditionType.kind != SemKind::Int) {
          error(statement.loc,
                std::format("`if` decides at lift time and needs a compile-time condition, but "
                            "this is {}; use `select` for a value or `cbranch` for control flow",
                            describe(conditionType)));
        }

        // Refining the environment inside the branches is what lets a rule
        // declare `bits(32 << sf)` and still return a concrete `bits(64)` under
        // `if sf == 1`.
        Env thenEnv = env;
        Env elseEnv = env;
        applyRefinement(*statement.value, /*positive=*/true, thenEnv);
        applyRefinement(*statement.value, /*positive=*/false, elseEnv);
        checkBody(statement.thenBody, thenEnv);
        checkBody(statement.elseBody, elseEnv);
        break;
      }

      case StmtKind::Return: {
        if (currentReturn_ == nullptr) {
          error(statement.loc, "`return` outside a function");
          break;
        }
        const bool wantsValue = currentReturn_->kind != TypeKind::Void;
        if (!wantsValue) {
          if (statement.value != nullptr) {
            error(statement.loc, "this function returns nothing");
          }
          break;
        }
        if (statement.value == nullptr) {
          error(statement.loc, "expected a return value");
          break;
        }
        const SemType actual = checkExpr(*statement.value, env);
        // Re-evaluated here rather than once per function: under a refined
        // environment `bits(32 << sf)` may have become `bits(64)`.
        const SemType expected = evalType(*currentReturn_, env);
        requireAssignable(statement.loc, expected, actual, "return value");
        break;
      }
    }
  }

  void checkAssignTarget(const Expr& target, const SemType& valueType, Env& env) {
    switch (target.kind) {
      case ExprKind::Name: {
        const auto named = checked_->namedRegs.find(target.name);
        if (named == checked_->namedRegs.end()) {
          if (env.contains(target.name)) {
            error(target.loc,
                  std::format("'{}' is a value, not a register; only registers and memory can "
                              "be assigned",
                              target.name));
          } else {
            error(target.loc, std::format("unknown register '{}'", target.name));
          }
          return;
        }
        const il::RegisterInfo& info = checked_->registers[named->second];
        const SemType expected = info.regClass == il::RegClass::Flags
                                     ? SemType::flags()
                                     : SemType::bits(syms_.constant(info.bits));
        requireAssignable(target.loc, expected, valueType,
                          std::format("write to '{}'", target.name));
        return;
      }

      case ExprKind::Index:
      case ExprKind::Member: {
        SymId width;
        if (!resolveRegFileAccess(target, env, width)) {
          return;
        }
        requireAssignable(target.loc, SemType::bits(width), valueType, "register write");
        return;
      }

      default:
        error(target.loc, "only a register or a register file element can be assigned");
        return;
    }
  }

  /// Resolves `file[index]` and `file[index].view` and yields the accessed width.
  bool resolveRegFileAccess(const Expr& expr, Env& env, SymId& width) {
    const Expr* indexExpr = &expr;
    const RegViewDecl* view = nullptr;
    std::string viewName;

    if (expr.kind == ExprKind::Member) {
      viewName = expr.name;
      indexExpr = expr.args[0].get();
    }
    if (indexExpr->kind != ExprKind::Index) {
      error(expr.loc, "expected a register file element such as `gpr[n]`");
      return false;
    }
    const Expr& base = *indexExpr->args[0];
    if (base.kind != ExprKind::Name) {
      error(base.loc, "expected a register file name");
      return false;
    }

    const RegFileDecl* file = nullptr;
    for (const RegFileDecl& candidate : module_.arch.regFiles) {
      if (candidate.name == base.name) {
        file = &candidate;
        break;
      }
    }
    if (file == nullptr) {
      error(base.loc, std::format("unknown register file '{}'", base.name));
      return false;
    }

    const SemType indexType = checkExpr(*indexExpr->args[1], env);
    if (!indexType.isError() && indexType.kind != SemKind::Int) {
      error(indexExpr->args[1]->loc,
            std::format("a register index must be a compile-time integer but is {}",
                        describe(indexType)));
    } else if (indexType.kind == SemKind::Int) {
      // An index that can reach past the end of the file is a spec bug, not a
      // runtime condition, so it is caught here. A 5-bit field indexing a
      // 32-register file is provably in range and passes silently.
      uint64_t low = 0;
      uint64_t high = 0;
      if (rangeOf(*indexExpr->args[1], env, low, high)) {
        if (high >= file->count) {
          error(indexExpr->args[1]->loc,
                std::format("register index ranges over {}..{} but '{}' has {} registers", low,
                            high, file->name, file->count));
        }
      } else {
        warn(indexExpr->args[1]->loc,
             std::format("cannot show that this index stays inside '{}'; declare the parameter "
                         "as int(0..{})",
                         file->name, file->count - 1));
      }
    }

    if (viewName.empty()) {
      width = syms_.constant(file->bits);
      return true;
    }
    for (const RegViewDecl& candidate : file->views) {
      if (candidate.name == viewName) {
        view = &candidate;
        break;
      }
    }
    if (view == nullptr) {
      error(expr.loc,
            std::format("register file '{}' has no view '{}'", file->name, viewName));
      return false;
    }
    width = syms_.constant(view->bits);
    return true;
  }

  /// Refines `env` by any `name == literal` fact the condition implies.
  void applyRefinement(const Expr& condition, bool positive, Env& env) {
    if (condition.kind != ExprKind::Binary) {
      return;
    }
    const BinaryOp op = condition.binaryOp;
    const bool isEqual = op == BinaryOp::Equal;
    const bool isNotEqual = op == BinaryOp::NotEqual;
    if (!isEqual && !isNotEqual) {
      return;
    }
    // The fact we can use is "the name equals the literal", which the positive
    // branch of `==` and the negative branch of `!=` both give.
    const bool asserts = positive == isEqual;

    const Expr* nameSide = condition.args[0].get();
    const Expr* literalSide = condition.args[1].get();
    if (nameSide->kind != ExprKind::Name) {
      std::swap(nameSide, literalSide);
    }
    if (nameSide->kind != ExprKind::Name || literalSide->kind != ExprKind::Integer) {
      return;
    }

    const auto it = env.find(nameSide->name);
    if (it == env.end() || it->second.type.kind != SemKind::Int) {
      return;
    }
    // Only refine a name bound directly to a symbol; substituting into a derived
    // expression would need to solve for the symbol.
    const SymId bound = it->second.value;
    if (!bound.valid() || syms_.node(bound).op != SymOp::Symbol) {
      return;
    }
    const std::string symbolName{syms_.symbolName(syms_.node(bound).symbol)};

    uint64_t value = literalSide->integer;
    if (!asserts) {
      // "not this value" pins the name down only when the name has exactly two
      // possible values, which a one-bit field or an `int(0..1)` parameter does.
      const Binding& binding = it->second;
      if (!binding.hasRange || binding.rangeHigh - binding.rangeLow != 1) {
        return;
      }
      if (value == binding.rangeLow) {
        value = binding.rangeHigh;
      } else if (value == binding.rangeHigh) {
        value = binding.rangeLow;
      } else {
        return;
      }
    }

    substituteEnv(env, symbolName, value);
  }

  void substituteEnv(Env& env, std::string_view symbolName, uint64_t value) {
    for (auto& [name, binding] : env) {
      if (binding.value.valid()) {
        binding.value = syms_.substitute(binding.value, symbolName, value);
        uint64_t folded = 0;
        if (syms_.asConstant(binding.value, folded)) {
          binding.hasRange = true;
          binding.rangeLow = folded;
          binding.rangeHigh = folded;
        }
      }
      if (binding.type.isSized() && binding.type.width.valid()) {
        binding.type.width = syms_.substitute(binding.type.width, symbolName, value);
      }
    }
  }

  /// The values an integer expression can take, when that is known.
  bool rangeOf(const Expr& expr, Env& env, uint64_t& low, uint64_t& high) {
    if (expr.kind == ExprKind::Integer) {
      low = expr.integer;
      high = expr.integer;
      return true;
    }
    if (expr.kind == ExprKind::Name) {
      const auto it = env.find(expr.name);
      if (it != env.end() && it->second.hasRange) {
        low = it->second.rangeLow;
        high = it->second.rangeHigh;
        return true;
      }
    }
    uint64_t constant = 0;
    if (syms_.asConstant(intValueOf(expr, env), constant)) {
      low = constant;
      high = constant;
      return true;
    }
    return false;
  }

  // -- types ----------------------------------------------------------------

  SemType evalType(const TypeExpr& type, Env& env) {
    switch (type.kind) {
      case TypeKind::Int:
        return SemType::integer();
      case TypeKind::Flags:
        return SemType::flags();
      case TypeKind::Void:
        return SemType::voidType();
      case TypeKind::Bits:
      case TypeKind::Float:
        break;
    }
    if (type.width == nullptr) {
      error(type.loc, "a sized type needs a width");
      return SemType::error();
    }
    const SemType widthType = checkExpr(*type.width, env);
    if (widthType.isError()) {
      return SemType::error();
    }
    if (widthType.kind != SemKind::Int) {
      error(type.loc, std::format("a width must be a compile-time integer but is {}",
                                  describe(widthType)));
      return SemType::error();
    }
    const SymId width = intValueOf(*type.width, env);
    uint64_t literal = 0;
    if (syms_.asConstant(width, literal) && (literal == 0 || literal > 2048)) {
      error(type.loc, std::format("width {} is out of range", literal));
      return SemType::error();
    }
    return type.kind == TypeKind::Bits ? SemType::bits(width) : SemType::floating(width);
  }

  void requireAssignable(SourceLoc loc, const SemType& expected, const SemType& actual,
                         std::string_view what) {
    if (expected.isError() || actual.isError()) {
      return;
    }
    if (expected.kind != actual.kind) {
      error(loc, std::format("{} expects {} but got {}", what, describe(expected),
                             describe(actual)));
      return;
    }
    if (!expected.isSized()) {
      return;
    }
    if (syms_.provablyEqual(expected.width, actual.width)) {
      return;
    }
    // Two widths that are both concrete and different is a definite error. When
    // either is symbolic, the mismatch is reported all the same: an unprovable
    // width is exactly the case where a silent truncation would hide.
    error(loc, std::format("{} expects {} but got {}", what, describe(expected),
                           describe(actual)));
  }

  // -- expressions ----------------------------------------------------------

  /// The symbolic value of a compile-time integer expression. Only meaningful
  /// when checkExpr said the expression is an Int.
  SymId intValueOf(const Expr& expr, Env& env) {
    switch (expr.kind) {
      case ExprKind::Integer:
        return syms_.constant(expr.integer);
      case ExprKind::Name: {
        const auto it = env.find(expr.name);
        if (it != env.end() && it->second.value.valid()) {
          return it->second.value;
        }
        return syms_.unknown();
      }
      case ExprKind::Unary: {
        const SymId operand = intValueOf(*expr.args[0], env);
        switch (expr.unaryOp) {
          case UnaryOp::Negate:
            return syms_.unary(SymOp::Negate, operand);
          case UnaryOp::BitNot:
            return syms_.unary(SymOp::Not, operand);
          case UnaryOp::LogicalNot:
            return syms_.binary(SymOp::Equal, operand, syms_.constant(0));
        }
        return syms_.unknown();
      }
      case ExprKind::Binary: {
        const SymId lhs = intValueOf(*expr.args[0], env);
        const SymId rhs = intValueOf(*expr.args[1], env);
        return syms_.binary(toSymOp(expr.binaryOp), lhs, rhs);
      }
      case ExprKind::Conditional:
        return syms_.select(intValueOf(*expr.args[0], env), intValueOf(*expr.args[1], env),
                            intValueOf(*expr.args[2], env));
      case ExprKind::Call:
        // A helper's return value is computed at lift time; the checker only
        // knows its type.
        return syms_.unknown();
      default:
        return syms_.unknown();
    }
  }

  [[nodiscard]] static SymOp toSymOp(BinaryOp op) noexcept {
    switch (op) {
      case BinaryOp::Add:
        return SymOp::Add;
      case BinaryOp::Sub:
        return SymOp::Sub;
      case BinaryOp::Mul:
        return SymOp::Mul;
      case BinaryOp::DivU:
        return SymOp::DivU;
      case BinaryOp::RemU:
        return SymOp::RemU;
      case BinaryOp::And:
        return SymOp::And;
      case BinaryOp::Or:
        return SymOp::Or;
      case BinaryOp::Xor:
        return SymOp::Xor;
      case BinaryOp::Shl:
        return SymOp::Shl;
      case BinaryOp::ShrU:
        return SymOp::ShrU;
      case BinaryOp::ShrS:
        return SymOp::ShrS;
      case BinaryOp::Equal:
        return SymOp::Equal;
      case BinaryOp::NotEqual:
        return SymOp::NotEqual;
      case BinaryOp::LessU:
        return SymOp::LessU;
      case BinaryOp::LessS:
        return SymOp::LessS;
      default:
        // The remaining comparisons and the logical connectives are modelled as
        // unknown rather than approximated: a wrong fold here would silently
        // change a width.
        return SymOp::Add;
    }
  }

  SemType checkExpr(const Expr& expr, Env& env) {
    switch (expr.kind) {
      case ExprKind::Integer:
        return SemType::integer();

      case ExprKind::String:
        return SemType::str();

      case ExprKind::Name: {
        const auto local = env.find(expr.name);
        if (local != env.end()) {
          return local->second.type;
        }
        const auto named = checked_->namedRegs.find(expr.name);
        if (named != checked_->namedRegs.end()) {
          const il::RegisterInfo& info = checked_->registers[named->second];
          return info.regClass == il::RegClass::Flags
                     ? SemType::flags()
                     : SemType::bits(syms_.constant(info.bits));
        }
        error(expr.loc, std::format("unknown name '{}'", expr.name));
        return SemType::error();
      }

      case ExprKind::Unary: {
        const SemType operand = checkExpr(*expr.args[0], env);
        if (operand.isError()) {
          return operand;
        }
        if (expr.unaryOp == UnaryOp::LogicalNot) {
          if (operand.kind == SemKind::Int) {
            return SemType::integer();
          }
          if (operand.kind == SemKind::Bits && isOneBit(operand.width)) {
            return operand;
          }
          error(expr.loc, std::format("'!' needs a compile-time integer or bits(1) but got {}",
                                      describe(operand)));
          return SemType::error();
        }
        if (operand.kind != SemKind::Int && operand.kind != SemKind::Bits) {
          error(expr.loc, std::format("'{}' needs an integer but got {}",
                                      toString(expr.unaryOp), describe(operand)));
          return SemType::error();
        }
        return operand;
      }

      case ExprKind::Binary:
        return checkBinary(expr, env);

      case ExprKind::Conditional: {
        const SemType condition = checkExpr(*expr.args[0], env);
        if (!condition.isError() && condition.kind != SemKind::Int) {
          error(expr.loc,
                std::format("'? :' decides at lift time and needs a compile-time condition, but "
                            "this is {}; use `select` to choose between two runtime values",
                            describe(condition)));
        }
        const SemType ifTrue = checkExpr(*expr.args[1], env);
        const SemType ifFalse = checkExpr(*expr.args[2], env);
        if (ifTrue.isError() || ifFalse.isError()) {
          return SemType::error();
        }
        if (ifTrue.kind != ifFalse.kind) {
          error(expr.loc, std::format("the arms of '? :' are {} and {}", describe(ifTrue),
                                      describe(ifFalse)));
          return SemType::error();
        }
        if (!ifTrue.isSized()) {
          return ifTrue;
        }
        // The chosen width follows the condition, which is known at lift time.
        const SymId width = syms_.select(intValueOf(*expr.args[0], env), ifTrue.width,
                                         ifFalse.width);
        return ifTrue.kind == SemKind::Bits ? SemType::bits(width) : SemType::floating(width);
      }

      case ExprKind::Index:
      case ExprKind::Member: {
        SymId width;
        if (!resolveRegFileAccess(expr, env, width)) {
          return SemType::error();
        }
        return SemType::bits(width);
      }

      case ExprKind::Call:
        return checkCall(expr, env);
    }
    return SemType::error();
  }

  [[nodiscard]] bool isOneBit(SymId width) const {
    uint64_t value = 0;
    return syms_.asConstant(width, value) && value == 1;
  }

  SemType checkBinary(const Expr& expr, Env& env) {
    const SemType lhs = checkExpr(*expr.args[0], env);
    const SemType rhs = checkExpr(*expr.args[1], env);
    if (lhs.isError() || rhs.isError()) {
      return SemType::error();
    }

    const BinaryOp op = expr.binaryOp;
    const bool isComparison =
        op == BinaryOp::Equal || op == BinaryOp::NotEqual || op == BinaryOp::LessU ||
        op == BinaryOp::LessEqualU || op == BinaryOp::LessS || op == BinaryOp::LessEqualS ||
        op == BinaryOp::GreaterU || op == BinaryOp::GreaterEqualU || op == BinaryOp::GreaterS ||
        op == BinaryOp::GreaterEqualS;
    const bool isLogical = op == BinaryOp::LogicalAnd || op == BinaryOp::LogicalOr;

    if (isLogical) {
      // Short-circuit connectives have no IL equivalent, so they are compile-time
      // only. A runtime conjunction is `a & b` on two bits(1) values.
      if (lhs.kind != SemKind::Int || rhs.kind != SemKind::Int) {
        error(expr.loc,
              std::format("'{}' is compile-time only; use '&' or '|' on bits(1) for a runtime "
                          "condition",
                          toString(op)));
        return SemType::error();
      }
      return SemType::integer();
    }

    if (lhs.kind == SemKind::Int && rhs.kind == SemKind::Int) {
      return SemType::integer();
    }

    if (lhs.kind == SemKind::Float || rhs.kind == SemKind::Float) {
      // Float arithmetic has explicit builtins so that rounding is never
      // implied by an operator.
      error(expr.loc,
            std::format("'{}' does not apply to floats; use fadd, fsub, fmul, fdiv or the fcmp "
                        "builtins",
                        toString(op)));
      return SemType::error();
    }

    if (lhs.kind != SemKind::Bits || rhs.kind != SemKind::Bits) {
      error(expr.loc, std::format("'{}' cannot combine {} with {}", toString(op),
                                  describe(lhs), describe(rhs)));
      return SemType::error();
    }

    // A shift amount is allowed to be a compile-time integer; everything else
    // must agree in width. Mixing widths silently is how a 32-bit value ends up
    // in a 64-bit register with the top half undefined.
    const bool isShift =
        op == BinaryOp::Shl || op == BinaryOp::ShrU || op == BinaryOp::ShrS;
    if (!syms_.provablyEqual(lhs.width, rhs.width)) {
      error(expr.loc,
            std::format("'{}' needs operands of one width but got {} and {}", toString(op),
                        describe(lhs), describe(rhs)));
      return SemType::error();
    }
    (void)isShift;

    return isComparison ? SemType::bits(syms_.constant(1)) : lhs;
  }

  // -- calls ----------------------------------------------------------------

  SemType checkCall(const Expr& expr, Env& env) {
    Builtin builtin = Builtin::Count;
    if (lookupBuiltin(expr.name, builtin)) {
      return checkBuiltinCall(builtin, expr, env);
    }

    const auto it = functions_.find(expr.name);
    if (it == functions_.end()) {
      error(expr.loc, std::format("unknown function '{}'", expr.name));
      return SemType::error();
    }
    const FnDecl& callee = *it->second;
    if (expr.args.size() != callee.params.size()) {
      error(expr.loc, std::format("'{}' takes {} arguments but got {}", callee.name,
                                  callee.params.size(), expr.args.size()));
      return SemType::error();
    }

    // Dependent parameter types are resolved by evaluating the callee's declared
    // types in an environment bound to the actual arguments, which is what makes
    // `fn write_gpr(n: int, sf: int, v: bits(32 << sf))` work.
    Env callEnv;
    for (std::size_t index = 0; index < callee.params.size(); ++index) {
      const ParamDecl& param = callee.params[index];
      const SemType actual = checkExpr(*expr.args[index], env);
      Binding binding;
      binding.type = actual;
      if (actual.kind == SemKind::Int) {
        binding.value = intValueOf(*expr.args[index], env);
        binding.hasRange =
            rangeOf(*expr.args[index], env, binding.rangeLow, binding.rangeHigh);
      }
      callEnv[param.name] = binding;

      const SemType expected = evalType(param.type, callEnv);
      requireAssignable(expr.args[index]->loc, expected, actual,
                        std::format("argument '{}' of '{}'", param.name, callee.name));

      // A declared bound is a promise the callee's body relies on, so it is
      // checked here rather than trusted.
      if (param.type.hasRange && actual.kind == SemKind::Int) {
        if (!binding.hasRange) {
          warn(expr.args[index]->loc,
               std::format("cannot show that argument '{}' of '{}' stays within {}..{}",
                           param.name, callee.name, param.type.rangeLow, param.type.rangeHigh));
        } else if (binding.rangeLow < param.type.rangeLow ||
                   binding.rangeHigh > param.type.rangeHigh) {
          error(expr.args[index]->loc,
                std::format("argument '{}' of '{}' ranges over {}..{} but the parameter accepts "
                            "{}..{}",
                            param.name, callee.name, binding.rangeLow, binding.rangeHigh,
                            param.type.rangeLow, param.type.rangeHigh));
        }
      }
    }
    return evalType(callee.result, callEnv);
  }

  SemType checkBuiltinCall(Builtin builtin, const Expr& expr, Env& env) {
    const BuiltinInfo& info = builtinInfo(builtin);
    const std::size_t count = expr.args.size();
    if (count < info.minArity || (info.maxArity != 0xFF && count > info.maxArity)) {
      if (info.maxArity == 0xFF) {
        error(expr.loc, std::format("'{}' takes at least {} arguments but got {}", info.text,
                                    info.minArity, count));
      } else {
        error(expr.loc, std::format("'{}' takes {} arguments but got {}", info.text,
                                    info.minArity, count));
      }
      return SemType::error();
    }

    std::vector<SemType> types;
    types.reserve(count);
    for (const ExprPtr& argument : expr.args) {
      types.push_back(checkExpr(*argument, env));
    }
    for (const SemType& type : types) {
      if (type.isError()) {
        return SemType::error();
      }
    }

    const auto requireKind = [&](std::size_t index, SemKind kind, std::string_view what) {
      if (types[index].kind != kind) {
        error(expr.args[index]->loc,
              std::format("argument {} of '{}' must be {} but is {}", index + 1, info.text,
                          what, describe(types[index])));
        return false;
      }
      return true;
    };
    const auto requireInt = [&](std::size_t index) {
      return requireKind(index, SemKind::Int, "a compile-time integer");
    };
    const auto requireBits = [&](std::size_t index) {
      return requireKind(index, SemKind::Bits, "a bits value");
    };
    const auto requireFloat = [&](std::size_t index) {
      return requireKind(index, SemKind::Float, "a float value");
    };
    const auto requireFlags = [&](std::size_t index) {
      return requireKind(index, SemKind::Flags, "a flags value");
    };
    const auto requireStr = [&](std::size_t index) {
      return requireKind(index, SemKind::Str, "a string literal");
    };
    const auto requireOneBit = [&](std::size_t index) {
      if (!requireBits(index)) {
        return false;
      }
      if (!isOneBit(types[index].width)) {
        error(expr.args[index]->loc,
              std::format("argument {} of '{}' must be bits(1) but is {}", index + 1,
                          info.text, describe(types[index])));
        return false;
      }
      return true;
    };
    const auto sameWidth = [&](std::size_t a, std::size_t b) {
      if (!syms_.provablyEqual(types[a].width, types[b].width)) {
        error(expr.loc, std::format("'{}' needs operands of one width but got {} and {}",
                                    info.text, describe(types[a]), describe(types[b])));
        return false;
      }
      return true;
    };
    /// The declared width argument, plus a range check when it is a literal.
    const auto widthArgument = [&](std::size_t index) {
      const SymId width = intValueOf(*expr.args[index], env);
      uint64_t literal = 0;
      if (syms_.asConstant(width, literal) && (literal == 0 || literal > 2048)) {
        error(expr.args[index]->loc, std::format("width {} is out of range", literal));
      }
      return width;
    };
    /// Rejects a resize that goes the wrong way, when both widths are literal.
    const auto checkResize = [&](std::size_t from, SymId to, bool widening) {
      uint64_t source = 0;
      uint64_t target = 0;
      if (!syms_.asConstant(types[from].width, source) || !syms_.asConstant(to, target)) {
        return;
      }
      if (widening && target < source) {
        error(expr.loc, std::format("'{}' cannot narrow {} bits to {}", info.text, source,
                                    target));
      }
      if (!widening && target > source) {
        error(expr.loc, std::format("'{}' cannot widen {} bits to {}", info.text, source,
                                    target));
      }
    };

    switch (builtin) {
      case Builtin::ZExt:
      case Builtin::SExt: {
        if (!requireBits(0) || !requireInt(1)) {
          return SemType::error();
        }
        const SymId width = widthArgument(1);
        checkResize(0, width, /*widening=*/true);
        return SemType::bits(width);
      }
      case Builtin::Trunc: {
        if (!requireBits(0) || !requireInt(1)) {
          return SemType::error();
        }
        const SymId width = widthArgument(1);
        checkResize(0, width, /*widening=*/false);
        return SemType::bits(width);
      }
      case Builtin::BitcastInt: {
        if (types[0].kind != SemKind::Bits && types[0].kind != SemKind::Float) {
          error(expr.loc, "bitcast_int needs a bits or float value");
          return SemType::error();
        }
        if (!requireInt(1)) {
          return SemType::error();
        }
        return SemType::bits(widthArgument(1));
      }
      case Builtin::BitcastFloat: {
        if (types[0].kind != SemKind::Bits && types[0].kind != SemKind::Float) {
          error(expr.loc, "bitcast_float needs a bits or float value");
          return SemType::error();
        }
        if (!requireInt(1)) {
          return SemType::error();
        }
        return SemType::floating(widthArgument(1));
      }
      case Builtin::Extract: {
        if (!requireBits(0) || !requireInt(1) || !requireInt(2)) {
          return SemType::error();
        }
        const SymId low = intValueOf(*expr.args[1], env);
        const SymId width = widthArgument(2);
        uint64_t lowValue = 0;
        uint64_t widthValue = 0;
        uint64_t sourceValue = 0;
        if (syms_.asConstant(low, lowValue) && syms_.asConstant(width, widthValue) &&
            syms_.asConstant(types[0].width, sourceValue) &&
            lowValue + widthValue > sourceValue) {
          error(expr.loc, std::format("extract reads bits {}..{} of a {} bit value", lowValue,
                                      lowValue + widthValue - 1, sourceValue));
        }
        return SemType::bits(width);
      }
      case Builtin::Concat: {
        if (!requireBits(0) || !requireBits(1)) {
          return SemType::error();
        }
        return SemType::bits(syms_.binary(SymOp::Add, types[0].width, types[1].width));
      }
      case Builtin::Select: {
        if (!requireOneBit(0) || !requireBits(1) || !requireBits(2) || !sameWidth(1, 2)) {
          return SemType::error();
        }
        return types[1];
      }
      case Builtin::SelectFlags: {
        // Separate from Select because a flag bundle has no width, so the
        // width-agreement check that makes Select safe cannot be applied to it.
        if (!requireOneBit(0) || !requireFlags(1) || !requireFlags(2)) {
          return SemType::error();
        }
        return SemType::flags();
      }
      case Builtin::Clz:
      case Builtin::Ctz:
      case Builtin::PopCount:
      case Builtin::ByteSwap:
      case Builtin::BitReverse: {
        if (!requireBits(0)) {
          return SemType::error();
        }
        return types[0];
      }
      case Builtin::RotR:
      case Builtin::RotL:
      case Builtin::MulHiU:
      case Builtin::MulHiS: {
        if (!requireBits(0) || !requireBits(1) || !sameWidth(0, 1)) {
          return SemType::error();
        }
        return types[0];
      }
      case Builtin::FlagDefAdd:
      case Builtin::FlagDefSub: {
        if (!requireBits(0) || !requireBits(1) || !sameWidth(0, 1)) {
          return SemType::error();
        }
        return SemType::flags();
      }
      case Builtin::FlagDefAdc:
      case Builtin::FlagDefSbc: {
        if (!requireBits(0) || !requireBits(1) || !sameWidth(0, 1) || !requireOneBit(2)) {
          return SemType::error();
        }
        return SemType::flags();
      }
      case Builtin::FlagConst:
      case Builtin::FlagDefLogic: {
        if (!requireBits(0)) {
          return SemType::error();
        }
        return SemType::flags();
      }
      case Builtin::Cond: {
        if (!requireFlags(0) || !requireInt(1)) {
          return SemType::error();
        }
        // A condition code must be known when the instruction is decoded: the IL
        // stores it in the node, not as an operand.
        const SymId code = intValueOf(*expr.args[1], env);
        uint64_t value = 0;
        if (!syms_.asConstant(code, value)) {
          if (syms_.isUnknown(code)) {
            error(expr.args[1]->loc,
                  "a condition code must be computable from the decoded fields");
          }
        } else if (value >= static_cast<uint64_t>(il::ConditionCode::Count)) {
          error(expr.args[1]->loc, std::format("condition code {} is out of range", value));
        }
        return SemType::bits(syms_.constant(1));
      }
      case Builtin::FlagBit: {
        if (!requireFlags(0) || !requireInt(1)) {
          return SemType::error();
        }
        const SymId bit = intValueOf(*expr.args[1], env);
        uint64_t value = 0;
        if (syms_.asConstant(bit, value) &&
            value >= static_cast<uint64_t>(il::FlagBitIndex::Count)) {
          error(expr.args[1]->loc, std::format("flag bit {} is out of range", value));
        }
        return SemType::bits(syms_.constant(1));
      }
      case Builtin::Imm: {
        if (!requireInt(0) || !requireInt(1)) {
          return SemType::error();
        }
        const SymId width = widthArgument(1);
        // A literal that does not fit the width it is given is a spec bug: the
        // IL would silently truncate it.
        uint64_t value = 0;
        uint64_t widthValue = 0;
        if (syms_.asConstant(intValueOf(*expr.args[0], env), value) &&
            syms_.asConstant(width, widthValue) && widthValue < 64 &&
            value > lowMask(static_cast<unsigned>(widthValue))) {
          error(expr.loc, std::format("{} does not fit in {} bits", value, widthValue));
        }
        return SemType::bits(width);
      }
      case Builtin::Undef: {
        if (!requireInt(0)) {
          return SemType::error();
        }
        return SemType::bits(widthArgument(0));
      }
      case Builtin::Load: {
        if (!requireBits(0) || !requireInt(1)) {
          return SemType::error();
        }
        requirePointerWidth(expr.args[0]->loc, types[0], "a load address");
        return SemType::bits(widthArgument(1));
      }
      case Builtin::FAdd:
      case Builtin::FSub:
      case Builtin::FMul:
      case Builtin::FDiv: {
        if (!requireFloat(0) || !requireFloat(1) || !sameWidth(0, 1)) {
          return SemType::error();
        }
        return types[0];
      }
      case Builtin::FNeg:
      case Builtin::FAbs:
      case Builtin::FSqrt: {
        if (!requireFloat(0)) {
          return SemType::error();
        }
        return types[0];
      }
      case Builtin::FCmpEq:
      case Builtin::FCmpLt:
      case Builtin::FCmpLe:
      case Builtin::FCmpUno: {
        if (!requireFloat(0) || !requireFloat(1) || !sameWidth(0, 1)) {
          return SemType::error();
        }
        return SemType::bits(syms_.constant(1));
      }
      case Builtin::IntToFpS:
      case Builtin::IntToFpU: {
        if (!requireBits(0) || !requireInt(1)) {
          return SemType::error();
        }
        return SemType::floating(widthArgument(1));
      }
      case Builtin::FpToIntS:
      case Builtin::FpToIntU: {
        if (!requireFloat(0) || !requireInt(1)) {
          return SemType::error();
        }
        return SemType::bits(widthArgument(1));
      }
      case Builtin::FpConvert: {
        if (!requireFloat(0) || !requireInt(1)) {
          return SemType::error();
        }
        return SemType::floating(widthArgument(1));
      }
      case Builtin::SExtInt:
      case Builtin::HighestBitInt:
      case Builtin::OnesInt: {
        for (std::size_t index = 0; index < count; ++index) {
          if (!requireInt(index)) {
            return SemType::error();
          }
        }
        return SemType::integer();
      }
      case Builtin::RorInt:
      case Builtin::ReplicateInt: {
        for (std::size_t index = 0; index < count; ++index) {
          if (!requireInt(index)) {
            return SemType::error();
          }
        }
        return SemType::integer();
      }
      case Builtin::Store: {
        if (!requireBits(0)) {
          return SemType::error();
        }
        requirePointerWidth(expr.args[0]->loc, types[0], "a store address");
        if (types[1].kind != SemKind::Bits && types[1].kind != SemKind::Float) {
          error(expr.args[1]->loc, "a store value must be a bits or float value");
          return SemType::error();
        }
        return SemType::voidType();
      }
      case Builtin::Branch:
      case Builtin::Call: {
        if (!requireInt(0)) {
          return SemType::error();
        }
        return SemType::voidType();
      }
      case Builtin::CondBranch: {
        if (!requireOneBit(0) || !requireInt(1) || !requireInt(2)) {
          return SemType::error();
        }
        return SemType::voidType();
      }
      case Builtin::IndirectBranch:
      case Builtin::IndirectCall: {
        if (!requireBits(0)) {
          return SemType::error();
        }
        requirePointerWidth(expr.args[0]->loc, types[0], "a computed target");
        return SemType::voidType();
      }
      case Builtin::Return:
      case Builtin::Nop:
      case Builtin::Unreachable:
        return SemType::voidType();
      case Builtin::Intrinsic: {
        if (!requireStr(0)) {
          return SemType::error();
        }
        for (std::size_t index = 1; index < count; ++index) {
          if (types[index].kind == SemKind::Str || types[index].kind == SemKind::Void) {
            error(expr.args[index]->loc, "an intrinsic argument must be a value");
            return SemType::error();
          }
        }
        return SemType::voidType();
      }
      case Builtin::IntrinsicValue: {
        if (!requireStr(0) || !requireInt(1)) {
          return SemType::error();
        }
        for (std::size_t index = 2; index < count; ++index) {
          if (types[index].kind == SemKind::Str || types[index].kind == SemKind::Void) {
            error(expr.args[index]->loc, "an intrinsic argument must be a value");
            return SemType::error();
          }
        }
        return SemType::bits(widthArgument(1));
      }
      case Builtin::Unimplemented: {
        if (!requireStr(0)) {
          return SemType::error();
        }
        return SemType::voidType();
      }
      case Builtin::Count:
        break;
    }
    return SemType::error();
  }

  void requirePointerWidth(SourceLoc loc, const SemType& type, std::string_view what) {
    uint64_t width = 0;
    if (!syms_.asConstant(type.width, width)) {
      return;
    }
    if (width != module_.arch.pointerBits) {
      error(loc, std::format("{} must be {} bits on this architecture but is {}", what,
                             module_.arch.pointerBits, width));
    }
  }

  // -- decoder --------------------------------------------------------------

  void buildDecoder() {
    const OverlapReport overlaps = findOverlaps(checked_->patterns);
    for (const OverlapReport::Conflict& conflict : overlaps.conflicts) {
      const InsnDecl& first = module_.instructions[conflict.first];
      const InsnDecl& second = module_.instructions[conflict.second];
      error(second.loc,
            std::format("encodings of '{}' and '{}' both match 0x{:x}; neither is more "
                        "specific, so add a `require` clause or an explicit `priority`",
                        first.name, second.name, conflict.witness));
    }
    checked_->decoder = buildDecisionTree(checked_->patterns, module_.arch.insnWidth);
  }

  // -- shared name sets -----------------------------------------------------

  static const std::unordered_set<std::string>& reservedNames() {
    static const std::unordered_set<std::string> kNames = {"insn_pc", "insn_len", "opcode"};
    return kNames;
  }

  static const std::unordered_set<std::string>& asmStyles() {
    static const std::unordered_set<std::string> kStyles = {"reg",   "regsp", "vreg",  "hex",
                                                            "dec",   "cond",  "label", "shift",
                                                            "extend"};
    return kStyles;
  }

  const Module& module_;
  std::unique_ptr<CheckedModule> checked_;
  CheckReport report_;
  SymPool syms_;
  std::unordered_map<std::string, const FnDecl*> functions_;
  std::unordered_set<std::string> insnNames_;
  std::unordered_map<std::string, unsigned> fieldBits_;
  const FnDecl* currentFunction_ = nullptr;
  const TypeExpr* currentReturn_ = nullptr;
};

}  // namespace

std::string CheckReport::format() const {
  std::string text;
  for (const Diag& diag : errors) {
    text += std::format("error: {}\n", diag.format());
  }
  for (const Diag& diag : warnings) {
    text += std::format("warning: {}\n", diag.format());
  }
  if (text.empty()) {
    text = "ok\n";
  }
  return text;
}

const RegFileBinding* CheckedModule::findRegFile(std::string_view name) const {
  for (const RegFileBinding& file : regFiles) {
    if (file.name == name) {
      return &file;
    }
  }
  return nullptr;
}

const CheckedInsn* CheckedModule::findInsn(std::string_view name) const {
  for (const CheckedInsn& insn : instructions) {
    if (insn.name == name) {
      return &insn;
    }
  }
  return nullptr;
}

CheckResult check(const Module& module) {
  Checker checker{module};
  return checker.run();
}

Result<std::unique_ptr<CheckedModule>> checkOrFail(const Module& module) {
  CheckResult result = check(module);
  if (result.report.ok()) {
    return std::move(result.module);
  }
  Diag diag{DiagCode::ParseError,
            std::format("spec '{}' has {} error(s)", module.sourceName,
                        result.report.errors.size())};
  for (const Diag& entry : result.report.errors) {
    diag.note(entry.format());
  }
  return err(std::move(diag));
}

const std::vector<std::string>& builtinNames() {
  static const std::vector<std::string> kNames = [] {
    std::vector<std::string> names;
    for (unsigned index = 0; index < static_cast<unsigned>(Builtin::Count); ++index) {
      names.emplace_back(builtinInfo(static_cast<Builtin>(index)).text);
    }
    return names;
  }();
  return kNames;
}

}  // namespace xdec::spec
