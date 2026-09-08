// PathState (see the header).
#include "xdec/analysis/path_state.h"

#include <array>
#include <bit>
#include <functional>

#include "xdec/analysis/entry_reg.h"
#include "xdec/support/bits.h"

namespace xdec::analysis {

namespace {

[[nodiscard]] bool widthOk(unsigned width) noexcept { return width > 0 && width <= 64; }

[[nodiscard]] uint64_t maskTo(unsigned width, uint64_t value) noexcept {
  return width >= 64 ? value : value & ((uint64_t{1} << width) - 1);
}

template <class F>
SymValueSet mapSet(const SymValueSet& a, F&& apply, unsigned width) {
  if (a.isTop() || !widthOk(width)) {
    return SymValueSet::top();
  }
  SymValueSet out = SymValueSet::empty();
  for (const uint64_t x : a.values()) {
    out.insert(maskTo(width, apply(x)));
  }
  return out;
}

template <class F>
SymValueSet crossSet(const SymValueSet& a, const SymValueSet& b, F&& apply, unsigned width) {
  if (a.isTop() || b.isTop() || !widthOk(width) ||
      a.values().size() * b.values().size() > SymValueSet::kCap) {
    return SymValueSet::top();
  }
  SymValueSet out = SymValueSet::empty();
  for (const uint64_t x : a.values()) {
    for (const uint64_t y : b.values()) {
      out.insert(maskTo(width, apply(x, y)));
    }
  }
  return out;
}

}  // namespace

unsigned PathState::blockVisits(il::BlockId block) const {
  const auto found = blockVisits_.find(block.index());
  return found == blockVisits_.end() ? 0 : found->second;
}

void PathState::markVisited(il::BlockId block) { ++blockVisits_[block.index()]; }

void PathState::bindPhi(il::ValueId id, il::ExprId incoming) {
  values_[id.index()] = eval(incoming);
}

void PathState::bindPhiUnknown(il::ValueId id) { values_[id.index()] = SymValueSet::top(); }

void PathState::bindLoad(il::ValueId id, il::Type type, il::ExprId address) {
  values_[id.index()] = loadFrom(eval(address), type);
}

void PathState::recordStore(il::ExprId address, il::Type type, il::ExprId value) {
  if (!type.isScalarInteger() || type.bits() > 64) {
    // An effect this path cannot characterise by width at all: safer to
    // forget everything it thought it knew about memory than to keep
    // records a write like this might have clobbered.
    localStore_.clear();
    return;
  }
  const SymValueSet addresses = eval(address);
  const SymValueSet stored = eval(value);
  const unsigned width = type.bits() / 8;
  // A handful of addresses (a select-computed slot, say) can each be
  // recorded; an address this path cannot pin down at all -- top, or an
  // implausibly wide set -- might have written anywhere this path thought it
  // knew about, so every existing record is dropped rather than kept stale.
  if (addresses.isTop() || addresses.values().size() > 4) {
    localStore_.clear();
    return;
  }
  for (const uint64_t at : addresses.values()) {
    localStore_[at] = PathStoreRecord{stored, width};
  }
}

SymValueSet PathState::eval(il::ExprId id) {
  if (const auto found = exprMemo_.find(id.index()); found != exprMemo_.end()) {
    return found->second;
  }
  const il::Expr& expr = function_->expr(id);
  SymValueSet result = SymValueSet::top();
  switch (expr.op) {
    case il::ExprOp::Const:
      result = SymValueSet::one(maskTo(expr.type.bits(), expr.immediate));
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
      break;
    }
  }
  exprMemo_.emplace(id.index(), result);
  return result;
}

SymValueSet PathState::evalValue(il::ValueId id) {
  if (!function_->hasValue(id)) {
    return SymValueSet::top();
  }
  // Bound by bindPhi/bindLoad as PathInterpreter walked the op that defines
  // it, in program order along this exact path. SSA dominance guarantees the
  // defining op was visited before any use reaches here -- except for a
  // value this evaluator does not model at all (a ReadReg the promotion pass
  // left behind, an Intrinsic result), which is the same "top" ImageEval
  // gives an op it does not recognise.
  const auto found = values_.find(id.index());
  return found == values_.end() ? SymValueSet::top() : found->second;
}

SymValueSet PathState::loadFrom(const SymValueSet& addresses, il::Type type) {
  if (addresses.isTop() || !type.isScalarInteger() || type.bits() > 64) {
    return SymValueSet::top();
  }
  const std::size_t width = type.bits() / 8;
  SymValueSet out = SymValueSet::empty();
  for (const uint64_t address : addresses.values()) {
    // This path's own writes outrank everything else: more specific than a
    // platform fact or the image's static bytes, because it is what this
    // exact walk just computed and spilled.
    if (const auto found = localStore_.find(address); found != localStore_.end()) {
      if (found->second.width == width) {
        out.unite(found->second.value);
        continue;
      }
      return SymValueSet::top();
    }
    std::array<std::byte, 8> bytes{};
    if (!reader_(address, std::span<std::byte>(bytes).subspan(0, width))) {
      return SymValueSet::top();  // unmapped memory is not zero
    }
    uint64_t value = 0;
    for (std::size_t index = 0; index < width; ++index) {
      value |= static_cast<uint64_t>(bytes[index]) << (index * 8);
    }
    out.insert(maskTo(type.bits(), value));
  }
  return out;
}

SymValueSet PathState::evalEntryReg(const il::Expr& expr) {
  if (entryRegs_ == nullptr) {
    return SymValueSet::top();
  }
  const il::RegId reg{static_cast<uint32_t>(expr.immediate)};
  if (!function_->registers().contains(reg)) {
    return SymValueSet::top();
  }
  const std::optional<uint64_t> resolved =
      entryRegs_->resolve(function_->registers().nameOf(reg));
  if (!resolved.has_value()) {
    return SymValueSet::top();
  }
  return SymValueSet::one(maskTo(expr.type.bits(), *resolved));
}

SymValueSet PathState::evalUnary(const il::Expr& expr) {
  const unsigned width = expr.type.bits();
  const SymValueSet a = eval(expr.operands[0]);
  switch (expr.op) {
    case il::ExprOp::Not:
      return mapSet(a, [](uint64_t x) { return ~x; }, width);
    case il::ExprOp::Neg:
      return mapSet(a, [](uint64_t x) { return ~x + 1; }, width);
    case il::ExprOp::Clz:
      return mapSet(a,
                    [width](uint64_t x) {
                      return x == 0 ? width
                                    : static_cast<unsigned>(
                                          std::countl_zero(maskTo(width, x))) -
                                          (64 - width);
                    },
                    width);
    default:
      return SymValueSet::top();
  }
}

SymValueSet PathState::evalBinary(const il::Expr& expr) {
  const unsigned width = expr.type.bits();
  const SymValueSet a = eval(expr.operands[0]);
  const SymValueSet b = eval(expr.operands[1]);
  switch (expr.op) {
    case il::ExprOp::Add: return crossSet(a, b, std::plus<>{}, width);
    case il::ExprOp::Sub: return crossSet(a, b, std::minus<>{}, width);
    case il::ExprOp::Mul: return crossSet(a, b, std::multiplies<>{}, width);
    case il::ExprOp::And: return crossSet(a, b, std::bit_and<>{}, width);
    case il::ExprOp::Or: return crossSet(a, b, std::bit_or<>{}, width);
    case il::ExprOp::Xor: return crossSet(a, b, std::bit_xor<>{}, width);
    case il::ExprOp::Shl:
      return crossSet(a, b, [width](uint64_t x, uint64_t y) {
        return y >= width ? uint64_t{0} : x << y;
      }, width);
    case il::ExprOp::ShrU:
      return crossSet(a, b, [width](uint64_t x, uint64_t y) {
        return y >= width ? uint64_t{0} : maskTo(width, x) >> y;
      }, width);
    case il::ExprOp::ShrS:
      return crossSet(a, b, [width](uint64_t x, uint64_t y) {
        const int64_t sx = static_cast<int64_t>(signExtend(x, width));
        return y >= width ? (sx < 0 ? ~uint64_t{0} : uint64_t{0})
                          : static_cast<uint64_t>(sx >> y);
      }, width);
    case il::ExprOp::RotR:
      return crossSet(a, b, [width](uint64_t x, uint64_t y) {
        return rotateRight(x, width, static_cast<unsigned>(y));
      }, width);
    case il::ExprOp::RotL:
      return crossSet(a, b, [width](uint64_t x, uint64_t y) {
        return rotateLeft(x, width, static_cast<unsigned>(y));
      }, width);
    case il::ExprOp::CmpEq:
      return crossSet(a, b, [](uint64_t x, uint64_t y) { return uint64_t{x == y}; }, 1);
    case il::ExprOp::CmpNe:
      return crossSet(a, b, [](uint64_t x, uint64_t y) { return uint64_t{x != y}; }, 1);
    case il::ExprOp::CmpLtU:
      return crossSet(a, b, [](uint64_t x, uint64_t y) { return uint64_t{x < y}; }, 1);
    case il::ExprOp::CmpLeU:
      return crossSet(a, b, [](uint64_t x, uint64_t y) { return uint64_t{x <= y}; }, 1);
    case il::ExprOp::CmpLtS:
      return crossSet(a, b, [width](uint64_t x, uint64_t y) {
        return uint64_t{static_cast<int64_t>(signExtend(x, width)) <
                        static_cast<int64_t>(signExtend(y, width))};
      }, 1);
    case il::ExprOp::CmpLeS:
      return crossSet(a, b, [width](uint64_t x, uint64_t y) {
        return uint64_t{static_cast<int64_t>(signExtend(x, width)) <=
                        static_cast<int64_t>(signExtend(y, width))};
      }, 1);
    default:
      return SymValueSet::top();
  }
}

SymValueSet PathState::evalSelect(const il::Expr& expr) {
  const SymValueSet condition = eval(expr.operands[0]);
  if (!condition.isTop() && condition.values().size() == 1) {
    return eval(expr.operands[condition.values()[0] != 0 ? 1 : 2]);
  }
  SymValueSet result = eval(expr.operands[1]);
  result.unite(eval(expr.operands[2]));
  return result;
}

SymValueSet PathState::evalCast(const il::Expr& expr) {
  const SymValueSet source = eval(expr.operands[0]);
  const unsigned toWidth = expr.type.bits();
  switch (expr.op) {
    case il::ExprOp::ZExt:
      return mapSet(source, [](uint64_t x) { return x; }, toWidth);
    case il::ExprOp::SExt: {
      const unsigned fromWidth = function_->expr(expr.operands[0]).type.bits();
      return mapSet(source, [fromWidth](uint64_t x) {
        return static_cast<uint64_t>(signExtend(x, fromWidth));
      }, toWidth);
    }
    case il::ExprOp::Trunc:
      return mapSet(source, [](uint64_t x) { return x; }, toWidth);
    case il::ExprOp::Extract: {
      const unsigned lo = static_cast<unsigned>(expr.immediate);
      return mapSet(source, [lo, toWidth](uint64_t x) {
        return extractBits(x, lo, toWidth);
      }, toWidth);
    }
    default:
      return SymValueSet::top();
  }
}

}  // namespace xdec::analysis
