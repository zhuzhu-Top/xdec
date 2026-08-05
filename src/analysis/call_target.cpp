// describeCallTarget / describeShape (see the header for what this is for).
#include "xdec/analysis/call_target.h"

#include <format>

namespace xdec::analysis {

namespace {

/// Widening casts and the like carry a value without changing which value it
/// is, so they say nothing about whether the target was decoded. Arithmetic and
/// bit operations do.
[[nodiscard]] bool isTransparent(il::ExprOp op) noexcept {
  switch (op) {
    case il::ExprOp::ZExt:
    case il::ExprOp::SExt:
    case il::ExprOp::Trunc:
    case il::ExprOp::Bitcast:
      return true;
    default:
      return false;
  }
}

/// The op defining `value`, or null when the value has no definition left (a
/// removed op's tombstone).
[[nodiscard]] const il::Op* definitionOf(const il::Function& function, il::ValueId value) {
  if (!function.hasValue(value)) {
    return nullptr;
  }
  const il::OpId definition = function.value(value).definition;
  return definition.valid() ? &function.op(definition) : nullptr;
}

/// The walk over a target expression. Bounded: an obfuscated target is a deep
/// tree, and a describer that runs out of budget must say so (`truncated`)
/// rather than describe a fragment as if it were the whole.
class Walker {
 public:
  explicit Walker(const il::Function& function) : function_(function) {}

  /// `decodedAbove` is whether anything between here and the call did
  /// arithmetic to the value — the difference between a pointer that is called
  /// and one that is decrypted first.
  void walk(il::ExprId id, bool decodedAbove) {
    if (++nodes_ > kMaxNodes) {
      shape.truncated = true;
      return;
    }

    const il::Expr& expr = function_.expr(id);
    if (expr.op == il::ExprOp::Value) {
      const il::ValueId value{static_cast<uint32_t>(expr.immediate)};
      const il::Op* definition = definitionOf(function_, value);
      if (definition != nullptr && definition->code == il::OpCode::Load) {
        shape.viaLoad = true;
        shape.decoded |= decodedAbove;
        const auto operands = function_.operands(*definition);
        if (!operands.empty()) {
          readAddress(operands[0]);
        }
      }
      return;
    }

    // A xor by a constant directly above the load is a key, and the single most
    // useful thing to be able to state about an encrypted table: it is the same
    // constant at every call site that shares a table, which is what lets a
    // reader group the call sites at all.
    if (expr.op == il::ExprOp::Xor && expr.operandCount == 2) {
      uint64_t key = 0;
      for (const int arm : {0, 1}) {
        if (function_.asConstant(expr.operands[arm], key)) {
          shape.hasXorKey = true;
          shape.xorKey = key;
          walk(expr.operands[arm ^ 1], true);
          return;
        }
      }
    }

    const bool opaque = !isTransparent(expr.op) && expr.op != il::ExprOp::Const;
    for (unsigned index = 0; index < expr.operandCount; ++index) {
      walk(expr.operands[index], decodedAbove || opaque);
    }
  }

  CallTargetShape shape;

 private:
  /// The addends of a load address. Sums only, because that is what indexing a
  /// table is; anything else in the address is recorded as one unscaled
  /// run-time term, which is the honest reading — an address that cannot be
  /// decomposed is one whose entry cannot be named.
  void readAddress(il::ExprId address) {
    uint64_t constant = 0;
    if (function_.asConstant(address, constant)) {
      // A constant address is not indexing at all. const-fold-memory would
      // have folded it were the memory immutable, so reaching here means the
      // pointer lives in memory the program may write.
      noteBase(constant);
      return;
    }
    const il::Expr& expr = function_.expr(address);
    if (expr.op == il::ExprOp::Add && expr.operandCount == 2) {
      readAddress(expr.operands[0]);
      readAddress(expr.operands[1]);
      return;
    }
    shape.terms.push_back(AddressTerm{scaleOf(address)});
  }

  /// The stride a run-time term is scaled by, or zero when it is unscaled.
  [[nodiscard]] uint64_t scaleOf(il::ExprId term) const {
    const il::Expr& expr = function_.expr(term);
    uint64_t amount = 0;
    if (expr.op == il::ExprOp::Mul && function_.asConstant(expr.operands[1], amount)) {
      return amount;
    }
    if (expr.op == il::ExprOp::Shl && function_.asConstant(expr.operands[1], amount) &&
        amount < 64) {
      return uint64_t{1} << amount;
    }
    return 0;
  }

  /// Two different bases in one target (a select over two tables) means no one
  /// base describes it, so the claim is withdrawn rather than picked from.
  void noteBase(uint64_t base) {
    if (shape.hasTableBase && shape.tableBase != base) {
      shape.hasTableBase = false;
      shape.truncated = true;
      return;
    }
    shape.hasTableBase = true;
    shape.tableBase = base;
  }

  const il::Function& function_;
  unsigned nodes_ = 0;
  /// Enough for the decode chains these samples build over a table load, small
  /// enough that describing a call is never the expensive part of a pass.
  static constexpr unsigned kMaxNodes = 512;
};

/// The load address as a formula: the constant base where there is one, then one
/// letter per run-time term with its stride. `v` for an unscaled term because it
/// is a value and nothing more is claimed about it; `i`, `j`, ... for the scaled
/// ones because a scale is what makes a term an index.
[[nodiscard]] std::string formatAddress(const CallTargetShape& shape) {
  std::string out;
  const auto append = [&out](std::string_view piece) {
    out += out.empty() ? "" : " + ";
    out += piece;
  };
  if (shape.hasTableBase) {
    append(std::format("{:#x}", shape.tableBase));
  }
  char index = 'i';
  for (const AddressTerm& term : shape.terms) {
    if (term.stride == 0) {
      append("v");
      continue;
    }
    append(std::format("{}*{:#x}", index, term.stride));
    if (index < 'z') {
      ++index;
    }
  }
  return out.empty() ? "?" : out;
}

}  // namespace

CallTargetShape describeCallTarget(const il::Function& function, il::ExprId target) {
  Walker walker(function);
  walker.walk(target, false);
  return walker.shape;
}

std::string describeShape(const CallTargetShape& shape, const TargetSlotFacts& slot) {
  if (!shape.viaLoad) {
    return "indirect call: target computed, not read from memory";
  }

  std::string out = std::format("indirect call: target = load({})", formatAddress(shape));
  if (shape.hasXorKey) {
    out += std::format(" ^ {:#x}", shape.xorKey);
  } else if (shape.decoded) {
    out += ", then transformed by arithmetic";
  }
  if (shape.isEncryptedTableDispatch()) {
    // ASCII only: this string ends up inside a C comment, and a decompiler that
    // needs its output read in a particular encoding has made a reader's job
    // harder for decoration.
    out += " (encrypted dispatch table)";
  }

  // The slot clauses apply to a single slot only: see TargetSlotFacts.
  if (!shape.hasTableBase || !shape.terms.empty()) {
    return out;
  }
  if (!slot.loader.importName.empty()) {
    out += std::format("; the loader fills that slot from the imported symbol '{}'",
                       slot.loader.importName);
  } else if (slot.loader.hasAddress) {
    out += std::format("; the loader fills that slot with {:#x}", slot.loader.address);
  } else if (!slot.immutable) {
    // Only worth saying when nothing better is known: paired with a loader value
    // it would deny in one clause what the next one states.
    return out + "; writable memory, so the target is not knowable from the image";
  } else {
    return out;
  }
  // A loader value is the slot's *initial* contents. Immutable memory keeps them
  // for the life of the program; writable memory only promises them at startup,
  // and a reader chasing a call that does something else needs to know which of
  // the two they are looking at.
  if (!slot.immutable) {
    out += " (writable, so it may have been replaced since)";
  }
  return out;
}

}  // namespace xdec::analysis
