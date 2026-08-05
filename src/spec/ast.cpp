#include "xdec/spec/ast.h"

#include <format>

namespace xdec::spec {

std::string SourceLoc::toString() const { return std::format("{}:{}", line, column); }

ExprPtr Expr::makeInteger(SourceLoc loc, uint64_t value) {
  auto expr = std::make_unique<Expr>();
  expr->kind = ExprKind::Integer;
  expr->loc = loc;
  expr->integer = value;
  return expr;
}

ExprPtr Expr::makeName(SourceLoc loc, std::string name) {
  auto expr = std::make_unique<Expr>();
  expr->kind = ExprKind::Name;
  expr->loc = loc;
  expr->name = std::move(name);
  return expr;
}

namespace {

ExprPtr cloneExpr(const Expr* expr) {
  if (expr == nullptr) {
    return nullptr;
  }
  auto copy = std::make_unique<Expr>();
  copy->kind = expr->kind;
  copy->loc = expr->loc;
  copy->integer = expr->integer;
  copy->name = expr->name;
  copy->unaryOp = expr->unaryOp;
  copy->binaryOp = expr->binaryOp;
  copy->args.reserve(expr->args.size());
  for (const ExprPtr& arg : expr->args) {
    copy->args.push_back(cloneExpr(arg.get()));
  }
  return copy;
}

}  // namespace

TypeExpr TypeExpr::clone() const {
  TypeExpr copy;
  copy.kind = kind;
  copy.loc = loc;
  copy.width = cloneExpr(width.get());
  copy.hasRange = hasRange;
  copy.rangeLow = rangeLow;
  copy.rangeHigh = rangeHigh;
  return copy;
}

std::string_view toString(UnaryOp op) noexcept {
  switch (op) {
    case UnaryOp::Negate:
      return "-";
    case UnaryOp::BitNot:
      return "~";
    case UnaryOp::LogicalNot:
      return "!";
  }
  return "?";
}

std::string_view toString(BinaryOp op) noexcept {
  switch (op) {
    case BinaryOp::Add:
      return "+";
    case BinaryOp::Sub:
      return "-";
    case BinaryOp::Mul:
      return "*";
    case BinaryOp::DivU:
      return "/";
    case BinaryOp::DivS:
      return "/s";
    case BinaryOp::RemU:
      return "%";
    case BinaryOp::RemS:
      return "%s";
    case BinaryOp::And:
      return "&";
    case BinaryOp::Or:
      return "|";
    case BinaryOp::Xor:
      return "^";
    case BinaryOp::Shl:
      return "<<";
    case BinaryOp::ShrU:
      return ">>";
    case BinaryOp::ShrS:
      return ">>>";
    case BinaryOp::Equal:
      return "==";
    case BinaryOp::NotEqual:
      return "!=";
    case BinaryOp::LessU:
      return "<u";
    case BinaryOp::LessEqualU:
      return "<=u";
    case BinaryOp::LessS:
      return "<";
    case BinaryOp::LessEqualS:
      return "<=";
    case BinaryOp::GreaterU:
      return ">u";
    case BinaryOp::GreaterEqualU:
      return ">=u";
    case BinaryOp::GreaterS:
      return ">";
    case BinaryOp::GreaterEqualS:
      return ">=";
    case BinaryOp::LogicalAnd:
      return "&&";
    case BinaryOp::LogicalOr:
      return "||";
  }
  return "?";
}

std::string_view toString(RegRole role) noexcept {
  switch (role) {
    case RegRole::General:
      return "general";
    case RegRole::Float:
      return "float";
    case RegRole::Vector:
      return "vector";
    case RegRole::Flags:
      return "flags";
    case RegRole::StackPointer:
      return "stack";
    case RegRole::ProgramCounter:
      return "pc";
    case RegRole::Special:
      return "special";
  }
  return "special";
}

bool parseRegRole(std::string_view text, RegRole& out) noexcept {
  if (text == "general") {
    out = RegRole::General;
  } else if (text == "float") {
    out = RegRole::Float;
  } else if (text == "vector") {
    out = RegRole::Vector;
  } else if (text == "flags") {
    out = RegRole::Flags;
  } else if (text == "stack") {
    out = RegRole::StackPointer;
  } else if (text == "pc") {
    out = RegRole::ProgramCounter;
  } else if (text == "special") {
    out = RegRole::Special;
  } else {
    return false;
  }
  return true;
}

}  // namespace xdec::spec
