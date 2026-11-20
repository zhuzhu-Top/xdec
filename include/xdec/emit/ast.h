// High-level structured statement/expression AST produced by AST lowering
// (ast_lower.cpp) and consumed by language backends (CBackend, future ObjcBackend,
// or plugin AST passes).
//
// Keeps CFG/SSA/Phi lowering completely separated from target-language syntax
// formatting so new output dialects (C, Objective-C, Swift, etc.) can walk or
// rewrite a uniform statement & call tree without touching SSA or ExprPrinter.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace xdec::il {
class Function;
}

namespace xdec::emit {

enum class EmitLanguage : uint8_t {
  C,
  Objc,
};

struct HlExpr;
struct HlStmt;
using HlExprPtr = std::unique_ptr<HlExpr>;
using HlStmtPtr = std::unique_ptr<HlStmt>;

enum class HlExprKind : uint8_t {
  Raw,          // Pre-rendered low-level expression (arithmetic/MBA/cast)
  Var,          // Identifier: local, argument, temp, or CSE variable
  Const,        // Integer constant (rendered as hex)
  StringLit,    // Quoted C string literal (raw unescaped characters in `text`)
  CFStringLit,  // Apple CFString reference (`name` + decoded `text`)
  Unary,        // `op operand`
  Binary,       // `lhs op rhs`
  Cast,         // `(type)operand`
  Deref,        // `*address` or `*(type*)(address)`
  AddressOf,    // `&inner`
  Call,         // Structured function/indirect call
  ObjcMsg,      // Objective-C message send `[receiver sel:args...]`
};

struct HlExpr {
  HlExprKind kind = HlExprKind::Raw;
  std::string text;       // Raw text, Var name, StringLit/CFStringLit text, op, or type
  std::string extra;      // CFString symbol name, cast prefix, or callee prototype
  uint64_t constant = 0;  // Const value or callee VA

  // Call / ObjcMsg metadata
  bool isDirectCall = false;
  bool isTailCall = false;
  bool isNoreturn = false;
  std::string importedName;
  std::string callSuffix;
  std::string callNote;

  HlExprPtr lhs;
  HlExprPtr rhs;
  std::vector<HlExprPtr> args;

  [[nodiscard]] static HlExprPtr makeRaw(std::string exprText) {
    auto e = std::make_unique<HlExpr>();
    e->kind = HlExprKind::Raw;
    e->text = std::move(exprText);
    return e;
  }

  [[nodiscard]] static HlExprPtr makeVar(std::string name) {
    auto e = std::make_unique<HlExpr>();
    e->kind = HlExprKind::Var;
    e->text = std::move(name);
    return e;
  }

  [[nodiscard]] static HlExprPtr makeConst(uint64_t value) {
    auto e = std::make_unique<HlExpr>();
    e->kind = HlExprKind::Const;
    e->constant = value;
    return e;
  }

  [[nodiscard]] static HlExprPtr makeStringLit(std::string unescapedText) {
    auto e = std::make_unique<HlExpr>();
    e->kind = HlExprKind::StringLit;
    e->text = std::move(unescapedText);
    return e;
  }

  [[nodiscard]] static HlExprPtr makeCFStringLit(std::string symbolName, std::string literalText) {
    auto e = std::make_unique<HlExpr>();
    e->kind = HlExprKind::CFStringLit;
    e->extra = std::move(symbolName);
    e->text = std::move(literalText);
    return e;
  }

  [[nodiscard]] static HlExprPtr makeUnary(std::string op, HlExprPtr operand) {
    auto e = std::make_unique<HlExpr>();
    e->kind = HlExprKind::Unary;
    e->text = std::move(op);
    e->lhs = std::move(operand);
    return e;
  }

  [[nodiscard]] static HlExprPtr makeBinary(std::string op, HlExprPtr left, HlExprPtr right) {
    auto e = std::make_unique<HlExpr>();
    e->kind = HlExprKind::Binary;
    e->text = std::move(op);
    e->lhs = std::move(left);
    e->rhs = std::move(right);
    return e;
  }

  [[nodiscard]] static HlExprPtr makeCast(std::string typeName, HlExprPtr operand) {
    auto e = std::make_unique<HlExpr>();
    e->kind = HlExprKind::Cast;
    e->text = std::move(typeName);
    e->lhs = std::move(operand);
    return e;
  }

  [[nodiscard]] static HlExprPtr makeCall(HlExprPtr calleeExpr,
                                          std::vector<HlExprPtr> arguments) {
    auto e = std::make_unique<HlExpr>();
    e->kind = HlExprKind::Call;
    e->lhs = std::move(calleeExpr);
    e->args = std::move(arguments);
    return e;
  }
};

enum class HlStmtKind : uint8_t {
  Sequence,       // Flat list of statements (`items`)
  Scope,          // Braced `{ ... }` block (`items`), e.g. parallel phi snapshots
  Label,          // `L_0x...:` (`text`)
  Comment,        // `/* ... */` or `// ...` (`text`, `lineComment`)
  VarDecl,        // `const T name = init;` (`typeText`, `text`, `lhs`, `isConst`)
  Assign,         // `lhs = rhs;` (with optional trailing `text` comment)
  MultiAssign,    // `a = b = ... = rhs;` (`names`, `rhs`, optional `text` comment)
  ExprStmt,       // `expr;` (`lhs`)
  If,             // `if (cond) { thenArm } else { elseArm }`
  While,          // `while (cond) { body }` (null `cond` = `while (true)`)
  DoWhile,        // `do { body } while (cond);`
  Switch,         // `switch (cond) { cases... }`, optional `elseArm` epilogue
  Return,         // `return;` or `return expr;` (`lhs`)
  Goto,           // `goto label;` (`text`)
  Break,          // `break;`
  Continue,       // `continue;`
  Unimplemented,  // `__xdec_unimplemented("...");` (`text`)
  Unreachable,    // `__builtin_unreachable();`
};

struct HlSwitchCase {
  bool isDefault = false;
  uint64_t value = 0;
  std::string comment;
  HlStmtPtr body;
};

struct HlStmt {
  HlStmtKind kind = HlStmtKind::Sequence;
  std::string text;       // Label, Comment, Goto target, VarDecl name, Unimplemented text
  std::string typeText;   // VarDecl type
  bool isConst = false;
  bool lineComment = false;  // Comment: `//` when true, `/* ... */` when false

  HlExprPtr cond;
  HlExprPtr lhs;
  HlExprPtr rhs;
  std::vector<std::string> names;  // MultiAssign targets

  std::vector<HlStmtPtr> items;    // Sequence / Scope
  HlStmtPtr thenArm;               // If / While / DoWhile body
  HlStmtPtr elseArm;               // If else / Switch epilogue
  std::vector<HlSwitchCase> cases;

  [[nodiscard]] static HlStmtPtr makeSequence() {
    auto s = std::make_unique<HlStmt>();
    s->kind = HlStmtKind::Sequence;
    return s;
  }

  [[nodiscard]] static HlStmtPtr makeScope() {
    auto s = std::make_unique<HlStmt>();
    s->kind = HlStmtKind::Scope;
    return s;
  }

  [[nodiscard]] static HlStmtPtr makeLabel(std::string name) {
    auto s = std::make_unique<HlStmt>();
    s->kind = HlStmtKind::Label;
    s->text = std::move(name);
    return s;
  }

  [[nodiscard]] static HlStmtPtr makeComment(std::string comment, bool slashSlash = false) {
    auto s = std::make_unique<HlStmt>();
    s->kind = HlStmtKind::Comment;
    s->text = std::move(comment);
    s->lineComment = slashSlash;
    return s;
  }

  [[nodiscard]] static HlStmtPtr makeVarDecl(std::string type, std::string name,
                                             HlExprPtr init, bool constQual = false) {
    auto s = std::make_unique<HlStmt>();
    s->kind = HlStmtKind::VarDecl;
    s->typeText = std::move(type);
    s->text = std::move(name);
    s->lhs = std::move(init);
    s->isConst = constQual;
    return s;
  }

  [[nodiscard]] static HlStmtPtr makeAssign(HlExprPtr target, HlExprPtr value,
                                            std::string trailingComment = {}) {
    auto s = std::make_unique<HlStmt>();
    s->kind = HlStmtKind::Assign;
    s->lhs = std::move(target);
    s->rhs = std::move(value);
    s->text = std::move(trailingComment);
    return s;
  }

  [[nodiscard]] static HlStmtPtr makeExprStmt(HlExprPtr expr) {
    auto s = std::make_unique<HlStmt>();
    s->kind = HlStmtKind::ExprStmt;
    s->lhs = std::move(expr);
    return s;
  }

  [[nodiscard]] static HlStmtPtr makeIf(HlExprPtr condition, HlStmtPtr thenBody,
                                        HlStmtPtr elseBody = nullptr) {
    auto s = std::make_unique<HlStmt>();
    s->kind = HlStmtKind::If;
    s->cond = std::move(condition);
    s->thenArm = std::move(thenBody);
    s->elseArm = std::move(elseBody);
    return s;
  }

  [[nodiscard]] static HlStmtPtr makeWhile(HlExprPtr condition, HlStmtPtr body) {
    auto s = std::make_unique<HlStmt>();
    s->kind = HlStmtKind::While;
    s->cond = std::move(condition);
    s->thenArm = std::move(body);
    return s;
  }

  [[nodiscard]] static HlStmtPtr makeDoWhile(HlStmtPtr body, HlExprPtr condition) {
    auto s = std::make_unique<HlStmt>();
    s->kind = HlStmtKind::DoWhile;
    s->thenArm = std::move(body);
    s->cond = std::move(condition);
    return s;
  }

  [[nodiscard]] static HlStmtPtr makeReturn(HlExprPtr value = nullptr) {
    auto s = std::make_unique<HlStmt>();
    s->kind = HlStmtKind::Return;
    s->lhs = std::move(value);
    return s;
  }

  [[nodiscard]] static HlStmtPtr makeGoto(std::string targetLabel) {
    auto s = std::make_unique<HlStmt>();
    s->kind = HlStmtKind::Goto;
    s->text = std::move(targetLabel);
    return s;
  }

  [[nodiscard]] static HlStmtPtr makeSimple(HlStmtKind k, std::string payload = {}) {
    auto s = std::make_unique<HlStmt>();
    s->kind = k;
    s->text = std::move(payload);
    return s;
  }
};

struct HlFunction {
  std::string name;
  EmitLanguage language = EmitLanguage::C;
  HlStmtPtr body;
};

/// Post-lowering high-level AST transform hook (runs after `lowerToHlAst` and
/// before the target language backend renders `HlFunction` to text).
using HlAstTransformFn = std::function<void(const il::Function&, HlFunction&)>;

}  // namespace xdec::emit
