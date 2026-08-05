// Concrete execution of the IL. See interp.h for the design stance.
#include "xdec/il/interp.h"

#include "u128.h"
#include "xdec/il/ceval.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>

#include "xdec/support/bits.h"

namespace xdec::il {
namespace {

}  // namespace

// ---------------------------------------------------------------------------
// ExecMemory
// ---------------------------------------------------------------------------

void ExecMemory::seed(uint64_t address, std::span<const std::byte> bytes) {
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    const uint64_t va = address + index;
    seed_[va & ~(kPageSize - 1)].bytes[va & (kPageSize - 1)] = bytes[index];
  }
}

void ExecMemory::fillDelta(uint64_t address, std::span<const std::byte> bytes) {
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    const uint64_t va = address + index;
    deltaPage(va & ~(kPageSize - 1))->bytes[va & (kPageSize - 1)] = bytes[index];
  }
}

bool ExecMemory::mapped(uint64_t address, uint64_t size) const {
  if (size == 0) {
    return true;
  }
  uint64_t page = address & ~(kPageSize - 1);
  const uint64_t last = (address + size - 1) & ~(kPageSize - 1);
  while (true) {
    if (pageAt(page) == nullptr) {
      return false;
    }
    if (page == last) {
      return true;
    }
    page += kPageSize;
  }
}

Result<ConcreteValue> ExecMemory::read(uint64_t address, unsigned bytes) const {
  if (bytes == 0 || bytes > 16) {
    return err(DiagCode::Internal, std::format("memory read of {} bytes", bytes));
  }
  if (!mapped(address, bytes)) {
    return err(Diag{DiagCode::UnmappedAddress,
                    std::format("read of {} unmapped bytes", bytes)}
                   .at(address));
  }
  ConcreteValue value{};
  for (unsigned index = 0; index < bytes; ++index) {
    const uint64_t va = address + index;
    const Page* page = pageAt(va & ~(kPageSize - 1));
    const auto byte = static_cast<uint64_t>(page->bytes[va & (kPageSize - 1)]);
    if (index < 8) {
      value.lo |= byte << (index * 8);
    } else {
      value.hi |= byte << ((index - 8) * 8);
    }
  }
  return value;
}

Result<void> ExecMemory::write(uint64_t address, unsigned bytes, ConcreteValue value) {
  if (bytes == 0 || bytes > 16) {
    return err(DiagCode::Internal, std::format("memory write of {} bytes", bytes));
  }
  if (!mapped(address, bytes)) {
    return err(Diag{DiagCode::UnmappedAddress,
                    std::format("write of {} unmapped bytes", bytes)}
                   .at(address));
  }
  for (unsigned index = 0; index < bytes; ++index) {
    const uint64_t va = address + index;
    const uint64_t byte = index < 8 ? (value.lo >> (index * 8)) & 0xFF
                                    : (value.hi >> ((index - 8) * 8)) & 0xFF;
    deltaPage(va & ~(kPageSize - 1))->bytes[va & (kPageSize - 1)] =
        static_cast<std::byte>(byte);
    writtenBytes_.push_back(va);
  }
  return ok();
}

std::vector<WrittenRange> ExecMemory::writtenRanges() const {
  if (writtenBytes_.empty()) {
    return {};
  }
  std::vector<uint64_t> sorted = writtenBytes_;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());

  std::vector<WrittenRange> ranges;
  uint64_t start = sorted.front();
  uint64_t previous = sorted.front();
  for (const uint64_t va : sorted) {
    if (va != previous && va != previous + 1) {
      ranges.push_back(WrittenRange{start, previous - start + 1});
      start = va;
    }
    previous = va;
  }
  ranges.push_back(WrittenRange{start, previous - start + 1});
  return ranges;
}

void ExecMemory::clearDelta() {
  delta_.clear();
  writtenBytes_.clear();
}

const ExecMemory::Page* ExecMemory::pageAt(uint64_t pageBase) const {
  if (const auto it = delta_.find(pageBase); it != delta_.end()) {
    return &it->second;
  }
  if (const auto it = seed_.find(pageBase); it != seed_.end()) {
    return &it->second;
  }
  return nullptr;
}

ExecMemory::Page* ExecMemory::deltaPage(uint64_t pageBase) {
  auto [it, inserted] = delta_.try_emplace(pageBase);
  if (inserted) {
    if (const auto seeded = seed_.find(pageBase); seeded != seed_.end()) {
      it->second = seeded->second;
    }
  }
  return &it->second;
}

// ---------------------------------------------------------------------------
// Interpreter
// ---------------------------------------------------------------------------

std::string_view toString(ExecStop stop) noexcept {
  switch (stop) {
    case ExecStop::Branch: return "branch";
    case ExecStop::CondBranch: return "cond-branch";
    case ExecStop::IndirectBranch: return "indirect-branch";
    case ExecStop::Call: return "call";
    case ExecStop::Return: return "return";
    case ExecStop::Unreachable: return "unreachable";
    case ExecStop::Unimplemented: return "unimplemented";
    case ExecStop::Intrinsic: return "intrinsic";
    case ExecStop::Error: return "error";
  }
  return "?";
}

Interpreter::Interpreter(const Function& function, ExecMemory* memory)
    : function_(&function), memory_(memory) {
  if (memory_ == nullptr) {
    ownedMemory_ = std::make_unique<ExecMemory>();
    memory_ = ownedMemory_.get();
  }
  resetState();
}

void Interpreter::resetState() {
  registers_.assign(function_->registers().size(), ConcreteValue{});
  values_.assign(function_->valueCount(), ConcreteValue{});
  defined_.assign(function_->valueCount(), false);
}

void Interpreter::writeRegister(RegId reg, ConcreteValue value) {
  const RegisterFile& file = function_->registers();
  const RegisterInfo& info = file[reg];
  if (info.regClass == RegClass::Zero) {
    return;
  }
  if (info.regClass == RegClass::Flags) {
    registers_[reg.asSize()] = ConcreteValue{value.lo & 0xF, 0};
    return;
  }
  const RegId root = file.rootOf(reg);
  if (root == reg) {
    const U128 whole = mask(U128{value.lo, value.hi}, info.bits);
    registers_[root.asSize()] = ConcreteValue{whole.lo, whole.hi};
    return;
  }
  U128 current{value.lo, value.hi};
  U128 parent{registers_[root.asSize()].lo, registers_[root.asSize()].hi};
  for (RegId level = reg; level != root; level = file[level].parent) {
    const RegisterInfo& view = file[level];
    const unsigned parentWidth = file[view.parent].bits;
    if (view.zeroExtendsParent) {
      // Writing the view zeroes the rest of the parent; whatever was there is
      // gone by definition, and the walk continues from a clean value.
      parent = zext(current, view.bits, parentWidth);
    } else {
      parent = insert(parent, current, view.offsetInParent, view.bits, parentWidth);
    }
    current = parent;
  }
  const U128 masked = mask(parent, file[root].bits);
  registers_[root.asSize()] = ConcreteValue{masked.lo, masked.hi};
}

ConcreteValue Interpreter::readRegister(RegId reg) const {
  const RegisterFile& file = function_->registers();
  const RegisterInfo& info = file[reg];
  if (info.regClass == RegClass::Zero) {
    return ConcreteValue{};
  }
  if (info.regClass == RegClass::Flags) {
    return ConcreteValue{registers_[reg.asSize()].lo & 0xF, 0};
  }
  const RegId root = file.rootOf(reg);
  const ConcreteValue& cell = registers_[root.asSize()];
  unsigned offset = 0;
  for (RegId level = reg; level != root; level = file[level].parent) {
    offset += file[level].offsetInParent;
  }
  const U128 extracted =
      extract(U128{cell.lo, cell.hi}, file[root].bits, offset, info.bits);
  return ConcreteValue{extracted.lo, extracted.hi};
}

ExecOutcome Interpreter::fail(std::string message, uint64_t va) {
  ExecOutcome outcome;
  outcome.stop = ExecStop::Error;
  outcome.va = va;
  outcome.detail = std::move(message);
  return outcome;
}

Result<ConcreteValue> Interpreter::evalFlagsOperand(ExprId id, unsigned depth) {
  const Expr& expr = function_->expr(id);
  if (expr.op == ExprOp::FlagDef) {
    ConcreteValue args[3];
    for (unsigned index = 0; index < expr.operandCount; ++index) {
      XDEC_TRY(args[index], eval(expr.operands[index], depth + 1));
    }
    return ConcreteValue{evalFlagDef(flagDefOp(expr.immediate),
                                   flagDefWidth(expr.immediate),
                                   std::span<const ConcreteValue>{args, expr.operandCount}),
                         0};
  }
  XDEC_TRY(const ConcreteValue value, eval(id, depth + 1));
  return ConcreteValue{value.lo & 0xF, 0};
}

Result<ConcreteValue> Interpreter::eval(ExprId id, unsigned depth) {
  if (depth > 512) {
    return err(DiagCode::AnalysisLimit, "expression nesting too deep");
  }
  const Expr& expr = function_->expr(id);
  const Type type = expr.type;
  const unsigned width = type.bits();
  if (width > 128 && !type.isFlags()) {
    return err(DiagCode::Internal,
               std::format("interpreter supports widths to 128, not {}", width));
  }

  ConcreteValue args[3];
  for (unsigned index = 0; index < expr.operandCount; ++index) {
    if (expr.op == ExprOp::FlagCond || expr.op == ExprOp::FlagBit) {
      XDEC_TRY(args[index], evalFlagsOperand(expr.operands[index], depth + 1));
    } else {
      XDEC_TRY(args[index], eval(expr.operands[index], depth + 1));
    }
  }
  const U128 a{args[0].lo, args[0].hi};
  const U128 b{args[1].lo, args[1].hi};
  const auto pack = [](U128 v) { return ConcreteValue{v.lo, v.hi}; };
  const auto boolean = [](bool value) {
    return ConcreteValue{value ? uint64_t{1} : uint64_t{0}, 0};
  };

  switch (expr.op) {
    case ExprOp::Const:
      return pack(mask(U128{expr.immediate, 0}, width));
    case ExprOp::Value: {
      const ValueId value{static_cast<uint32_t>(expr.immediate)};
      if (!function_->hasValue(value) || !defined_[value.asSize()]) {
        return err(DiagCode::VerifyFailure, "value used before its definition");
      }
      return values_[value.asSize()];
    }
    case ExprOp::Undef:
      return err(DiagCode::LiftFailure, "an undef value was evaluated");
    case ExprOp::EntryReg:
      // The register state the block was seeded with is the function-entry
      // value as far as this execution knows.
      return readRegister(RegId{static_cast<uint32_t>(expr.immediate)});

    case ExprOp::Add: { bool carry = false; return pack(add(a, b, width, carry)); }
    case ExprOp::Sub:
      return pack(sub(a, b, width));
    case ExprOp::Mul:
      return pack(mulLow(a, b, width));
    case ExprOp::MulHiU: {
      if (width != 64) {
        return err(DiagCode::NotImplemented, "mulhi.u is implemented at width 64");
      }
      const auto [hi, lo] = mul64x64(a.lo, b.lo);
      (void)lo;
      return ConcreteValue{hi, 0};
    }
    case ExprOp::MulHiS: {
      if (width != 64) {
        return err(DiagCode::NotImplemented, "mulhi.s is implemented at width 64");
      }
      auto [hi, lo] = mul64x64(a.lo, b.lo);
      (void)lo;
      // Signed correction: subtract a if b is negative, b if a is.
      if ((a.lo >> 63) != 0) {
        hi -= b.lo;
      }
      if ((b.lo >> 63) != 0) {
        hi -= a.lo;
      }
      return ConcreteValue{hi, 0};
    }
    case ExprOp::DivU: {
      if (width > 64) {
        return err(DiagCode::NotImplemented, "div.u above 64 bits");
      }
      if (b.lo == 0) {
        return ConcreteValue{};  // AArch64: no trap, result is zero
      }
      return pack(mask(U128{a.lo / b.lo, 0}, width));
    }
    case ExprOp::DivS: {
      if (width > 64) {
        return err(DiagCode::NotImplemented, "div.s above 64 bits");
      }
      const int64_t dividend = static_cast<int64_t>(sext(a, width, 64).lo);
      const int64_t divisor = static_cast<int64_t>(sext(b, width, 64).lo);
      if (divisor == 0) {
        return ConcreteValue{};
      }
      // INT_MIN / -1 wraps rather than trapping, which is the ARM result too.
      const int64_t quotient = divisor == -1 ? -dividend : dividend / divisor;
      return pack(mask(U128{static_cast<uint64_t>(quotient), 0}, width));
    }
    case ExprOp::RemU: {
      if (width > 64) {
        return err(DiagCode::NotImplemented, "rem.u above 64 bits");
      }
      if (b.lo == 0) {
        return pack(mask(a, width));  // quotient is zero, so the remainder is a
      }
      return pack(mask(U128{a.lo % b.lo, 0}, width));
    }
    case ExprOp::RemS: {
      if (width > 64) {
        return err(DiagCode::NotImplemented, "rem.s above 64 bits");
      }
      const int64_t dividend = static_cast<int64_t>(sext(a, width, 64).lo);
      const int64_t divisor = static_cast<int64_t>(sext(b, width, 64).lo);
      if (divisor == 0) {
        return pack(mask(a, width));
      }
      const int64_t remainder = divisor == -1 ? 0 : dividend % divisor;
      return pack(mask(U128{static_cast<uint64_t>(remainder), 0}, width));
    }
    case ExprOp::Neg:
      return pack(sub(U128{}, a, width));

    case ExprOp::And:
      return pack(mask(U128{a.lo & b.lo, a.hi & b.hi}, width));
    case ExprOp::Or:
      return pack(mask(U128{a.lo | b.lo, a.hi | b.hi}, width));
    case ExprOp::Xor:
      return pack(mask(U128{a.lo ^ b.lo, a.hi ^ b.hi}, width));
    case ExprOp::Not: {
      const U128 m = maskOf(width);
      return ConcreteValue{a.lo ^ m.lo, a.hi ^ m.hi};
    }

    case ExprOp::Shl:
      return pack(shl(a, width, static_cast<unsigned>(b.lo)));
    case ExprOp::ShrU:
      return pack(shrU(a, width, static_cast<unsigned>(b.lo)));
    case ExprOp::ShrS:
      return pack(shrS(a, width, static_cast<unsigned>(b.lo)));
    case ExprOp::RotR:
      return pack(rotR(a, width, static_cast<unsigned>(b.lo)));
    case ExprOp::RotL:
      return pack(rotL(a, width, static_cast<unsigned>(b.lo)));

    case ExprOp::CmpEq:
      return boolean(a.lo == b.lo && a.hi == b.hi);
    case ExprOp::CmpNe:
      return boolean(a.lo != b.lo || a.hi != b.hi);
    case ExprOp::CmpLtU:
      return boolean(cmpU(a, b) < 0);
    case ExprOp::CmpLeU:
      return boolean(cmpU(a, b) <= 0);
    case ExprOp::CmpLtS:
      return boolean(
          cmpS(a, b, function_->expr(expr.operands[0]).type.bits()) < 0);
    case ExprOp::CmpLeS:
      return boolean(
          cmpS(a, b, function_->expr(expr.operands[0]).type.bits()) <= 0);

    case ExprOp::ZExt:
      return pack(zext(a, function_->expr(expr.operands[0]).type.bits(), width));
    case ExprOp::SExt:
      return pack(sext(a, function_->expr(expr.operands[0]).type.bits(), width));
    case ExprOp::Trunc:
      return pack(mask(a, width));
    case ExprOp::Bitcast:
      return pack(mask(a, width));
    case ExprOp::Extract: {
      const unsigned sourceWidth = function_->expr(expr.operands[0]).type.bits();
      return pack(extract(a, sourceWidth, static_cast<unsigned>(expr.immediate), width));
    }
    case ExprOp::Concat: {
      const unsigned lowWidth = function_->expr(expr.operands[1]).type.bits();
      const U128 high = shl(a, width, lowWidth);
      const U128 low = mask(b, lowWidth);
      return pack(mask(U128{high.lo | low.lo, high.hi | low.hi}, width));
    }

    case ExprOp::Clz:
      return ConcreteValue{clzOf(a, width), 0};
    case ExprOp::Ctz:
      return ConcreteValue{ctzOf(a, width), 0};
    case ExprOp::PopCount: {
      const U128 v = mask(a, width);
      return ConcreteValue{
          static_cast<uint64_t>(std::popcount(v.lo) + std::popcount(v.hi)), 0};
    }
    case ExprOp::ByteSwap:
      return pack(bswapOf(a, width));
    case ExprOp::BitReverse:
      return pack(brevOf(a, width));

    case ExprOp::Select:
      return (args[0].lo & 1) != 0 ? args[1] : args[2];

    case ExprOp::FlagDef:
      return ConcreteValue{evalFlagDef(flagDefOp(expr.immediate),
                                       flagDefWidth(expr.immediate),
                                       std::span<const ConcreteValue>{args, expr.operandCount}),
                           0};
    case ExprOp::FlagCond:
      return boolean(
          evalCondition(static_cast<ConditionCode>(expr.immediate),
                        static_cast<uint8_t>(args[0].lo & 0xF)));
    case ExprOp::FlagBit: {
      const auto bit = static_cast<FlagBitIndex>(expr.immediate);
      const unsigned position = bit == FlagBitIndex::Negative  ? 3
                                : bit == FlagBitIndex::Zero    ? 2
                                : bit == FlagBitIndex::Carry   ? 1
                                                               : 0;
      return boolean(((args[0].lo >> position) & 1) != 0);
    }

    case ExprOp::FAdd:
    case ExprOp::FSub:
    case ExprOp::FMul:
    case ExprOp::FDiv:
    case ExprOp::FSqrt: {
      if (width == 32) {
        const float x = std::bit_cast<float>(static_cast<uint32_t>(args[0].lo));
        const float y = std::bit_cast<float>(static_cast<uint32_t>(args[1].lo));
        float r = 0.0F;
        if (expr.op == ExprOp::FAdd) r = x + y;
        else if (expr.op == ExprOp::FSub) r = x - y;
        else if (expr.op == ExprOp::FMul) r = x * y;
        else if (expr.op == ExprOp::FDiv) r = x / y;
        else r = std::sqrt(x);
        return ConcreteValue{std::bit_cast<uint32_t>(r), 0};
      }
      const double x = std::bit_cast<double>(args[0].lo);
      const double y = std::bit_cast<double>(args[1].lo);
      double r = 0.0;
      if (expr.op == ExprOp::FAdd) r = x + y;
      else if (expr.op == ExprOp::FSub) r = x - y;
      else if (expr.op == ExprOp::FMul) r = x * y;
      else if (expr.op == ExprOp::FDiv) r = x / y;
      else r = std::sqrt(x);
      return ConcreteValue{std::bit_cast<uint64_t>(r), 0};
    }
    case ExprOp::FNeg:
      // Sign flip is exact even on NaN, which a subtract would canonicalise.
      if (width == 32) {
        return ConcreteValue{args[0].lo ^ 0x80000000ull, 0};
      }
      return ConcreteValue{args[0].lo ^ (uint64_t{1} << 63), 0};
    case ExprOp::FAbs:
      if (width == 32) {
        return ConcreteValue{args[0].lo & 0x7FFFFFFFull, 0};
      }
      return ConcreteValue{args[0].lo & ~(uint64_t{1} << 63), 0};

    case ExprOp::FCmpEq:
    case ExprOp::FCmpLt:
    case ExprOp::FCmpLe:
    case ExprOp::FCmpUnordered: {
      const double x = width == 32
                           ? static_cast<double>(std::bit_cast<float>(
                                 static_cast<uint32_t>(args[0].lo)))
                           : std::bit_cast<double>(args[0].lo);
      const double y = width == 32
                           ? static_cast<double>(std::bit_cast<float>(
                                 static_cast<uint32_t>(args[1].lo)))
                           : std::bit_cast<double>(args[1].lo);
      if (std::isnan(x) || std::isnan(y)) {
        return boolean(expr.op == ExprOp::FCmpUnordered);
      }
      if (expr.op == ExprOp::FCmpEq) return boolean(x == y);
      if (expr.op == ExprOp::FCmpLt) return boolean(x < y);
      if (expr.op == ExprOp::FCmpLe) return boolean(x <= y);
      return boolean(false);
    }

    case ExprOp::FpConvert: {
      const unsigned fromWidth = function_->expr(expr.operands[0]).type.bits();
      const double value = fromWidth == 32
                               ? static_cast<double>(std::bit_cast<float>(
                                     static_cast<uint32_t>(args[0].lo)))
                               : std::bit_cast<double>(args[0].lo);
      if (width == 32) {
        return ConcreteValue{std::bit_cast<uint32_t>(static_cast<float>(value)), 0};
      }
      return ConcreteValue{std::bit_cast<uint64_t>(value), 0};
    }
    case ExprOp::FpToIntS:
    case ExprOp::FpToIntU: {
      const unsigned fromWidth = function_->expr(expr.operands[0]).type.bits();
      const double x = fromWidth == 32
                           ? static_cast<double>(std::bit_cast<float>(
                                 static_cast<uint32_t>(args[0].lo)))
                           : std::bit_cast<double>(args[0].lo);
      // ARM fcvtzs/fcvtzu saturate and map NaN to zero; C++ casts are undefined
      // there, so clamp explicitly.
      if (std::isnan(x)) {
        return ConcreteValue{};
      }
      if (expr.op == ExprOp::FpToIntS) {
        const double upper = std::ldexp(1.0, static_cast<int>(width - 1));
        if (x >= upper) {
          return pack(mask(U128{static_cast<uint64_t>(static_cast<int64_t>(upper - 1)), 0},
                           width));
        }
        if (x < -upper) {
          return pack(mask(U128{static_cast<uint64_t>(static_cast<int64_t>(-upper)), 0},
                           width));
        }
        return pack(
            mask(U128{static_cast<uint64_t>(static_cast<int64_t>(x)), 0}, width));
      }
      const double upper = std::ldexp(1.0, static_cast<int>(width));
      if (x <= 0.0) {
        return ConcreteValue{};
      }
      if (x >= upper) {
        return pack(maskOf(width));
      }
      return pack(mask(U128{static_cast<uint64_t>(x), 0}, width));
    }
    case ExprOp::IntToFpS:
    case ExprOp::IntToFpU: {
      const unsigned fromWidth = function_->expr(expr.operands[0]).type.bits();
      double value;
      if (expr.op == ExprOp::IntToFpS) {
        value = static_cast<double>(
            static_cast<int64_t>(sext(a, fromWidth, 64).lo));
      } else {
        value = static_cast<double>(zext(a, fromWidth, 64).lo);
      }
      if (width == 32) {
        return ConcreteValue{std::bit_cast<uint32_t>(static_cast<float>(value)), 0};
      }
      return ConcreteValue{std::bit_cast<uint64_t>(value), 0};
    }

    case ExprOp::Count:
      break;
  }
  return err(DiagCode::Internal,
             std::format("interpreter does not implement expression op {}",
                         toString(expr.op)));
}

ExecOutcome Interpreter::runBlock(BlockId blockId) {
  if (values_.size() != function_->valueCount()) {
    values_.assign(function_->valueCount(), ConcreteValue{});
    defined_.assign(function_->valueCount(), false);
  }
  std::fill(defined_.begin(), defined_.end(), false);

  const Block& block = function_->block(blockId);
  const auto define = [&](ValueId value, ConcreteValue contents) {
    values_[value.asSize()] = contents;
    defined_[value.asSize()] = true;
  };

  for (const OpId opId : block.ops) {
    const Op& op = function_->op(opId);
    const std::span<const ExprId> operands = function_->operands(op);
    switch (op.code) {
      case OpCode::ReadReg:
        define(op.result, readRegister(op.reg()));
        break;
      case OpCode::WriteReg: {
        auto value = eval(operands[0], 0);
        if (!value) {
          return fail(value.error().format(), op.va);
        }
        writeRegister(op.reg(), *value);
        break;
      }
      case OpCode::Load: {
        auto address = eval(operands[0], 0);
        if (!address) {
          return fail(address.error().format(), op.va);
        }
        auto contents = memory_->read(address->lo, op.type.bits() / 8);
        if (!contents) {
          return fail(contents.error().format(), op.va);
        }
        define(op.result, *contents);
        break;
      }
      case OpCode::Store: {
        auto address = eval(operands[0], 0);
        auto value = eval(operands[1], 0);
        if (!address) {
          return fail(address.error().format(), op.va);
        }
        if (!value) {
          return fail(value.error().format(), op.va);
        }
        if (auto written = memory_->write(address->lo, op.type.bits() / 8, *value);
            !written) {
          return fail(written.error().format(), op.va);
        }
        break;
      }
      case OpCode::Branch: {
        ExecOutcome outcome;
        outcome.stop = ExecStop::Branch;
        outcome.va = op.va;
        outcome.target = function_->block(function_->targets(op)[0]).va;
        return outcome;
      }
      case OpCode::CondBranch: {
        auto condition = eval(operands[0], 0);
        if (!condition) {
          return fail(condition.error().format(), op.va);
        }
        const std::span<const BlockId> targets = function_->targets(op);
        ExecOutcome outcome;
        outcome.stop = ExecStop::CondBranch;
        outcome.va = op.va;
        outcome.condition = (condition->lo & 1) != 0;
        outcome.target = function_->block(targets[0]).va;
        outcome.fallthrough = function_->block(targets[1]).va;
        return outcome;
      }
      case OpCode::IndirectBranch: {
        auto target = eval(operands[0], 0);
        if (!target) {
          return fail(target.error().format(), op.va);
        }
        ExecOutcome outcome;
        outcome.stop = ExecStop::IndirectBranch;
        outcome.va = op.va;
        outcome.target = target->lo;
        return outcome;
      }
      case OpCode::Call: {
        auto target = eval(operands[0], 0);
        if (!target) {
          return fail(target.error().format(), op.va);
        }
        ExecOutcome outcome;
        outcome.stop = ExecStop::Call;
        outcome.va = op.va;
        outcome.target = target->lo;
        return outcome;
      }
      case OpCode::Return: {
        ExecOutcome outcome;
        outcome.stop = ExecStop::Return;
        outcome.va = op.va;
        return outcome;
      }
      case OpCode::Unreachable: {
        ExecOutcome outcome;
        outcome.stop = ExecStop::Unreachable;
        outcome.va = op.va;
        return outcome;
      }
      case OpCode::Unimplemented: {
        ExecOutcome outcome;
        outcome.stop = ExecStop::Unimplemented;
        outcome.va = op.va;
        outcome.detail = std::string{function_->nameOf(op.payload)};
        return outcome;
      }
      case OpCode::Nop:
        break;
      case OpCode::Intrinsic: {
        const std::string_view name = function_->nameOf(op.payload);
        std::vector<ConcreteValue> arguments(operands.size());
        for (std::size_t index = 0; index < operands.size(); ++index) {
          auto argument = eval(operands[index], 0);
          if (!argument) {
            return fail(argument.error().format(), op.va);
          }
          arguments[index] = *argument;
        }
        ConcreteValue result{};
        if (!hook_ || !hook_(name, op.type, arguments, result)) {
          ExecOutcome outcome;
          outcome.stop = ExecStop::Intrinsic;
          outcome.va = op.va;
          outcome.detail = std::string{name};
          return outcome;
        }
        if (op.result.valid()) {
          define(op.result, result);
        }
        break;
      }
      case OpCode::Phi:
        return fail("phi nodes do not exist before SSA construction; the interpreter "
                    "runs at lifted maturity",
                    op.va);
      case OpCode::Count:
        return fail("op with no opcode", op.va);
    }
  }
  return fail("block has no terminator", block.endVa);
}

}  // namespace xdec::il
