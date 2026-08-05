#include "xdec/spec/symint.h"

#include <format>

namespace xdec::spec {

bool operator==(const SymNode& lhs, const SymNode& rhs) noexcept {
  if (lhs.op != rhs.op || lhs.value != rhs.value || lhs.symbol != rhs.symbol ||
      lhs.operandCount != rhs.operandCount) {
    return false;
  }
  for (unsigned index = 0; index < lhs.operandCount; ++index) {
    if (lhs.operands[index] != rhs.operands[index]) {
      return false;
    }
  }
  return true;
}

SymPool::SymPool() {
  SymNode node;
  node.op = SymOp::Unknown;
  unknown_ = intern(node);
}

SymId SymPool::intern(const SymNode& node) {
  const auto it = index_.find(node);
  if (it != index_.end()) {
    return it->second;
  }
  const SymId id = nodes_.emplace(node);
  index_.emplace(node, id);
  return id;
}

uint32_t SymPool::internSymbol(std::string_view name) {
  const std::string key{name};
  const auto it = symbolIndex_.find(key);
  if (it != symbolIndex_.end()) {
    return it->second;
  }
  const auto id = static_cast<uint32_t>(symbolNames_.size());
  symbolNames_.push_back(key);
  symbolIndex_.emplace(key, id);
  return id;
}

SymId SymPool::constant(uint64_t value) {
  SymNode node;
  node.op = SymOp::Const;
  node.value = value;
  return intern(node);
}

SymId SymPool::symbol(std::string_view name) {
  SymNode node;
  node.op = SymOp::Symbol;
  node.symbol = internSymbol(name);
  return intern(node);
}

SymId SymPool::unknown() { return unknown_; }

bool SymPool::isConstant(SymId id) const {
  return nodes_.contains(id) && nodes_[id].op == SymOp::Const;
}

bool SymPool::asConstant(SymId id, uint64_t& out) const {
  if (!isConstant(id)) {
    return false;
  }
  out = nodes_[id].value;
  return true;
}

bool SymPool::isUnknown(SymId id) const {
  return !nodes_.contains(id) || nodes_[id].op == SymOp::Unknown;
}

bool SymPool::provablyEqual(SymId lhs, SymId rhs) const {
  // Hash-consing makes identical structure identical handles, so this is exact
  // for everything the folder normalises and conservative elsewhere. An unknown
  // is never equal even to itself: two unmodelled values may well differ.
  return lhs == rhs && !isUnknown(lhs);
}

namespace {

/// Folds an operation on two literals. Division and remainder by zero are left
/// unfolded rather than given a made-up value.
[[nodiscard]] bool foldBinary(SymOp op, uint64_t a, uint64_t b, uint64_t& out) noexcept {
  switch (op) {
    case SymOp::Add:
      out = a + b;
      return true;
    case SymOp::Sub:
      out = a - b;
      return true;
    case SymOp::Mul:
      out = a * b;
      return true;
    case SymOp::DivU:
      if (b == 0) {
        return false;
      }
      out = a / b;
      return true;
    case SymOp::RemU:
      if (b == 0) {
        return false;
      }
      out = a % b;
      return true;
    case SymOp::And:
      out = a & b;
      return true;
    case SymOp::Or:
      out = a | b;
      return true;
    case SymOp::Xor:
      out = a ^ b;
      return true;
    case SymOp::Shl:
      out = b >= 64 ? 0 : a << b;
      return true;
    case SymOp::ShrU:
      out = b >= 64 ? 0 : a >> b;
      return true;
    case SymOp::ShrS: {
      const auto signedValue = static_cast<int64_t>(a);
      out = static_cast<uint64_t>(b >= 64 ? (signedValue < 0 ? -1 : 0) : signedValue >> b);
      return true;
    }
    case SymOp::Equal:
      out = a == b ? 1 : 0;
      return true;
    case SymOp::NotEqual:
      out = a != b ? 1 : 0;
      return true;
    case SymOp::LessU:
      out = a < b ? 1 : 0;
      return true;
    case SymOp::LessS:
      out = static_cast<int64_t>(a) < static_cast<int64_t>(b) ? 1 : 0;
      return true;
    default:
      return false;
  }
}

/// Algebraic identities worth folding, because they turn up constantly in width
/// arithmetic and keep two spellings of one width interning to one node.
[[nodiscard]] bool foldIdentity(SymOp op, uint64_t literal, bool literalIsRight,
                                SymId other, SymId& out) noexcept {
  switch (op) {
    case SymOp::Add:
    case SymOp::Or:
    case SymOp::Xor:
      if (literal == 0) {
        out = other;
        return true;
      }
      return false;
    case SymOp::Sub:
      if (literal == 0 && literalIsRight) {
        out = other;
        return true;
      }
      return false;
    case SymOp::Mul:
      if (literal == 1) {
        out = other;
        return true;
      }
      return false;
    case SymOp::Shl:
    case SymOp::ShrU:
    case SymOp::ShrS:
      if (literal == 0 && literalIsRight) {
        out = other;
        return true;
      }
      return false;
    case SymOp::DivU:
      if (literal == 1 && literalIsRight) {
        out = other;
        return true;
      }
      return false;
    default:
      return false;
  }
}

[[nodiscard]] bool isCommutative(SymOp op) noexcept {
  switch (op) {
    case SymOp::Add:
    case SymOp::Mul:
    case SymOp::And:
    case SymOp::Or:
    case SymOp::Xor:
    case SymOp::Equal:
    case SymOp::NotEqual:
      return true;
    default:
      return false;
  }
}

}  // namespace

SymId SymPool::unary(SymOp op, SymId operand) {
  if (isUnknown(operand)) {
    return unknown_;
  }
  uint64_t value = 0;
  if (asConstant(operand, value)) {
    switch (op) {
      case SymOp::Not:
        return constant(~value);
      case SymOp::Negate:
        return constant(~value + 1);
      default:
        break;
    }
  }
  SymNode node;
  node.op = op;
  node.operandCount = 1;
  node.operands[0] = operand;
  return intern(node);
}

SymId SymPool::binary(SymOp op, SymId lhs, SymId rhs) {
  if (isUnknown(lhs) || isUnknown(rhs)) {
    return unknown_;
  }

  uint64_t left = 0;
  uint64_t right = 0;
  const bool leftConst = asConstant(lhs, left);
  const bool rightConst = asConstant(rhs, right);

  if (leftConst && rightConst) {
    uint64_t folded = 0;
    if (foldBinary(op, left, right, folded)) {
      return constant(folded);
    }
  } else if (leftConst || rightConst) {
    SymId simplified;
    if (foldIdentity(op, leftConst ? left : right, rightConst, leftConst ? rhs : lhs,
                     simplified)) {
      return simplified;
    }
  }

  // x == x and x - x are provable without knowing x, and both appear in width
  // arithmetic often enough to be worth folding.
  if (lhs == rhs) {
    switch (op) {
      case SymOp::Sub:
      case SymOp::Xor:
        return constant(0);
      case SymOp::And:
      case SymOp::Or:
        return lhs;
      case SymOp::Equal:
        return constant(1);
      case SymOp::NotEqual:
      case SymOp::LessU:
      case SymOp::LessS:
        return constant(0);
      default:
        break;
    }
  }

  SymNode node;
  node.op = op;
  node.operandCount = 2;
  // Canonical operand order for commutative operations, so that `a + b` and
  // `b + a` are one node.
  if (isCommutative(op) && rhs < lhs) {
    node.operands[0] = rhs;
    node.operands[1] = lhs;
  } else {
    node.operands[0] = lhs;
    node.operands[1] = rhs;
  }
  return intern(node);
}

SymId SymPool::select(SymId condition, SymId ifTrue, SymId ifFalse) {
  uint64_t value = 0;
  if (asConstant(condition, value)) {
    return value != 0 ? ifTrue : ifFalse;
  }
  if (ifTrue == ifFalse) {
    return ifTrue;
  }
  if (isUnknown(condition) || isUnknown(ifTrue) || isUnknown(ifFalse)) {
    return unknown_;
  }
  SymNode node;
  node.op = SymOp::Select;
  node.operandCount = 3;
  node.operands[0] = condition;
  node.operands[1] = ifTrue;
  node.operands[2] = ifFalse;
  return intern(node);
}

SymId SymPool::substitute(SymId id, std::string_view name, uint64_t value) {
  if (!nodes_.contains(id)) {
    return id;
  }
  const SymNode& node = nodes_[id];
  switch (node.op) {
    case SymOp::Const:
    case SymOp::Unknown:
      return id;
    case SymOp::Symbol:
      return symbolName(node.symbol) == name ? constant(value) : id;
    default:
      break;
  }

  // Copy first: rebuilding may reallocate the node pool.
  const SymOp op = node.op;
  const uint8_t count = node.operandCount;
  SymId operands[3] = {node.operands[0], node.operands[1], node.operands[2]};

  bool changed = false;
  for (unsigned index = 0; index < count; ++index) {
    const SymId replaced = substitute(operands[index], name, value);
    changed = changed || replaced != operands[index];
    operands[index] = replaced;
  }
  if (!changed) {
    return id;
  }
  if (op == SymOp::Select) {
    return select(operands[0], operands[1], operands[2]);
  }
  if (count == 1) {
    return unary(op, operands[0]);
  }
  return binary(op, operands[0], operands[1]);
}

std::string_view SymPool::symbolName(uint32_t id) const {
  return id < symbolNames_.size() ? std::string_view{symbolNames_[id]} : std::string_view{"?"};
}

std::string SymPool::toString(SymId id) const {
  if (!nodes_.contains(id)) {
    return "<invalid>";
  }
  const SymNode& node = nodes_[id];
  const auto operand = [this, &node](unsigned index) { return toString(node.operands[index]); };

  switch (node.op) {
    case SymOp::Const:
      return std::format("{}", node.value);
    case SymOp::Symbol:
      return std::string{symbolName(node.symbol)};
    case SymOp::Unknown:
      return "?";
    case SymOp::Not:
      return std::format("~{}", operand(0));
    case SymOp::Negate:
      return std::format("-{}", operand(0));
    case SymOp::Select:
      return std::format("({} ? {} : {})", operand(0), operand(1), operand(2));
    default:
      break;
  }

  std::string_view text = "?";
  switch (node.op) {
    case SymOp::Add:
      text = "+";
      break;
    case SymOp::Sub:
      text = "-";
      break;
    case SymOp::Mul:
      text = "*";
      break;
    case SymOp::DivU:
      text = "/";
      break;
    case SymOp::RemU:
      text = "%";
      break;
    case SymOp::And:
      text = "&";
      break;
    case SymOp::Or:
      text = "|";
      break;
    case SymOp::Xor:
      text = "^";
      break;
    case SymOp::Shl:
      text = "<<";
      break;
    case SymOp::ShrU:
      text = ">>";
      break;
    case SymOp::ShrS:
      text = ">>>";
      break;
    case SymOp::Equal:
      text = "==";
      break;
    case SymOp::NotEqual:
      text = "!=";
      break;
    case SymOp::LessU:
      text = "<u";
      break;
    case SymOp::LessS:
      text = "<";
      break;
    default:
      break;
  }
  return std::format("({} {} {})", operand(0), text, operand(1));
}

}  // namespace xdec::spec
