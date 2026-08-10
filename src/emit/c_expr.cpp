// ExprPrinter (see the header for the two rules it enforces).
#include "c_expr.h"

#include <format>
#include <vector>

#include "c_flags.h"

namespace xdec::emit {

std::string ExprPrinter::text(il::ExprId id) { return value(id).text; }

std::string ExprPrinter::integerOperand(il::ExprId id) {
  const Text operand = value(id);
  if (operand.pointer) {
    return std::format("(uint64_t)({})", operand.text);
  }
  return operand.text;
}

bool ExprPrinter::isRedundantRootZext(const il::Expr& expr) const {
  return expr.op == il::ExprOp::ZExt &&
         ctx_.function.expr(expr.operand(0)).type.isScalarInteger();
}

std::string ExprPrinter::rootText(il::ExprId id) {
  if (const auto cached = materializedText_.find(id.index());
      cached != materializedText_.end()) {
    return cached->second.text;
  }
  const il::Expr& expr = ctx_.function.expr(id);
  // Dropping the cast here means printing straight through to `operand(0)`'s
  // own text, under `operand(0)`'s own identity -- fine when this ZExt node
  // is not itself something another use needs to name, but wrong the moment
  // it is: a parent elsewhere in this scope may reference this exact ZExt
  // node (not its operand) enough times to make IT the shared one (see
  // isShared's own per-node counting), and skipping straight to the operand
  // here would print that shared value's full text instead of the `_cseN`
  // name every other use of this same node already agreed on.
  if (!isShared(id, expr) && isRedundantRootZext(expr)) {
    return integerOperand(expr.operand(0));
  }
  return text(id);
}

std::string ExprPrinter::rootInteger(il::ExprId id) {
  if (const auto cached = materializedText_.find(id.index());
      cached != materializedText_.end()) {
    return cached->second.text;
  }
  const il::Expr& expr = ctx_.function.expr(id);
  if (!isShared(id, expr) && isRedundantRootZext(expr)) {
    return integerOperand(expr.operand(0));
  }
  return integerOperand(id);
}

void ExprPrinter::beginScope(const std::vector<il::ExprId>& roots) {
  referenceCounts_.clear();
  materializedText_.clear();
  pendingDecls_.clear();
  for (const il::ExprId root : roots) {
    countReferences(root);
  }
}

void ExprPrinter::extendScope(const std::vector<il::ExprId>& moreRoots) {
  for (const il::ExprId root : moreRoots) {
    countReferences(root);
  }
}

std::vector<std::string> ExprPrinter::takePendingDecls() {
  std::vector<std::string> decls;
  decls.swap(pendingDecls_);
  return decls;
}

ExprPrinter::Text ExprPrinter::value(il::ExprId id) {
  const il::Expr& expr = ctx_.function.expr(id);
  if (expr.op == il::ExprOp::Value) {
    const il::ValueId valueId{static_cast<uint32_t>(expr.immediate)};
    if (const auto snapshot = ctx_.snapshots.find(valueId.index());
        snapshot != ctx_.snapshots.end()) {
      return {snapshot->second, false};
    }
    // A load a header typed: named through tempNames, and declared as the
    // pointer the field is, so arithmetic on it needs the cast back.
    if (const std::string* named = ctx_.tempFor(valueId);
        named != nullptr && ctx_.valueIsPointer(valueId)) {
      return {*named, true};
    }
    if (const analysis::Variable* temp = ctx_.variables.tempFor(valueId)) {
      // Or a pointer only a header knows about: a value read out of a declared
      // struct field is declared with that field's type, and arithmetic on it
      // has to cast back for the same reason a parameter's does.
      return {temp->name,
              temp->type.pointerDepth > 0 || ctx_.valueIsPointer(valueId)};
    }
  }
  // An argument declared as a pointer is the same story as a pointer temp:
  // whatever the machine code did with the register, the C the reader gets
  // scales arithmetic by the pointee, so the cast back to an integer has to be
  // there. This matters most where the declaration came from a header --
  // `v + 4` on an `EvalVec3*` would step twelve bytes.
  if (expr.op == il::ExprOp::EntryReg) {
    const il::RegId root{static_cast<uint32_t>(expr.immediate)};
    if (const analysis::Variable* arg = ctx_.variables.argumentFor(root)) {
      return {ctx_.argumentName(*arg), ctx_.argumentIsPointer(*arg)};
    }
  }
  return materialized(id);
}

/// Reference-counts every node reachable from `root` (each node visited once
/// regardless of how many parents share it, so this stays linear in the DAG's
/// size rather than the tree it would unfold into).
void ExprPrinter::countReferences(il::ExprId root) {
  std::vector<il::ExprId> stack{root};
  ++referenceCounts_[root.index()];
  while (!stack.empty()) {
    const il::ExprId id = stack.back();
    stack.pop_back();
    const il::Expr& expr = ctx_.function.expr(id);
    for (unsigned index = 0; index < expr.operandCount; ++index) {
      const il::ExprId child = expr.operand(index);
      const bool first = referenceCounts_.find(child.index()) == referenceCounts_.end();
      ++referenceCounts_[child.index()];
      if (first) {
        stack.push_back(child);
      }
    }
  }
}

/// Worth a temporary: reached from 2+ places, and not a leaf a temporary
/// would not shorten anything for (a bare constant or name is already as
/// short as a reference to it would be).
bool ExprPrinter::isShared(il::ExprId id, const il::Expr& expr) const {
  if (expr.operandCount == 0 || !expr.type.isScalarInteger()) {
    return false;
  }
  const auto count = referenceCounts_.find(id.index());
  return count != referenceCounts_.end() && count->second >= 2;
}

/// Checks the cache a shared node's first print filled, otherwise prints via
/// `inner()` and, if this node turned out to be shared, promotes that text
/// to a named temporary: declared alongside every other temp (see
/// CContext::cseTemps) and assigned by a plain statement `takePendingDecls()`
/// hands the caller to print right before the code that needs it, so a
/// reference after this one is just the name.
ExprPrinter::Text ExprPrinter::materialized(il::ExprId id) {
  if (const auto cached = materializedText_.find(id.index());
      cached != materializedText_.end()) {
    return cached->second;
  }
  const il::Expr& expr = ctx_.function.expr(id);
  if (!isShared(id, expr)) {
    return Text{inner(id), false};
  }
  // A shared node becomes a plain `_cseN = <text>;` statement, assigned to a
  // temp already declared at `expr.type`'s width (see CContext::cseTemps):
  // a leading ZExt cast here is exactly the redundant-root case `rootText`
  // documents, since this assignment is the same kind of conversion.
  const std::string name = std::format("_cse{}", cseCounter_++);
  ctx_.cseTemps.emplace_back(name, expr.type.bits());
  const std::string initializer =
      isRedundantRootZext(expr) ? integerOperand(expr.operand(0)) : inner(id);
  pendingDecls_.push_back(std::format("{} = {};", name, initializer));
  const Text result{name, false};
  materializedText_.emplace(id.index(), result);
  return result;
}

/// The operand of a signed operation, in the signed type of `width`. C would
/// otherwise convert the whole operation to unsigned as soon as one side is.
std::string ExprPrinter::signedOperand(il::ExprId id, uint32_t width) {
  return std::format("({})({})", intType(width, true), integerOperand(id));
}

std::string ExprPrinter::inner(il::ExprId id) {
  const il::Expr& e = ctx_.function.expr(id);
  switch (e.op) {
    case il::ExprOp::Const:
      return std::format("0x{:x}", e.immediate);
    case il::ExprOp::Value: {
      // Values with no temporary are the ones no stage claimed: a bug in the
      // pipeline rather than a value that is legitimately nameless, so say so
      // instead of printing a plausible zero.
      const il::ValueId valueId{static_cast<uint32_t>(e.immediate)};
      if (const std::string* temp = ctx_.tempFor(valueId)) {
        return *temp;
      }
      return std::format("/*unnamed-value-%{}*/0", valueId.index());
    }
    case il::ExprOp::Undef:
      return "/*undef*/0";
    case il::ExprOp::EntryReg: {
      const il::RegId root{static_cast<uint32_t>(e.immediate)};
      if (const analysis::Variable* arg = ctx_.variables.argumentFor(root)) {
        // Through the context, not straight off the variable: an imported
        // prototype may have named this position, and the signature will have
        // used that name.
        return ctx_.argumentName(*arg);
      }
      const il::RegisterInfo& info = ctx_.function.registers()[root];
      const std::string name = std::format("__entry_{}", info.name);
      ctx_.entryLeaves.emplace(name, info.bits == 0 ? 64 : info.bits);
      return name;
    }
    case il::ExprOp::Add:
      return infix(id, "+", false);
    case il::ExprOp::Sub:
      return infix(id, "-", false);
    case il::ExprOp::Mul:
      return infix(id, "*", false);
    case il::ExprOp::And:
      return infix(id, "&", false);
    case il::ExprOp::Or:
      return infix(id, "|", false);
    case il::ExprOp::Xor:
      return infix(id, "^", false);
    case il::ExprOp::Shl:
      return shift(id, "<<");
    case il::ExprOp::ShrU:
      return shift(id, ">>");
    case il::ExprOp::ShrS:
      // Shifts promote their operands independently, so only the shifted side
      // needs the signed type for C to produce an arithmetic shift.
      return widthWrap(e,
                       std::format("({} >> {})",
                                   signedOperand(e.operand(0), e.type.bits()),
                                   integerOperand(e.operand(1))),
                       true);
    case il::ExprOp::DivU:
      return infix(id, "/", false);
    case il::ExprOp::RemU:
      return infix(id, "%", false);
    case il::ExprOp::DivS:
      return infix(id, "/", true);
    case il::ExprOp::RemS:
      return infix(id, "%", true);
    case il::ExprOp::MulHiU:
    case il::ExprOp::MulHiS: {
      const bool sign = e.op == il::ExprOp::MulHiS;
      const uint32_t width = e.type.bits();
      ctx_.helpers.insert(std::format("mulhi{}{}", sign ? "s" : "u", width));
      return std::format("xdec_mulhi{}{}({}, {})", sign ? "s" : "u", width,
                         integerOperand(e.operand(0)),
                         integerOperand(e.operand(1)));
    }
    case il::ExprOp::RotR:
    case il::ExprOp::RotL: {
      // Bit rotate: extremely common in the hash/PRNG-shaped code obfuscators
      // favour, so this is not the rare case an embedder-stub elsewhere in
      // this switch is for — it gets a real, portable definition, in
      // xdec_helpers.h rather than repeated inline in every decompiled file.
      const bool right = e.op == il::ExprOp::RotR;
      const uint32_t width = e.type.bits();
      ctx_.helpers.insert(std::format("rot{}{}", right ? "r" : "l", width));
      return std::format("rot{}{}({}, {})", right ? "r" : "l", width,
                         integerOperand(e.operand(0)),
                         integerOperand(e.operand(1)));
    }
    case il::ExprOp::Ctz:
      // Same shape as Clz just above: the zero-input result is target-defined,
      // so this stays an embedder-supplied stub rather than guessing.
      ctx_.helpers.insert(std::format("ctz{}", e.type.bits()));
      return std::format("xdec_ctz{}({})", e.type.bits(),
                         integerOperand(e.operand(0)));
    case il::ExprOp::Bitcast:
      // Reinterpreting the same bits at a possibly different type: this IL
      // already treats every scalar as a raw bit pattern, so the only work is
      // the width cast (a genuine type change, e.g. int <-> float, still goes
      // through the Fp* conversions below, which is where semantics live).
      return std::format("(({})({}))", intType(e.type.bits()),
                         integerOperand(e.operand(0)));
    case il::ExprOp::Concat: {
      // operand(0) is the high half, operand(1) the low half (see ceval's
      // Concat case): result = hi << lowWidth | lo.
      const uint32_t lowWidth = ctx_.function.expr(e.operand(1)).type.bits();
      return std::format("((({})({}) << {}) | ({})({}))", intType(e.type.bits()),
                         integerOperand(e.operand(0)), lowWidth,
                         intType(e.type.bits()), integerOperand(e.operand(1)));
    }
    case il::ExprOp::Neg:
      return widthWrap(e, std::format("(-({}))", integerOperand(e.operand(0))),
                       false);
    case il::ExprOp::Not:
      // A 1-bit not is logical. `~` on a bool yields -1 or -2 in C, both true,
      // which would turn every negated condition into a tautology.
      if (e.type.bits() == 1) {
        return std::format("(!({}))", text(e.operand(0)));
      }
      return widthWrap(e, std::format("(~({}))", integerOperand(e.operand(0))),
                       false);
    case il::ExprOp::CmpEq:
      return compare(id, "==", false);
    case il::ExprOp::CmpNe:
      return compare(id, "!=", false);
    case il::ExprOp::CmpLtU:
      return compare(id, "<", false);
    case il::ExprOp::CmpLeU:
      return compare(id, "<=", false);
    case il::ExprOp::CmpLtS:
      return compare(id, "<", true);
    case il::ExprOp::CmpLeS:
      return compare(id, "<=", true);
    case il::ExprOp::ZExt:
    case il::ExprOp::Trunc: {
      // A cast to the width the operand already has says nothing. Beyond
      // that, this text can end up as an *operand* of another C operator
      // (inner() is the recursive path every nested embedding goes
      // through) rather than the target of a plain conversion, and for one
      // operator in particular -- a shift, see shift() below -- what
      // matters is the operand's OWN promoted width, not whatever the
      // destination width here implies. So the cast always stays; only a
      // "root" call (rootText/rootInteger, used from statement-level
      // contexts that really do just convert) can drop it.
      const il::Expr& source = ctx_.function.expr(e.operand(0));
      if (source.type.isScalarInteger() && source.type.bits() == e.type.bits()) {
        return integerOperand(e.operand(0));
      }
      return std::format("(({})({}))", intType(e.type.bits()),
                         integerOperand(e.operand(0)));
    }
    case il::ExprOp::SExt:
      return signExtend(id);
    case il::ExprOp::Select:
      return std::format("({} ? {} : {})", text(e.operand(0)),
                         text(e.operand(1)), text(e.operand(2)));
    case il::ExprOp::Extract: {
      const uint32_t width = e.type.bits();
      if (e.immediate == 0) {
        return std::format("(({})({}))", intType(width),
                           integerOperand(e.operand(0)));
      }
      return std::format("(({})({} >> {}))", intType(width),
                         integerOperand(e.operand(0)), e.immediate);
    }
    case il::ExprOp::ByteSwap:
      ctx_.helpers.insert(std::format("bswap{}", e.type.bits()));
      return std::format("bswap{}({})", e.type.bits(),
                         integerOperand(e.operand(0)));
    case il::ExprOp::BitReverse:
      ctx_.helpers.insert(std::format("brev{}", e.type.bits()));
      return std::format("xdec_brev{}({})", e.type.bits(),
                         integerOperand(e.operand(0)));
    case il::ExprOp::PopCount:
      ctx_.helpers.insert("popcount64");
      return std::format("popcount64({})", integerOperand(e.operand(0)));
    case il::ExprOp::Clz:
      ctx_.helpers.insert(std::format("clz{}", e.type.bits()));
      return std::format("xdec_clz{}({})", e.type.bits(),
                         integerOperand(e.operand(0)));
    case il::ExprOp::FlagCond:
      return printFlagCond(ctx_, *this, id);
    case il::ExprOp::FlagBit:
      ctx_.helpers.insert("flagbit");
      return std::format("xdec_flagbit({}, {})", text(e.operand(0)), e.immediate);
    case il::ExprOp::FlagDef:
      // Only meaningful under a FlagCond or FlagBit; alone it says nothing.
      return "/*flagdef*/0";
    case il::ExprOp::FAdd:
    case il::ExprOp::FSub:
    case il::ExprOp::FMul:
    case il::ExprOp::FDiv:
    case il::ExprOp::FNeg: {
      const std::string name = std::format("{}{}", il::toString(e.op), e.type.bits());
      ctx_.helpers.insert(name);
      std::string call = std::format("xdec_{}(", name);
      for (unsigned index = 0; index < e.operandCount; ++index) {
        call += index == 0 ? "" : ", ";
        call += text(e.operand(index));
      }
      return call + ")";
    }
    default:
      return std::format("/*{}?*/0", il::toString(e.op));
  }
}

std::string ExprPrinter::infix(il::ExprId id, std::string_view op, bool sign) {
  const il::Expr& e = ctx_.function.expr(id);
  const uint32_t width = e.type.bits();
  const std::string left =
      sign ? signedOperand(e.operand(0), width) : integerOperand(e.operand(0));
  const std::string right =
      sign ? signedOperand(e.operand(1), width) : integerOperand(e.operand(1));
  return widthWrap(e, std::format("({} {} {})", left, op, right), sign);
}

/// Shl/ShrU: unlike +, -, *, &, |, ^ (whose usual arithmetic conversions
/// promote a narrower operand to match a wider sibling automatically), a
/// C shift's well-definedness depends only on the shifted operand's OWN
/// promoted width -- shifting by an amount at or past that width is
/// undefined behaviour, regardless of what width the surrounding
/// expression implies. widthWrap (which only ever adds a cast around the
/// whole result, and only for results narrower than int) does not cover
/// this, so the shifted operand is force-cast to this shift's own width
/// whenever it is not already exactly that width -- mirroring how ShrS
/// already forces a signed cast for the same reason.
std::string ExprPrinter::shift(il::ExprId id, std::string_view op) {
  const il::Expr& e = ctx_.function.expr(id);
  const uint32_t width = e.type.bits();
  const il::Expr& source = ctx_.function.expr(e.operand(0));
  const std::string left = (source.type.isScalarInteger() && source.type.bits() == width)
                               ? integerOperand(e.operand(0))
                               : std::format("(({})({}))", intType(width),
                                            integerOperand(e.operand(0)));
  return widthWrap(
      e, std::format("({} {} {})", left, op, integerOperand(e.operand(1))), false);
}

/// Comparisons take their width from the operands, not from the i1 result.
std::string ExprPrinter::compare(il::ExprId id, std::string_view op, bool sign) {
  const il::Expr& e = ctx_.function.expr(id);
  const il::Expr& lhs = ctx_.function.expr(e.operand(0));
  const uint32_t width = lhs.type.isScalarInteger() ? lhs.type.bits() : 64;
  if (!sign) {
    return std::format("({} {} {})", integerOperand(e.operand(0)), op,
                       integerOperand(e.operand(1)));
  }
  return std::format("({} {} {})", signedOperand(e.operand(0), width), op,
                     signedOperand(e.operand(1), width));
}

/// Sign extension must go through the signed type of the SOURCE width: a cast
/// straight to the destination type would zero-extend, because the operand's C
/// type is unsigned. A 1-bit source has no signed C type, so it negates.
std::string ExprPrinter::signExtend(il::ExprId id) {
  const il::Expr& e = ctx_.function.expr(id);
  const il::Expr& source = ctx_.function.expr(e.operand(0));
  const std::string destination = intType(e.type.bits(), true);
  const uint32_t sourceWidth =
      source.type.isScalarInteger() ? source.type.bits() : 64;
  if (sourceWidth == 1) {
    return std::format("(-(({})({})))", destination, text(e.operand(0)));
  }
  if (sourceWidth >= e.type.bits()) {
    return std::format("(({})({}))", destination, integerOperand(e.operand(0)));
  }
  return std::format("(({})({})({}))", destination, intType(sourceWidth, true),
                     integerOperand(e.operand(0)));
}

/// The IL's arithmetic wraps at its declared width; C's does not below `int`,
/// so a result narrower than `int` carries the cast that makes it wrap. On
/// every target this project decompiles, `int` is exactly 32 bits, so a
/// 32-bit (or wider) operation between already-32-bit operands is already
/// computed at that width by C's own rules — the cast would be a no-op, just
/// noise repeated at every arithmetic node. Only 1/8/16-bit results, which C
/// promotes to `int` before operating, need it to truncate back down.
std::string ExprPrinter::widthWrap(const il::Expr& expr, std::string text,
                                  bool sign) {
  const uint32_t width = expr.type.bits();
  if (width >= 32 || !expr.type.isScalarInteger()) {
    return text;
  }
  return std::format("({}){}", intType(width, sign), text);
}

}  // namespace xdec::emit
