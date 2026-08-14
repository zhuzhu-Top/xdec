// ImageEval: undef-tolerant value-set evaluation (see the header).
#include "xdec/analysis/image_eval.h"

#include <array>
#include <bit>
#include <functional>

#include "xdec/support/bits.h"

namespace xdec::analysis {

namespace {

/// The width mask shared by every arithmetic step; widths above 64 are
/// outside this evaluator's world (its answers feed 64-bit addresses).
[[nodiscard]] bool widthOk(unsigned width) noexcept { return width > 0 && width <= 64; }

[[nodiscard]] uint64_t maskTo(unsigned width, uint64_t value) noexcept {
  return width >= 64 ? value : value & ((uint64_t{1} << width) - 1);
}

}  // namespace

void ValueSet::insert(uint64_t value) {
  if (top_) {
    return;
  }
  for (const uint64_t existing : values_) {
    if (existing == value) {
      return;
    }
  }
  if (values_.size() >= kCap) {
    top_ = true;
    values_.clear();
    return;
  }
  values_.push_back(value);
}

void ValueSet::unite(const ValueSet& other) {
  if (top_) {
    return;
  }
  if (other.top_) {
    top_ = true;
    values_.clear();
    return;
  }
  for (const uint64_t value : other.values_) {
    insert(value);
  }
}

template <class F>
ValueSet ImageEval::map(const ValueSet& a, F&& apply, unsigned width) {
  if (a.isTop() || !widthOk(width)) {
    return ValueSet::top();
  }
  ValueSet out = ValueSet::empty();
  for (const uint64_t x : a.values()) {
    out.insert(maskTo(width, apply(x)));
  }
  return out;
}

template <class F>
ValueSet ImageEval::cross(const ValueSet& a, const ValueSet& b, F&& apply,
                          unsigned width) {
  if (a.isTop() || b.isTop() || !widthOk(width) ||
      a.values().size() * b.values().size() > ValueSet::kCap) {
    return ValueSet::top();
  }
  ValueSet out = ValueSet::empty();
  for (const uint64_t x : a.values()) {
    for (const uint64_t y : b.values()) {
      out.insert(maskTo(width, apply(x, y)));
    }
  }
  return out;
}

ValueSet ImageEval::eval(il::ExprId id) {
  if (const auto found = memo_.find(id); found != memo_.end()) {
    return found->second;
  }
  if (active_.contains(id)) {
    // A phi loop re-entering its own evaluation: the cyclic edge carries no
    // information, so it contributes the empty set (union's identity), not
    // top — phi(0x77, phi) has values {0x77}, not "anything".
    return ValueSet::empty();
  }
  // Deep substituted chains outgrow the call stack; past the bound the
  // answer is top, never an overflow.
  if (++depth_ > kMaxDepth) {
    --depth_;
    return ValueSet::top();
  }
  active_.emplace(id, true);

  const il::Expr& expr = function_.expr(id);
  ValueSet result = ValueSet::top();
  switch (expr.op) {
    case il::ExprOp::Const:
      result = ValueSet::one(maskTo(expr.type.bits(), expr.immediate));
      break;
    case il::ExprOp::Value:
      result = evalValue(il::ValueId{static_cast<uint32_t>(expr.immediate)});
      break;
    case il::ExprOp::Select:
      result = evalSelect(expr);
      break;
    case il::ExprOp::ZExt:
    case il::ExprOp::SExt:
    case il::ExprOp::Trunc:
    case il::ExprOp::Extract:
      result = evalCast(expr);
      break;
    case il::ExprOp::EntryReg:
      result = evalEntryReg(expr);
      break;
    default: {
      const il::ExprOpInfo& info = il::info(expr.op);
      if (info.category == il::ExprCategory::IntArithmetic ||
          info.category == il::ExprCategory::IntBitwise ||
          info.category == il::ExprCategory::IntShift ||
          info.category == il::ExprCategory::IntCompare) {
        result = expr.operandCount == 1 ? evalUnary(expr) : evalBinary(expr);
      }
      // Flags, flag conditions, and everything else stay top.
      break;
    }
  }

  active_.erase(id);
  --depth_;
  memo_.emplace(id, result);
  return result;
}

ValueSet ImageEval::evalValue(il::ValueId id) {
  // DCE tombstones definitions and leaves the uses in the pool; a use of such
  // a value is unknowable here, not a crash.
  if (!function_.hasValue(id)) {
    return ValueSet::top();
  }
  if (const auto found = valueMemo_.find(id); found != valueMemo_.end()) {
    return found->second;
  }
  const il::ValueInfo& info = function_.value(id);
  if (!function_.hasOp(info.definition)) {
    return ValueSet::top();
  }
  const il::Op& definition = function_.op(info.definition);
  ValueSet result = ValueSet::top();
  if (definition.code == il::OpCode::Phi) {
    result = unionEntryRegAware(function_.operands(definition));
  } else if (definition.code == il::OpCode::Load) {
    const auto operands = function_.operands(definition);
    result = loadFrom(eval(operands[0]), function_.value(id).type);
  }
  // Call results, register reads, and anything else: top.
  valueMemo_.emplace(id, result);
  return result;
}

ValueSet ImageEval::loadFrom(const ValueSet& addresses, il::Type type) {
  if (addresses.isTop() || !type.isScalarInteger() || type.bits() > 64) {
    return ValueSet::top();
  }
  const std::size_t width = type.bits() / 8;
  ValueSet out = ValueSet::empty();
  for (const uint64_t address : addresses.values()) {
    if (entryRegs_ != nullptr) {
      // A captured fact about memory the image itself does not contain (an
      // argument pointer's pointee, say -- see MemorySeed) always wins over
      // the image's own bytes at that address, the same priority an
      // EntryReg literal has over a platform formula.
      if (const std::optional<uint64_t> seeded =
              entryRegs_->memoryValueAt(address, static_cast<unsigned>(width))) {
        out.insert(maskTo(type.bits(), *seeded));
        continue;
      }
    }
    std::array<std::byte, 8> bytes{};
    if (!reader_(address, std::span<std::byte>(bytes).subspan(0, width))) {
      return ValueSet::top();  // unmapped memory is not zero
    }
    uint64_t value = 0;
    for (std::size_t index = 0; index < width; ++index) {
      value |= static_cast<uint64_t>(bytes[index]) << (index * 8);
    }
    out.insert(maskTo(type.bits(), value));
  }
  return out;
}

bool ImageEval::isRawEntryReg(il::ExprId id) const {
  return function_.expr(id).op == il::ExprOp::EntryReg;
}

ValueSet ImageEval::unionEntryRegAware(std::span<const il::ExprId> arms) {
  bool anyComputed = false;
  for (const il::ExprId arm : arms) {
    if (!isRawEntryReg(arm)) {
      anyComputed = true;
      break;
    }
  }
  ValueSet result = ValueSet::empty();
  for (const il::ExprId arm : arms) {
    if (anyComputed && isRawEntryReg(arm)) {
      continue;
    }
    result.unite(eval(arm));
  }
  return result;
}

ValueSet ImageEval::evalEntryReg(const il::Expr& expr) {
  if (entryRegs_ == nullptr) {
    return ValueSet::top();
  }
  const il::RegId reg{static_cast<uint32_t>(expr.immediate)};
  if (!function_.registers().contains(reg)) {
    return ValueSet::top();
  }
  const std::optional<uint64_t> resolved =
      entryRegs_->resolve(function_.registers().nameOf(reg));
  if (!resolved.has_value()) {
    return ValueSet::top();
  }
  return ValueSet::one(maskTo(expr.type.bits(), *resolved));
}

ValueSet ImageEval::evalUnary(const il::Expr& expr) {
  const unsigned width = expr.type.bits();
  const ValueSet a = eval(expr.operands[0]);
  switch (expr.op) {
    case il::ExprOp::Not:
      return map(a, [](uint64_t x) { return ~x; }, width);
    case il::ExprOp::Neg:
      return map(a, [](uint64_t x) { return ~x + 1; }, width);
    case il::ExprOp::Clz: {
      return map(a,
                 [width](uint64_t x) {
                   return x == 0 ? width
                                 : static_cast<unsigned>(std::countl_zero(
                                       maskTo(width, x))) -
                                       (64 - width);
                 },
                 width);
    }
    default:
      return ValueSet::top();
  }
}

ValueSet ImageEval::evalBinary(const il::Expr& expr) {
  const unsigned width = expr.type.bits();
  const ValueSet a = eval(expr.operands[0]);
  const ValueSet b = eval(expr.operands[1]);
  switch (expr.op) {
    case il::ExprOp::Add: return cross(a, b, std::plus<>{}, width);
    case il::ExprOp::Sub: return cross(a, b, std::minus<>{}, width);
    case il::ExprOp::Mul: return cross(a, b, std::multiplies<>{}, width);
    case il::ExprOp::And: return cross(a, b, std::bit_and<>{}, width);
    case il::ExprOp::Or: return cross(a, b, std::bit_or<>{}, width);
    case il::ExprOp::Xor: return cross(a, b, std::bit_xor<>{}, width);
    case il::ExprOp::Shl:
      return cross(a, b, [width](uint64_t x, uint64_t y) {
        return y >= width ? uint64_t{0} : x << y;
      }, width);
    case il::ExprOp::ShrU:
      return cross(a, b, [width](uint64_t x, uint64_t y) {
        return y >= width ? uint64_t{0} : maskTo(width, x) >> y;
      }, width);
    case il::ExprOp::ShrS:
      return cross(a, b, [width](uint64_t x, uint64_t y) {
        const int64_t sx = static_cast<int64_t>(signExtend(x, width));
        return y >= width ? (sx < 0 ? ~uint64_t{0} : uint64_t{0})
                          : static_cast<uint64_t>(sx >> y);
      }, width);
    case il::ExprOp::RotR:
      return cross(a, b, [width](uint64_t x, uint64_t y) {
        return rotateRight(x, width, static_cast<unsigned>(y));
      }, width);
    case il::ExprOp::RotL:
      return cross(a, b, [width](uint64_t x, uint64_t y) {
        return rotateLeft(x, width, static_cast<unsigned>(y));
      }, width);
    case il::ExprOp::CmpEq:
      return cross(a, b, [](uint64_t x, uint64_t y) { return uint64_t{x == y}; }, 1);
    case il::ExprOp::CmpNe:
      return cross(a, b, [](uint64_t x, uint64_t y) { return uint64_t{x != y}; }, 1);
    case il::ExprOp::CmpLtU:
      return cross(a, b, [](uint64_t x, uint64_t y) { return uint64_t{x < y}; }, 1);
    case il::ExprOp::CmpLeU:
      return cross(a, b, [](uint64_t x, uint64_t y) { return uint64_t{x <= y}; }, 1);
    case il::ExprOp::CmpLtS:
      return cross(a, b, [width](uint64_t x, uint64_t y) {
        return uint64_t{static_cast<int64_t>(signExtend(x, width)) <
                        static_cast<int64_t>(signExtend(y, width))};
      }, 1);
    case il::ExprOp::CmpLeS:
      return cross(a, b, [width](uint64_t x, uint64_t y) {
        return uint64_t{static_cast<int64_t>(signExtend(x, width)) <=
                        static_cast<int64_t>(signExtend(y, width))};
      }, 1);
    default:
      return ValueSet::top();
  }
}

ValueSet ImageEval::evalSelect(const il::Expr& expr) {
  const ValueSet condition = eval(expr.operands[0]);
  if (!condition.isTop() && condition.values().size() == 1) {
    return eval(expr.operands[condition.values()[0] != 0 ? 1 : 2]);
  }
  // The case this evaluator exists for: both arms, unioned -- except a bare
  // EntryReg arm next to a computed one (see unionEntryRegAware).
  return unionEntryRegAware(std::span<const il::ExprId>(&expr.operands[1], 2));
}

ValueSet ImageEval::evalCast(const il::Expr& expr) {
  const ValueSet source = eval(expr.operands[0]);
  const unsigned toWidth = expr.type.bits();
  switch (expr.op) {
    case il::ExprOp::ZExt:
      return map(source, [](uint64_t x) { return x; }, toWidth);
    case il::ExprOp::SExt: {
      const unsigned fromWidth = function_.expr(expr.operands[0]).type.bits();
      return map(source, [fromWidth](uint64_t x) {
        return static_cast<uint64_t>(signExtend(x, fromWidth));
      }, toWidth);
    }
    case il::ExprOp::Trunc:
      return map(source, [](uint64_t x) { return x; }, toWidth);
    case il::ExprOp::Extract: {
      const unsigned lo = static_cast<unsigned>(expr.immediate);
      return map(source, [lo, toWidth](uint64_t x) {
        return extractBits(x, lo, toWidth);
      }, toWidth);
    }
    default:
      return ValueSet::top();
  }
}

}  // namespace xdec::analysis
