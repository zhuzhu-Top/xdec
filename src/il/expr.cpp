#include "xdec/il/expr.h"

#include <array>
#include <string>
#include <unordered_map>

namespace xdec::il {
namespace {

constexpr std::array<ExprOpInfo, static_cast<std::size_t>(ExprOp::Count)> kExprOpInfo = {{
#define XDEC_EXPR_OP_INFO(name, text, minArity, maxArity, category, result) \
  ExprOpInfo{text, minArity, maxArity, ExprCategory::category, ResultRule::result},
    XDEC_EXPR_OPS(XDEC_EXPR_OP_INFO)
#undef XDEC_EXPR_OP_INFO
}};

/// Built once from the same table the printer uses, so a name can never be
/// printable but unparseable.
const std::unordered_map<std::string_view, ExprOp>& exprOpByName() {
  static const std::unordered_map<std::string_view, ExprOp> table = [] {
    std::unordered_map<std::string_view, ExprOp> map;
    for (std::size_t index = 0; index < kExprOpInfo.size(); ++index) {
      map.emplace(kExprOpInfo[index].text, static_cast<ExprOp>(index));
    }
    return map;
  }();
  return table;
}

}  // namespace

const ExprOpInfo& info(ExprOp op) noexcept {
  const auto index = static_cast<std::size_t>(op);
  XDEC_ASSERT(index < kExprOpInfo.size(), "expression op out of range");
  return kExprOpInfo[index];
}

std::string_view toString(ExprOp op) noexcept { return info(op).text; }

bool parseExprOp(std::string_view text, ExprOp& out) noexcept {
  const auto& table = exprOpByName();
  const auto it = table.find(text);
  if (it == table.end()) {
    return false;
  }
  out = it->second;
  return true;
}

std::string_view toString(FlagOp op) noexcept {
  switch (op) {
    case FlagOp::Add:
      return "add";
    case FlagOp::Sub:
      return "sub";
    case FlagOp::AddCarry:
      return "adc";
    case FlagOp::SubCarry:
      return "sbc";
    case FlagOp::Logical:
      return "logic";
    case FlagOp::Const:
      return "const";
    case FlagOp::Count:
      break;
  }
  return "invalid";
}

bool parseFlagOp(std::string_view text, FlagOp& out) noexcept {
  if (text == "add") {
    out = FlagOp::Add;
  } else if (text == "sub") {
    out = FlagOp::Sub;
  } else if (text == "adc") {
    out = FlagOp::AddCarry;
  } else if (text == "sbc") {
    out = FlagOp::SubCarry;
  } else if (text == "logic") {
    out = FlagOp::Logical;
  } else if (text == "const") {
    out = FlagOp::Const;
  } else {
    return false;
  }
  return true;
}

std::string_view toString(FlagBitIndex bit) noexcept {
  switch (bit) {
    case FlagBitIndex::Negative:
      return "n";
    case FlagBitIndex::Zero:
      return "z";
    case FlagBitIndex::Carry:
      return "c";
    case FlagBitIndex::Overflow:
      return "v";
    case FlagBitIndex::Count:
      break;
  }
  return "invalid";
}

bool parseFlagBit(std::string_view text, FlagBitIndex& out) noexcept {
  if (text == "n") {
    out = FlagBitIndex::Negative;
  } else if (text == "z") {
    out = FlagBitIndex::Zero;
  } else if (text == "c") {
    out = FlagBitIndex::Carry;
  } else if (text == "v") {
    out = FlagBitIndex::Overflow;
  } else {
    return false;
  }
  return true;
}

std::string_view toString(ConditionCode code) noexcept {
  switch (code) {
    case ConditionCode::Equal:
      return "eq";
    case ConditionCode::NotEqual:
      return "ne";
    case ConditionCode::CarrySet:
      return "cs";
    case ConditionCode::CarryClear:
      return "cc";
    case ConditionCode::Negative:
      return "mi";
    case ConditionCode::NonNegative:
      return "pl";
    case ConditionCode::Overflow:
      return "vs";
    case ConditionCode::NoOverflow:
      return "vc";
    case ConditionCode::UnsignedGreater:
      return "hi";
    case ConditionCode::UnsignedLessEqual:
      return "ls";
    case ConditionCode::SignedGreaterEqual:
      return "ge";
    case ConditionCode::SignedLess:
      return "lt";
    case ConditionCode::SignedGreater:
      return "gt";
    case ConditionCode::SignedLessEqual:
      return "le";
    case ConditionCode::Always:
      return "al";
    case ConditionCode::Never:
      return "nv";
    case ConditionCode::Count:
      break;
  }
  return "invalid";
}

bool parseConditionCode(std::string_view text, ConditionCode& out) noexcept {
  static const std::unordered_map<std::string_view, ConditionCode> table = {
      {"eq", ConditionCode::Equal},
      {"ne", ConditionCode::NotEqual},
      {"cs", ConditionCode::CarrySet},
      {"cc", ConditionCode::CarryClear},
      {"mi", ConditionCode::Negative},
      {"pl", ConditionCode::NonNegative},
      {"vs", ConditionCode::Overflow},
      {"vc", ConditionCode::NoOverflow},
      {"hi", ConditionCode::UnsignedGreater},
      {"ls", ConditionCode::UnsignedLessEqual},
      {"ge", ConditionCode::SignedGreaterEqual},
      {"lt", ConditionCode::SignedLess},
      {"gt", ConditionCode::SignedGreater},
      {"le", ConditionCode::SignedLessEqual},
      {"al", ConditionCode::Always},
      {"nv", ConditionCode::Never},
  };
  const auto it = table.find(text);
  if (it == table.end()) {
    return false;
  }
  out = it->second;
  return true;
}

ConditionCode invert(ConditionCode code) noexcept {
  switch (code) {
    case ConditionCode::Equal:
      return ConditionCode::NotEqual;
    case ConditionCode::NotEqual:
      return ConditionCode::Equal;
    case ConditionCode::CarrySet:
      return ConditionCode::CarryClear;
    case ConditionCode::CarryClear:
      return ConditionCode::CarrySet;
    case ConditionCode::Negative:
      return ConditionCode::NonNegative;
    case ConditionCode::NonNegative:
      return ConditionCode::Negative;
    case ConditionCode::Overflow:
      return ConditionCode::NoOverflow;
    case ConditionCode::NoOverflow:
      return ConditionCode::Overflow;
    case ConditionCode::UnsignedGreater:
      return ConditionCode::UnsignedLessEqual;
    case ConditionCode::UnsignedLessEqual:
      return ConditionCode::UnsignedGreater;
    case ConditionCode::SignedGreaterEqual:
      return ConditionCode::SignedLess;
    case ConditionCode::SignedLess:
      return ConditionCode::SignedGreaterEqual;
    case ConditionCode::SignedGreater:
      return ConditionCode::SignedLessEqual;
    case ConditionCode::SignedLessEqual:
      return ConditionCode::SignedGreater;
    case ConditionCode::Always:
      return ConditionCode::Never;
    case ConditionCode::Never:
      return ConditionCode::Always;
    case ConditionCode::Count:
      break;
  }
  XDEC_UNREACHABLE("invalid condition code");
}

}  // namespace xdec::il
