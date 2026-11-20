// ExprPrinter (see the header for the two rules it enforces).
#include "c_expr.h"

#include <bit>
#include <cmath>
#include <format>
#include <vector>

#include "address_render.h"
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

std::string ExprPrinter::pointerOperand(il::ExprId id, uint32_t width) {
  // A stack slot or an immutable global reads the same way here as it does
  // as an lvalue: `&var_70`/`g_400900`, not the raw arithmetic that
  // classifies it (see AddressRenderer). Tried ahead of `value()` because
  // a StackSlot expression is ordinary arithmetic to it (`Sub(entry_sp,
  // 0x70)`), ExprPrinter's own leaf-recognition patterns notwithstanding.
  if (const auto rendered = AddressRenderer(ctx_).render(id, AddressRole::PointerValue, width)) {
    if (rendered->pointer) {
      return rendered->text;
    }
    return std::format("({}*)({})", intType(width), rendered->text);
  }
  const Text operand = value(id);
  if (operand.pointer) {
    return operand.text;
  }
  // A C-style cast already binds tighter than dereference or a call's comma,
  // the two contexts this is used in, so no outer parens are needed to keep
  // `*{result}` or `f({result})` reading the way the cast alone would.
  return std::format("({}*)({})", intType(width), operand.text);
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

std::string ExprPrinter::condition(il::ExprId id, bool invert) {
  if (!invert) {
    return rootText(id);
  }
  const il::Expr& expr = ctx_.function.expr(id);
  switch (expr.op) {
    case il::ExprOp::CmpEq:
      return compare(id, "!=", false);
    case il::ExprOp::CmpNe:
      return compare(id, "==", false);
    case il::ExprOp::CmpLtU:
      return compare(id, ">=", false);
    case il::ExprOp::CmpLeU:
      return compare(id, ">", false);
    case il::ExprOp::CmpLtS:
      return compare(id, ">=", true);
    case il::ExprOp::CmpLeS:
      return compare(id, ">", true);
    case il::ExprOp::Not:
      if (expr.type.bits() == 1) {
        return rootText(expr.operand(0));
      }
      break;
    default:
      break;
  }
  return std::format("!({})", rootText(id));
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
  decls.reserve(pendingDecls_.size());
  for (PendingCseDecl& d : pendingDecls_) {
    decls.push_back(std::move(d.text));
  }
  pendingDecls_.clear();
  return decls;
}

std::vector<PendingCseDecl> ExprPrinter::takePendingHlDecls() {
  std::vector<PendingCseDecl> decls;
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
    // A load stack-load-fold proved has exactly one reader, this one: its
    // own statement never printed (see CContext's constructor and
    // `deadOps`), so what would have been its temporary reads the slot's
    // own text directly instead.
    if (const auto inlined = ctx_.inlinedStackLoads.find(valueId.index());
        inlined != ctx_.inlinedStackLoads.end()) {
      return {inlined->second.text, inlined->second.pointer};
    }
    // A load load-inline proved has exactly one reader through a Global or
    // Other address: same shape as the stack-slot case just above, but the
    // substitution text has no name fixed ahead of printing, so it is
    // rendered here exactly the way StmtPrinter::memoryLvalue would have
    // printed it at the load's own (now-dead) statement -- struct field
    // first, then a named global, then the generic cast dereference.
    if (const auto inlined = ctx_.inlinedMemoryLoads.find(valueId.index());
        inlined != ctx_.inlinedMemoryLoads.end()) {
      const il::ExprId address = inlined->second.address;
      const uint32_t width = inlined->second.width;
      if (std::string field = ctx_.fieldAccess(address, width, valueId); !field.empty()) {
        return {field, false};
      }
      uint64_t absolute = 0;
      if (ctx_.function.asConstant(address, absolute)) {
        if (const std::string* global = ctx_.globalName(absolute)) {
          return {std::format("(*({}*)({}))", intType(width), *global), false};
        }
      }
      return {std::format("(*{})", pointerOperand(address, width)), false};
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
  if ((expr.op == il::ExprOp::Add || expr.op == il::ExprOp::Sub) &&
      expr.type.bits() == 64) {
    if (std::string localAddr = ctx_.addressOfLocal(id); !localAddr.empty()) {
      return {std::move(localAddr), true};
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
  const auto visit = [&](il::ExprId child) {
    const bool first = referenceCounts_.find(child.index()) == referenceCounts_.end();
    ++referenceCounts_[child.index()];
    if (first) {
      stack.push_back(child);
    }
  };
  while (!stack.empty()) {
    const il::ExprId id = stack.back();
    stack.pop_back();
    const il::Expr& expr = ctx_.function.expr(id);
    // An inlined load prints its address in place of the value (see value()),
    // and that address belongs to a dead op no scope's roots include. Left
    // uncounted, every node in it reads as unshared and prints as a tree.
    if (expr.op == il::ExprOp::Value) {
      if (const auto inlined =
              ctx_.inlinedMemoryLoads.find(static_cast<uint32_t>(expr.immediate));
          inlined != ctx_.inlinedMemoryLoads.end()) {
        visit(inlined->second.address);
      }
      continue;
    }
    for (unsigned index = 0; index < expr.operandCount; ++index) {
      visit(expr.operand(index));
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
  if (expr.operandCount != 0 && !referenceCounts_.contains(id.index())) {
    // Reached from outside every root the scope counted: count it now, or its
    // shared subtrees would all read as unshared and print as a tree.
    countReferences(id);
  }
  if (!isShared(id, expr)) {
    return Text{inner(id), false};
  }
  // A shared node becomes a plain `_cseN = <text>;` statement, assigned to a
  // temp already declared at `expr.type`'s width (see CContext::cseTemps):
  // a leading ZExt cast here is exactly the redundant-root case `rootText`
  // documents, since this assignment is the same kind of conversion.
  const std::string name = std::format("_cse{}", cseCounter_++);
  bool pointer = false;
  std::string initializer;
  if (const auto rendered =
          AddressRenderer(ctx_).render(id, AddressRole::PointerValue, 8);
      rendered.has_value() && rendered->pointer) {
    pointer = true;
    initializer = rendered->text;
  } else {
    initializer =
        isRedundantRootZext(expr) ? integerOperand(expr.operand(0)) : inner(id);
  }
  ctx_.cseTemps.push_back(CseTemp{name, expr.type.bits(), pointer});
  pendingDecls_.push_back(
      PendingCseDecl{name, std::format("{} = {};", name, initializer), nullptr});
  const Text result{name, pointer};
  materializedText_.emplace(id.index(), result);
  return result;
}

std::optional<std::string> ExprPrinter::materializeAs(il::ExprId id,
                                                       const std::string& preferredName) {
  if (materializedText_.contains(id.index())) {
    return std::nullopt;
  }
  const il::Expr& expr = ctx_.function.expr(id);
  if (!isShared(id, expr)) {
    return std::nullopt;
  }
  // Same initializer `materialized()` would compute for a fresh `_cseN`;
  // only the name that gets cached for later references differs.
  std::string initializer =
      isRedundantRootZext(expr) ? integerOperand(expr.operand(0)) : inner(id);
  materializedText_.emplace(id.index(), Text{preferredName, false});
  return initializer;
}

/// The operand of a signed operation, in the signed type of `width`. C would
/// otherwise convert the whole operation to unsigned as soon as one side is.
std::string ExprPrinter::signedOperand(il::ExprId id, uint32_t width) {
  uint64_t constant = 0;
  if (width <= 32 && ctx_.function.asConstant(id, constant)) {
    const uint64_t signBit = uint64_t{1} << (width - 1);
    // A non-negative 32-bit-range constant (`<= 0x7fffffff`) is already signed
    // `int` in C, matching `(int32_t)(0x1f)` without the redundant cast.
    if (constant < signBit) {
      return std::format("0x{:x}", constant);
    }
  }
  const il::Expr& expr = ctx_.function.expr(id);
  if (expr.op == il::ExprOp::Value) {
    const il::ValueId valueId{static_cast<uint32_t>(expr.immediate)};
    if (const analysis::Variable* temp = ctx_.variables.tempFor(valueId);
        temp != nullptr && temp->type.pointerDepth == 0 &&
        temp->type.isSigned && temp->type.width == width &&
        !ctx_.typeOfValue(valueId).valid()) {
      return temp->name;
    }
  }
  return std::format("({})({})", intType(width, true), integerOperand(id));
}

std::string ExprPrinter::inner(il::ExprId id) {
  const il::Expr& e = ctx_.function.expr(id);
  switch (e.op) {
    case il::ExprOp::Const:
      if (e.type.bits() == 64 && e.immediate >= 0x10000 && ctx_.options.addresses) {
        const AddressFacts facts = ctx_.options.addresses(e.immediate);
        if (facts.cstringSection) {
          if (const std::optional<analysis::ImageLiteral> literal =
                  ctx_.literals.at(e.immediate, /*allowEmpty=*/true)) {
            return analysis::quoteCString(literal->text);
          }
        }
        if (facts.cfstringLiteral.has_value()) {
          const std::string* global = ctx_.globalName(e.immediate);
          const std::string base =
              global != nullptr ? *global : std::format("0x{:x}", e.immediate);
          return std::format("{} /* @{} */", base,
                             analysis::quoteCString(*facts.cfstringLiteral));
        }
      }
      return std::format("0x{:x}", e.immediate);
    case il::ExprOp::Value: {
      // Values with no temporary are the ones no stage claimed: a bug in the
      // pipeline rather than a value that is legitimately nameless, so say so
      // instead of printing a plausible zero.
      const il::ValueId valueId{static_cast<uint32_t>(e.immediate)};
      if (const std::string* temp = ctx_.tempFor(valueId)) {
        return *temp;
      }
      return std::format("/*unnamed-value-%{}*/0x0", valueId.index());
    }
    case il::ExprOp::Undef:
      return "/*undef*/0x0";
    case il::ExprOp::EntryReg: {
      const il::RegId root{static_cast<uint32_t>(e.immediate)};
      if (const analysis::Variable* arg = ctx_.variables.argumentFor(root)) {
        // Through the context, not straight off the variable: an imported
        // prototype may have named this position, and the signature will have
        // used that name.
        return ctx_.argumentName(*arg);
      }
      const il::RegisterInfo& info = ctx_.function.registers()[root];
      if (info.name == "x8") {
        ctx_.hasSret = true;
        return "_sret";
      }
      const std::string name = std::format("__entry_{}", info.name);
      ctx_.entryLeaves.emplace(name, info.bits == 0 ? 64 : info.bits);
      if (ctx_.options.entryRegs != nullptr) {
        if (const std::optional<uint64_t> resolved = ctx_.options.entryRegs->resolve(info.name)) {
          ctx_.anchoredEntryLeaves.emplace(name, *resolved);
        }
      }
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
    case il::ExprOp::Bitcast: {
      if (e.type.isFloat()) {
        uint64_t bits = 0;
        if (ctx_.function.asConstant(e.operand(0), bits)) {
          if (e.type.bits() == 64) {
            const double d = std::bit_cast<double>(bits);
            if (std::isfinite(d)) {
              std::string s = std::format("{}", d);
              if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
                  s.find('E') == std::string::npos) {
                s += ".0";
              }
              return s;
            }
          } else if (e.type.bits() == 32) {
            const float f = std::bit_cast<float>(static_cast<uint32_t>(bits));
            if (std::isfinite(f)) {
              std::string s = std::format("{}", f);
              if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
                  s.find('E') == std::string::npos) {
                s += ".0";
              }
              return s + "f";
            }
          }
        }
        return std::format("(({})({}))", e.type.bits() == 32 ? "float" : "double",
                           integerOperand(e.operand(0)));
      }
      // Reinterpreting the same bits at a possibly different type: this IL
      // already treats every scalar as a raw bit pattern, so the only work is
      // the width cast (a genuine type change, e.g. int <-> float, still goes
      // through the Fp* conversions below, which is where semantics live).
      return std::format("(({})({}))", intType(e.type.bits()),
                         integerOperand(e.operand(0)));
    }
    case il::ExprOp::Concat: {
      // operand(0) is the high half, operand(1) the low half (see ceval's
      // Concat case): result = hi << lowWidth | lo.
      const uint32_t lowWidth = ctx_.function.expr(e.operand(1)).type.bits();
      return std::format("((({})({}) << 0x{:x}) | ({})({}))", intType(e.type.bits()),
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
      // Same "already has that width" case as just above, just seen through
      // the argument's own C declaration rather than the raw register's:
      // `arg1`'s type in the signature already IS this width whenever
      // nothing more specific (a header's prototype) claims otherwise, so
      // its bare name promotes exactly as `(uint32_t)(arg1)` would have --
      // safe for the same reason dropping the cast above is.
      if (source.op == il::ExprOp::EntryReg) {
        const il::RegId root{static_cast<uint32_t>(source.immediate)};
        if (const analysis::Variable* arg = ctx_.variables.argumentFor(root);
            arg != nullptr && arg->type.pointerDepth == 0 &&
            arg->type.width == e.type.bits() && !ctx_.argumentType(*arg).valid()) {
          return integerOperand(e.operand(0));
        }
      }
      return std::format("(({})({}))", intType(e.type.bits()),
                         integerOperand(e.operand(0)));
    }
    case il::ExprOp::SExt:
      return signExtend(id);
    case il::ExprOp::Select:
      return std::format("({} ? {} : {})", text(e.operand(0)), text(e.operand(1)),
                         text(e.operand(2)));
    case il::ExprOp::Extract: {
      const uint32_t width = e.type.bits();
      if (e.immediate == 0) {
        return std::format("(({})({}))", intType(width),
                           integerOperand(e.operand(0)));
      }
      return std::format("(({})({} >> 0x{:x}))", intType(width),
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
      return std::format("xdec_flagbit({}, 0x{:x})", text(e.operand(0)), e.immediate);
    case il::ExprOp::FlagDef:
      // Only meaningful under a FlagCond or FlagBit; alone it says nothing.
      return "/*flagdef*/0x0";
    case il::ExprOp::FAdd:
    case il::ExprOp::FSub:
    case il::ExprOp::FMul:
    case il::ExprOp::FDiv:
    case il::ExprOp::FNeg:
    case il::ExprOp::FAbs:
    case il::ExprOp::FSqrt:
    case il::ExprOp::FCeil: {
      const std::string name = std::format("{}{}", il::toString(e.op), e.type.bits());
      ctx_.helpers.insert(name);
      std::string call = std::format("xdec_{}(", name);
      for (unsigned index = 0; index < e.operandCount; ++index) {
        call += index == 0 ? "" : ", ";
        call += text(e.operand(index));
      }
      return call + ")";
    }
    case il::ExprOp::FCmpEq:
      return std::format("({} == {})", text(e.operand(0)), text(e.operand(1)));
    case il::ExprOp::FCmpLt:
      return std::format("({} < {})", text(e.operand(0)), text(e.operand(1)));
    case il::ExprOp::FCmpLe:
      return std::format("({} <= {})", text(e.operand(0)), text(e.operand(1)));
    case il::ExprOp::FCmpUnordered: {
      const uint32_t bits = ctx_.function.expr(e.operand(0)).type.bits();
      const std::string name = std::format("fcmp_uno{}", bits);
      ctx_.helpers.insert(name);
      return std::format("xdec_{}({}, {})", name, text(e.operand(0)), text(e.operand(1)));
    }
    case il::ExprOp::FpConvert:
      return std::format("(({})({}))", e.type.bits() == 32 ? "float" : "double",
                         text(e.operand(0)));
    case il::ExprOp::FpToIntS:
      return std::format("(({})({}))", intType(e.type.bits(), true), text(e.operand(0)));
    case il::ExprOp::FpToIntU:
      return std::format("(({})({}))", intType(e.type.bits(), false), text(e.operand(0)));
    case il::ExprOp::IntToFpS: {
      const uint32_t srcBits = ctx_.function.expr(e.operand(0)).type.bits();
      return std::format("(({})({}))", e.type.bits() == 32 ? "float" : "double",
                         signedOperand(e.operand(0), srcBits));
    }
    case il::ExprOp::IntToFpU:
      return std::format("(({})({}))", e.type.bits() == 32 ? "float" : "double",
                         integerOperand(e.operand(0)));
    default:
      return std::format("/*{}?*/0x0", il::toString(e.op));
  }
}

std::string ExprPrinter::infix(il::ExprId id, std::string_view op, bool sign) {
  const il::Expr& e = ctx_.function.expr(id);
  const uint32_t width = e.type.bits();
  const std::string left =
      sign ? signedOperand(e.operand(0), width) : integerOperand(e.operand(0));
  if (op == "+" && !sign) {
    uint64_t rhsConst = 0;
    if (ctx_.function.asConstant(e.operand(1), rhsConst)) {
      if (width == 32 && rhsConst >= 0x80000000ull && rhsConst <= 0xffffffffull) {
        const uint64_t neg = 0x100000000ull - rhsConst;
        return widthWrap(e, std::format("({} - 0x{:x})", left, neg), sign);
      }
      if (width == 64 && static_cast<int64_t>(rhsConst) < 0 &&
          static_cast<int64_t>(rhsConst) >= -0x10000000LL) {
        const uint64_t neg = static_cast<uint64_t>(-static_cast<int64_t>(rhsConst));
        return widthWrap(e, std::format("({} - 0x{:x})", left, neg), sign);
      }
    }
  }
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

std::string ExprPrinter::equalityOperand(
    il::ExprId id, std::optional<uint64_t> comparedConstant) {
  const il::Expr& expr = ctx_.function.expr(id);
  if (comparedConstant.has_value() && expr.op == il::ExprOp::ZExt) {
    const il::Expr& source = ctx_.function.expr(expr.operand(0));
    const uint32_t bits = source.type.isScalarInteger() ? source.type.bits() : 0;
    const bool fits = bits >= 64 || (bits != 0 && *comparedConstant < (uint64_t{1} << bits));
    if (fits) {
      return integerOperand(expr.operand(0));
    }
  }
  return integerOperand(id);
}

/// Comparisons take their width from the operands, not from the i1 result.
std::string ExprPrinter::compare(il::ExprId id, std::string_view op, bool sign) {
  const il::Expr& e = ctx_.function.expr(id);
  const il::Expr& lhs = ctx_.function.expr(e.operand(0));
  const uint32_t width = lhs.type.isScalarInteger() ? lhs.type.bits() : 64;
  if (!sign) {
    if (op == "==" || op == "!=") {
      uint64_t leftConstant = 0;
      uint64_t rightConstant = 0;
      const bool leftIsConstant = ctx_.function.asConstant(e.operand(0), leftConstant);
      const bool rightIsConstant = ctx_.function.asConstant(e.operand(1), rightConstant);
      return std::format(
          "({} {} {})",
          equalityOperand(e.operand(0),
                          rightIsConstant ? std::optional<uint64_t>{rightConstant}
                                          : std::nullopt),
          op,
          equalityOperand(e.operand(1),
                          leftIsConstant ? std::optional<uint64_t>{leftConstant}
                                         : std::nullopt));
    }
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
  // If the inner operand is already a signed variable of `sourceWidth`, C
  // casts it straight to `destination` without a redundant `(int32_t)` step.
  bool alreadySigned = false;
  if (source.op == il::ExprOp::Value) {
    const il::ValueId valueId{static_cast<uint32_t>(source.immediate)};
    if (const analysis::Variable* temp = ctx_.variables.tempFor(valueId);
        temp != nullptr && temp->type.pointerDepth == 0 &&
        temp->type.isSigned && temp->type.width == sourceWidth &&
        !ctx_.typeOfValue(valueId).valid()) {
      alreadySigned = true;
    }
  } else if (source.op == il::ExprOp::EntryReg) {
    const il::RegId root{static_cast<uint32_t>(source.immediate)};
    if (const analysis::Variable* arg = ctx_.variables.argumentFor(root);
        arg != nullptr && arg->type.pointerDepth == 0 &&
        arg->type.isSigned && arg->type.width == sourceWidth &&
        !ctx_.argumentType(*arg).valid()) {
      alreadySigned = true;
    }
  }
  if (alreadySigned) {
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

// ---------------------------------------------------------------------------
// Structured HlExpr lowering (Dual-View AST mode)
// ---------------------------------------------------------------------------

HlExprPtr ExprPrinter::hlStamp(il::ExprId id, HlExprPtr node) const {
  if (!node) {
    return nullptr;
  }
  if (!node->ilExpr.valid()) {
    node->ilExpr = id;
  }
  if (node->widthBits == 0) {
    const il::Type t = ctx_.function.expr(id).type;
    if (t.isScalarInteger() || t.isFloat()) {
      node->widthBits = t.bits();
    }
  }
  return node;
}

HlExprPtr ExprPrinter::toHlExpr(il::ExprId id) { return hlValue(id).expr; }

HlExprPtr ExprPrinter::integerToHlExpr(il::ExprId id) {
  HlNode operand = hlValue(id);
  if (operand.pointer) {
    return hlStamp(id, HlExpr::makeCast("uint64_t", std::move(operand.expr)));
  }
  return std::move(operand.expr);
}

HlExprPtr ExprPrinter::pointerToHlExpr(il::ExprId id, uint32_t width) {
  if (const auto rendered =
          AddressRenderer(ctx_).render(id, AddressRole::PointerValue, width)) {
    if (rendered->pointer) {
      if (rendered->text.size() > 1 && rendered->text[0] == '&') {
        return hlStamp(id, HlExpr::makeAddressOf(HlExpr::makeVar(rendered->text.substr(1))));
      }
      return hlStamp(id, HlExpr::makeRaw(rendered->text));
    }
    return hlStamp(id, HlExpr::makeCast(std::format("{}*", intType(width)),
                                        HlExpr::makeRaw(rendered->text)));
  }
  HlNode operand = hlValue(id);
  if (operand.pointer) {
    return std::move(operand.expr);
  }
  return hlStamp(id, HlExpr::makeCast(std::format("{}*", intType(width)),
                                      std::move(operand.expr)));
}

HlExprPtr ExprPrinter::rootToHlExpr(il::ExprId id) {
  if (const auto cached = materializedText_.find(id.index());
      cached != materializedText_.end()) {
    return hlStamp(id, HlExpr::makeVar(cached->second.text));
  }
  const il::Expr& expr = ctx_.function.expr(id);
  if (!isShared(id, expr) && isRedundantRootZext(expr)) {
    return integerToHlExpr(expr.operand(0));
  }
  return toHlExpr(id);
}

HlExprPtr ExprPrinter::rootIntegerToHlExpr(il::ExprId id) {
  if (const auto cached = materializedText_.find(id.index());
      cached != materializedText_.end()) {
    auto node = HlExpr::makeVar(cached->second.text);
    if (cached->second.pointer) {
      return hlStamp(id, HlExpr::makeCast("uint64_t", std::move(node)));
    }
    return hlStamp(id, std::move(node));
  }
  const il::Expr& expr = ctx_.function.expr(id);
  if (!isShared(id, expr) && isRedundantRootZext(expr)) {
    return integerToHlExpr(expr.operand(0));
  }
  return integerToHlExpr(id);
}

HlExprPtr ExprPrinter::conditionToHlExpr(il::ExprId id, bool invert) {
  if (!invert) {
    return rootToHlExpr(id);
  }
  const il::Expr& expr = ctx_.function.expr(id);
  switch (expr.op) {
    case il::ExprOp::CmpEq:
      return hlCompare(id, "!=", false);
    case il::ExprOp::CmpNe:
      return hlCompare(id, "==", false);
    case il::ExprOp::CmpLtU:
      return hlCompare(id, ">=", false);
    case il::ExprOp::CmpLeU:
      return hlCompare(id, ">", false);
    case il::ExprOp::CmpLtS:
      return hlCompare(id, ">=", true);
    case il::ExprOp::CmpLeS:
      return hlCompare(id, ">", true);
    case il::ExprOp::Not:
      if (expr.type.bits() == 1) {
        return rootToHlExpr(expr.operand(0));
      }
      break;
    default:
      break;
  }
  return hlStamp(id, HlExpr::makeUnary("!", rootToHlExpr(id)));
}

ExprPrinter::HlNode ExprPrinter::hlValue(il::ExprId id) {
  const il::Expr& expr = ctx_.function.expr(id);
  if (expr.op == il::ExprOp::Value) {
    const il::ValueId valueId{static_cast<uint32_t>(expr.immediate)};
    const il::OpId defId =
        ctx_.function.hasValue(valueId) ? ctx_.function.value(valueId).definition : il::OpId{};
    if (const auto snapshot = ctx_.snapshots.find(valueId.index());
        snapshot != ctx_.snapshots.end()) {
      auto node = hlStamp(id, HlExpr::makeVar(snapshot->second));
      node->ilOp = defId;
      return {std::move(node), false};
    }
    if (const auto inlined = ctx_.inlinedStackLoads.find(valueId.index());
        inlined != ctx_.inlinedStackLoads.end()) {
      auto node = hlStamp(id, HlExpr::makeRaw(inlined->second.text));
      node->ilOp = defId;
      return {std::move(node), inlined->second.pointer};
    }
    if (const auto inlined = ctx_.inlinedMemoryLoads.find(valueId.index());
        inlined != ctx_.inlinedMemoryLoads.end()) {
      const il::ExprId address = inlined->second.address;
      const uint32_t width = inlined->second.width;
      if (std::string field = ctx_.fieldAccess(address, width, valueId); !field.empty()) {
        auto node = hlStamp(id, HlExpr::makeRaw(std::move(field)));
        node->ilOp = defId;
        return {std::move(node), false};
      }
      uint64_t absolute = 0;
      if (ctx_.function.asConstant(address, absolute)) {
        if (const std::string* global = ctx_.globalName(absolute)) {
          auto node = hlStamp(id, HlExpr::makeDeref(intType(width), HlExpr::makeVar(*global)));
          node->ilOp = defId;
          return {std::move(node), false};
        }
      }
      auto node = hlStamp(id, HlExpr::makeDeref(intType(width), pointerToHlExpr(address, width)));
      node->ilOp = defId;
      return {std::move(node), false};
    }
    if (const std::string* named = ctx_.tempFor(valueId);
        named != nullptr && ctx_.valueIsPointer(valueId)) {
      auto node = hlStamp(id, HlExpr::makeVar(*named));
      node->ilOp = defId;
      return {std::move(node), true};
    }
    if (const analysis::Variable* temp = ctx_.variables.tempFor(valueId)) {
      auto node = hlStamp(id, HlExpr::makeVar(temp->name));
      node->ilOp = defId;
      return {std::move(node), temp->type.pointerDepth > 0 || ctx_.valueIsPointer(valueId)};
    }
  }
  if (expr.op == il::ExprOp::EntryReg) {
    const il::RegId root{static_cast<uint32_t>(expr.immediate)};
    if (const analysis::Variable* arg = ctx_.variables.argumentFor(root)) {
      return {hlStamp(id, HlExpr::makeVar(ctx_.argumentName(*arg))),
              ctx_.argumentIsPointer(*arg)};
    }
  }
  if ((expr.op == il::ExprOp::Add || expr.op == il::ExprOp::Sub) &&
      expr.type.bits() == 64) {
    if (std::string localAddr = ctx_.addressOfLocal(id); !localAddr.empty()) {
      if (localAddr.size() > 1 && localAddr[0] == '&') {
        return {hlStamp(id, HlExpr::makeAddressOf(HlExpr::makeVar(localAddr.substr(1)))),
                true};
      }
      return {hlStamp(id, HlExpr::makeVar(std::move(localAddr))), true};
    }
  }
  return hlMaterialized(id);
}

ExprPrinter::HlNode ExprPrinter::hlMaterialized(il::ExprId id) {
  if (const auto cached = materializedText_.find(id.index());
      cached != materializedText_.end()) {
    return {hlStamp(id, HlExpr::makeVar(cached->second.text)), cached->second.pointer};
  }
  const il::Expr& expr = ctx_.function.expr(id);
  if (expr.operandCount != 0 && !referenceCounts_.contains(id.index())) {
    countReferences(id);
  }
  if (!isShared(id, expr)) {
    return {hlInner(id), false};
  }
  const std::string name = std::format("_cse{}", cseCounter_++);
  bool pointer = false;
  HlExprPtr initAst;
  std::string initText;
  if (const auto rendered =
          AddressRenderer(ctx_).render(id, AddressRole::PointerValue, 8);
      rendered.has_value() && rendered->pointer) {
    pointer = true;
    initText = rendered->text;
    initAst = (initText.size() > 1 && initText[0] == '&')
                  ? hlStamp(id, HlExpr::makeAddressOf(HlExpr::makeVar(initText.substr(1))))
                  : hlStamp(id, HlExpr::makeRaw(initText));
  } else {
    initAst = isRedundantRootZext(expr) ? integerToHlExpr(expr.operand(0)) : hlInner(id);
    // Keep text fallback consistent for legacy readers.
    initText = initAst ? initAst->text : std::string{};
  }
  ctx_.cseTemps.push_back(CseTemp{name, expr.type.bits(), pointer});
  pendingDecls_.push_back(PendingCseDecl{
      name, initText.empty() ? std::string{} : std::format("{} = {};", name, initText),
      std::move(initAst)});
  const Text result{name, pointer};
  materializedText_.emplace(id.index(), result);
  return {hlStamp(id, HlExpr::makeVar(name)), pointer};
}

HlExprPtr ExprPrinter::hlWidthWrap(const il::Expr& expr, HlExprPtr node, bool sign) {
  const uint32_t width = expr.type.bits();
  if (width >= 32 || !expr.type.isScalarInteger()) {
    return node;
  }
  return HlExpr::makeCast(intType(width, sign), std::move(node));
}

HlExprPtr ExprPrinter::hlSignedOperand(il::ExprId id, uint32_t width) {
  uint64_t constant = 0;
  if (width <= 32 && ctx_.function.asConstant(id, constant)) {
    const uint64_t signBit = uint64_t{1} << (width - 1);
    if (constant < signBit) {
      return hlStamp(id, HlExpr::makeConst(constant));
    }
  }
  const il::Expr& expr = ctx_.function.expr(id);
  if (expr.op == il::ExprOp::Value) {
    const il::ValueId valueId{static_cast<uint32_t>(expr.immediate)};
    if (const analysis::Variable* temp = ctx_.variables.tempFor(valueId);
        temp != nullptr && temp->type.pointerDepth == 0 &&
        temp->type.isSigned && temp->type.width == width &&
        !ctx_.typeOfValue(valueId).valid()) {
      auto node = hlStamp(id, HlExpr::makeVar(temp->name));
      node->ilOp = ctx_.function.value(valueId).definition;
      return node;
    }
  }
  return hlStamp(id, HlExpr::makeCast(intType(width, true), integerToHlExpr(id)));
}

HlExprPtr ExprPrinter::hlInfix(il::ExprId id, std::string_view op, bool sign) {
  const il::Expr& e = ctx_.function.expr(id);
  const uint32_t width = e.type.bits();
  HlExprPtr left =
      sign ? hlSignedOperand(e.operand(0), width) : integerToHlExpr(e.operand(0));
  if (op == "+" && !sign) {
    uint64_t rhsConst = 0;
    if (ctx_.function.asConstant(e.operand(1), rhsConst)) {
      if (width == 32 && rhsConst >= 0x80000000ull && rhsConst <= 0xffffffffull) {
        const uint64_t neg = 0x100000000ull - rhsConst;
        return hlStamp(id, hlWidthWrap(
                               e, HlExpr::makeBinary("-", std::move(left), HlExpr::makeConst(neg)),
                               sign));
      }
      if (width == 64 && static_cast<int64_t>(rhsConst) < 0 &&
          static_cast<int64_t>(rhsConst) >= -0x10000000LL) {
        const uint64_t neg = static_cast<uint64_t>(-static_cast<int64_t>(rhsConst));
        return hlStamp(id, hlWidthWrap(
                               e, HlExpr::makeBinary("-", std::move(left), HlExpr::makeConst(neg)),
                               sign));
      }
    }
  }
  HlExprPtr right =
      sign ? hlSignedOperand(e.operand(1), width) : integerToHlExpr(e.operand(1));
  return hlStamp(id, hlWidthWrap(
                         e, HlExpr::makeBinary(std::string{op}, std::move(left), std::move(right)),
                         sign));
}

HlExprPtr ExprPrinter::hlShift(il::ExprId id, std::string_view op) {
  const il::Expr& e = ctx_.function.expr(id);
  const uint32_t width = e.type.bits();
  const il::Expr& source = ctx_.function.expr(e.operand(0));
  HlExprPtr left =
      (source.type.isScalarInteger() && source.type.bits() == width)
          ? integerToHlExpr(e.operand(0))
          : hlStamp(e.operand(0), HlExpr::makeCast(intType(width), integerToHlExpr(e.operand(0))));
  return hlStamp(id, hlWidthWrap(e,
                                 HlExpr::makeBinary(std::string{op}, std::move(left),
                                                    integerToHlExpr(e.operand(1))),
                                 false));
}

HlExprPtr ExprPrinter::hlEqualityOperand(
    il::ExprId id, std::optional<uint64_t> comparedConstant) {
  const il::Expr& expr = ctx_.function.expr(id);
  if (comparedConstant.has_value() && expr.op == il::ExprOp::ZExt) {
    const il::Expr& source = ctx_.function.expr(expr.operand(0));
    const uint32_t bits = source.type.isScalarInteger() ? source.type.bits() : 0;
    const bool fits = bits >= 64 || (bits != 0 && *comparedConstant < (uint64_t{1} << bits));
    if (fits) {
      return integerToHlExpr(expr.operand(0));
    }
  }
  return integerToHlExpr(id);
}

HlExprPtr ExprPrinter::hlCompare(il::ExprId id, std::string_view op, bool sign) {
  const il::Expr& e = ctx_.function.expr(id);
  const il::Expr& lhs = ctx_.function.expr(e.operand(0));
  const uint32_t width = lhs.type.isScalarInteger() ? lhs.type.bits() : 64;
  if (!sign) {
    if (op == "==" || op == "!=") {
      uint64_t leftConstant = 0;
      uint64_t rightConstant = 0;
      const bool leftIsConstant = ctx_.function.asConstant(e.operand(0), leftConstant);
      const bool rightIsConstant = ctx_.function.asConstant(e.operand(1), rightConstant);
      return hlStamp(
          id,
          HlExpr::makeBinary(
              std::string{op},
              hlEqualityOperand(e.operand(0),
                                rightIsConstant ? std::optional<uint64_t>{rightConstant}
                                                : std::nullopt),
              hlEqualityOperand(e.operand(1),
                                leftIsConstant ? std::optional<uint64_t>{leftConstant}
                                               : std::nullopt)));
    }
    return hlStamp(id, HlExpr::makeBinary(std::string{op}, integerToHlExpr(e.operand(0)),
                                          integerToHlExpr(e.operand(1))));
  }
  return hlStamp(id, HlExpr::makeBinary(std::string{op}, hlSignedOperand(e.operand(0), width),
                                        hlSignedOperand(e.operand(1), width)));
}

HlExprPtr ExprPrinter::hlSignExtend(il::ExprId id) {
  const il::Expr& e = ctx_.function.expr(id);
  const il::Expr& source = ctx_.function.expr(e.operand(0));
  const std::string destination = intType(e.type.bits(), true);
  const uint32_t sourceWidth =
      source.type.isScalarInteger() ? source.type.bits() : 64;
  if (sourceWidth == 1) {
    return hlStamp(id, HlExpr::makeUnary("-", HlExpr::makeCast(destination, toHlExpr(e.operand(0)))));
  }
  if (sourceWidth >= e.type.bits()) {
    return hlStamp(id, HlExpr::makeCast(destination, integerToHlExpr(e.operand(0))));
  }
  bool alreadySigned = false;
  if (source.op == il::ExprOp::Value) {
    const il::ValueId valueId{static_cast<uint32_t>(source.immediate)};
    if (const analysis::Variable* temp = ctx_.variables.tempFor(valueId);
        temp != nullptr && temp->type.pointerDepth == 0 &&
        temp->type.isSigned && temp->type.width == sourceWidth &&
        !ctx_.typeOfValue(valueId).valid()) {
      alreadySigned = true;
    }
  } else if (source.op == il::ExprOp::EntryReg) {
    const il::RegId root{static_cast<uint32_t>(source.immediate)};
    if (const analysis::Variable* arg = ctx_.variables.argumentFor(root);
        arg != nullptr && arg->type.pointerDepth == 0 &&
        arg->type.isSigned && arg->type.width == sourceWidth &&
        !ctx_.argumentType(*arg).valid()) {
      alreadySigned = true;
    }
  }
  if (alreadySigned) {
    return hlStamp(id, HlExpr::makeCast(destination, integerToHlExpr(e.operand(0))));
  }
  return hlStamp(
      id, HlExpr::makeCast(destination,
                           HlExpr::makeCast(intType(sourceWidth, true),
                                            integerToHlExpr(e.operand(0)))));
}

HlExprPtr ExprPrinter::hlInner(il::ExprId id) {
  const il::Expr& e = ctx_.function.expr(id);
  switch (e.op) {
    case il::ExprOp::Const:
      if (e.type.bits() == 64 && e.immediate >= 0x10000 && ctx_.options.addresses) {
        const AddressFacts facts = ctx_.options.addresses(e.immediate);
        if (facts.cstringSection) {
          if (const std::optional<analysis::ImageLiteral> literal =
                  ctx_.literals.at(e.immediate, /*allowEmpty=*/true)) {
            return hlStamp(id, HlExpr::makeStringLit(literal->text));
          }
        }
        if (facts.cfstringLiteral.has_value()) {
          const std::string* global = ctx_.globalName(e.immediate);
          const std::string base =
              global != nullptr ? *global : std::format("0x{:x}", e.immediate);
          return hlStamp(id, HlExpr::makeCFStringLit(base, *facts.cfstringLiteral));
        }
      }
      return hlStamp(id, HlExpr::makeConst(e.immediate));
    case il::ExprOp::Value: {
      const il::ValueId valueId{static_cast<uint32_t>(e.immediate)};
      if (const std::string* temp = ctx_.tempFor(valueId)) {
        auto node = hlStamp(id, HlExpr::makeVar(*temp));
        if (ctx_.function.hasValue(valueId)) {
          node->ilOp = ctx_.function.value(valueId).definition;
        }
        return node;
      }
      return hlStamp(id, HlExpr::makeRaw(std::format("/*unnamed-value-%{}*/0x0", valueId.index())));
    }
    case il::ExprOp::Undef:
      return hlStamp(id, HlExpr::makeRaw("/*undef*/0x0"));
    case il::ExprOp::EntryReg: {
      const il::RegId root{static_cast<uint32_t>(e.immediate)};
      if (const analysis::Variable* arg = ctx_.variables.argumentFor(root)) {
        return hlStamp(id, HlExpr::makeVar(ctx_.argumentName(*arg)));
      }
      const il::RegisterInfo& info = ctx_.function.registers()[root];
      if (info.name == "x8") {
        ctx_.hasSret = true;
        return hlStamp(id, HlExpr::makeVar("_sret"));
      }
      const std::string name = std::format("__entry_{}", info.name);
      ctx_.entryLeaves.emplace(name, info.bits == 0 ? 64 : info.bits);
      if (ctx_.options.entryRegs != nullptr) {
        if (const std::optional<uint64_t> resolved =
                ctx_.options.entryRegs->resolve(info.name)) {
          ctx_.anchoredEntryLeaves.emplace(name, *resolved);
        }
      }
      return hlStamp(id, HlExpr::makeVar(name));
    }
    case il::ExprOp::Add: return hlInfix(id, "+", false);
    case il::ExprOp::Sub: return hlInfix(id, "-", false);
    case il::ExprOp::Mul: return hlInfix(id, "*", false);
    case il::ExprOp::And: return hlInfix(id, "&", false);
    case il::ExprOp::Or: return hlInfix(id, "|", false);
    case il::ExprOp::Xor: return hlInfix(id, "^", false);
    case il::ExprOp::Shl: return hlShift(id, "<<");
    case il::ExprOp::ShrU: return hlShift(id, ">>");
    case il::ExprOp::ShrS:
      return hlStamp(
          id, hlWidthWrap(e,
                          HlExpr::makeBinary(">>", hlSignedOperand(e.operand(0), e.type.bits()),
                                             integerToHlExpr(e.operand(1))),
                          true));
    case il::ExprOp::DivU: return hlInfix(id, "/", false);
    case il::ExprOp::RemU: return hlInfix(id, "%", false);
    case il::ExprOp::DivS: return hlInfix(id, "/", true);
    case il::ExprOp::RemS: return hlInfix(id, "%", true);
    case il::ExprOp::MulHiU:
    case il::ExprOp::MulHiS: {
      const bool sign = e.op == il::ExprOp::MulHiS;
      const uint32_t width = e.type.bits();
      ctx_.helpers.insert(std::format("mulhi{}{}", sign ? "s" : "u", width));
      std::vector<HlExprPtr> args;
      args.push_back(integerToHlExpr(e.operand(0)));
      args.push_back(integerToHlExpr(e.operand(1)));
      auto call = HlExpr::makeCall(
          HlExpr::makeVar(std::format("xdec_mulhi{}{}", sign ? "s" : "u", width)),
          std::move(args));
      call->isDirectCall = true;
      return hlStamp(id, std::move(call));
    }
    case il::ExprOp::RotR:
    case il::ExprOp::RotL: {
      const bool right = e.op == il::ExprOp::RotR;
      const uint32_t width = e.type.bits();
      ctx_.helpers.insert(std::format("rot{}{}", right ? "r" : "l", width));
      std::vector<HlExprPtr> args;
      args.push_back(integerToHlExpr(e.operand(0)));
      args.push_back(integerToHlExpr(e.operand(1)));
      auto call = HlExpr::makeCall(
          HlExpr::makeVar(std::format("rot{}{}", right ? "r" : "l", width)),
          std::move(args));
      call->isDirectCall = true;
      return hlStamp(id, std::move(call));
    }
    case il::ExprOp::Ctz: {
      ctx_.helpers.insert(std::format("ctz{}", e.type.bits()));
      std::vector<HlExprPtr> args;
      args.push_back(integerToHlExpr(e.operand(0)));
      auto call = HlExpr::makeCall(
          HlExpr::makeVar(std::format("xdec_ctz{}", e.type.bits())), std::move(args));
      call->isDirectCall = true;
      return hlStamp(id, std::move(call));
    }
    case il::ExprOp::Bitcast: {
      if (e.type.isFloat()) {
        uint64_t bits = 0;
        if (ctx_.function.asConstant(e.operand(0), bits)) {
          if (e.type.bits() == 64) {
            const double d = std::bit_cast<double>(bits);
            if (std::isfinite(d)) {
              std::string s = std::format("{}", d);
              if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
                  s.find('E') == std::string::npos) {
                s += ".0";
              }
              return hlStamp(id, HlExpr::makeFloatLit(std::move(s), d, 64));
            }
          } else if (e.type.bits() == 32) {
            const float f = std::bit_cast<float>(static_cast<uint32_t>(bits));
            if (std::isfinite(f)) {
              std::string s = std::format("{}", f);
              if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
                  s.find('E') == std::string::npos) {
                s += ".0";
              }
              return hlStamp(id, HlExpr::makeFloatLit(s + "f", static_cast<double>(f), 32));
            }
          }
        }
        return hlStamp(id, HlExpr::makeCast(e.type.bits() == 32 ? "float" : "double",
                                            integerToHlExpr(e.operand(0))));
      }
      return hlStamp(id, HlExpr::makeCast(intType(e.type.bits()), integerToHlExpr(e.operand(0))));
    }
    case il::ExprOp::Concat: {
      const uint32_t lowWidth = ctx_.function.expr(e.operand(1)).type.bits();
      const std::string tname = intType(e.type.bits());
      auto hi = HlExpr::makeBinary(
          "<<", HlExpr::makeCast(tname, integerToHlExpr(e.operand(0))),
          HlExpr::makeConst(lowWidth));
      auto lo = HlExpr::makeCast(tname, integerToHlExpr(e.operand(1)));
      return hlStamp(id, HlExpr::makeBinary("|", std::move(hi), std::move(lo)));
    }
    case il::ExprOp::Neg:
      return hlStamp(
          id, hlWidthWrap(e, HlExpr::makeUnary("-", integerToHlExpr(e.operand(0))), false));
    case il::ExprOp::Not:
      if (e.type.bits() == 1) {
        return hlStamp(id, HlExpr::makeUnary("!", toHlExpr(e.operand(0))));
      }
      return hlStamp(
          id, hlWidthWrap(e, HlExpr::makeUnary("~", integerToHlExpr(e.operand(0))), false));
    case il::ExprOp::CmpEq: return hlCompare(id, "==", false);
    case il::ExprOp::CmpNe: return hlCompare(id, "!=", false);
    case il::ExprOp::CmpLtU: return hlCompare(id, "<", false);
    case il::ExprOp::CmpLeU: return hlCompare(id, "<=", false);
    case il::ExprOp::CmpLtS: return hlCompare(id, "<", true);
    case il::ExprOp::CmpLeS: return hlCompare(id, "<=", true);
    case il::ExprOp::ZExt:
    case il::ExprOp::Trunc: {
      const il::Expr& source = ctx_.function.expr(e.operand(0));
      if (source.type.isScalarInteger() && source.type.bits() == e.type.bits()) {
        return integerToHlExpr(e.operand(0));
      }
      if (source.op == il::ExprOp::EntryReg) {
        const il::RegId root{static_cast<uint32_t>(source.immediate)};
        if (const analysis::Variable* arg = ctx_.variables.argumentFor(root);
            arg != nullptr && arg->type.pointerDepth == 0 &&
            arg->type.width == e.type.bits() && !ctx_.argumentType(*arg).valid()) {
          return integerToHlExpr(e.operand(0));
        }
      }
      return hlStamp(id, HlExpr::makeCast(intType(e.type.bits()), integerToHlExpr(e.operand(0))));
    }
    case il::ExprOp::SExt:
      return hlSignExtend(id);
    case il::ExprOp::Select:
      return hlStamp(id, HlExpr::makeSelect(toHlExpr(e.operand(0)), toHlExpr(e.operand(1)),
                                            toHlExpr(e.operand(2))));
    case il::ExprOp::Extract: {
      const uint32_t width = e.type.bits();
      if (e.immediate == 0) {
        return hlStamp(id, HlExpr::makeCast(intType(width), integerToHlExpr(e.operand(0))));
      }
      return hlStamp(
          id, HlExpr::makeCast(
                  intType(width),
                  HlExpr::makeBinary(">>", integerToHlExpr(e.operand(0)),
                                     HlExpr::makeConst(e.immediate))));
    }
    case il::ExprOp::ByteSwap:
    case il::ExprOp::BitReverse:
    case il::ExprOp::PopCount:
    case il::ExprOp::Clz: {
      std::string key;
      std::string fn;
      if (e.op == il::ExprOp::ByteSwap) {
        key = std::format("bswap{}", e.type.bits());
        fn = key;
      } else if (e.op == il::ExprOp::BitReverse) {
        key = std::format("brev{}", e.type.bits());
        fn = "xdec_" + key;
      } else if (e.op == il::ExprOp::PopCount) {
        key = "popcount64";
        fn = key;
      } else {
        key = std::format("clz{}", e.type.bits());
        fn = "xdec_" + key;
      }
      ctx_.helpers.insert(std::move(key));
      std::vector<HlExprPtr> args;
      args.push_back(integerToHlExpr(e.operand(0)));
      auto call = HlExpr::makeCall(HlExpr::makeVar(std::move(fn)), std::move(args));
      call->isDirectCall = true;
      return hlStamp(id, std::move(call));
    }
    case il::ExprOp::FlagCond:
      return hlStamp(id, HlExpr::makeRaw(printFlagCond(ctx_, *this, id)));
    case il::ExprOp::FlagBit: {
      ctx_.helpers.insert("flagbit");
      std::vector<HlExprPtr> args;
      args.push_back(toHlExpr(e.operand(0)));
      args.push_back(HlExpr::makeConst(e.immediate));
      auto call = HlExpr::makeCall(HlExpr::makeVar("xdec_flagbit"), std::move(args));
      call->isDirectCall = true;
      return hlStamp(id, std::move(call));
    }
    case il::ExprOp::FlagDef:
      return hlStamp(id, HlExpr::makeRaw("/*flagdef*/0x0"));
    case il::ExprOp::FAdd:
    case il::ExprOp::FSub:
    case il::ExprOp::FMul:
    case il::ExprOp::FDiv:
    case il::ExprOp::FNeg:
    case il::ExprOp::FAbs:
    case il::ExprOp::FSqrt:
    case il::ExprOp::FCeil: {
      const std::string name = std::format("{}{}", il::toString(e.op), e.type.bits());
      ctx_.helpers.insert(name);
      std::vector<HlExprPtr> args;
      args.reserve(e.operandCount);
      for (unsigned index = 0; index < e.operandCount; ++index) {
        args.push_back(toHlExpr(e.operand(index)));
      }
      auto call =
          HlExpr::makeCall(HlExpr::makeVar(std::format("xdec_{}", name)), std::move(args));
      call->isDirectCall = true;
      return hlStamp(id, std::move(call));
    }
    case il::ExprOp::FCmpEq:
      return hlStamp(id, HlExpr::makeBinary("==", toHlExpr(e.operand(0)), toHlExpr(e.operand(1))));
    case il::ExprOp::FCmpLt:
      return hlStamp(id, HlExpr::makeBinary("<", toHlExpr(e.operand(0)), toHlExpr(e.operand(1))));
    case il::ExprOp::FCmpLe:
      return hlStamp(id, HlExpr::makeBinary("<=", toHlExpr(e.operand(0)), toHlExpr(e.operand(1))));
    case il::ExprOp::FCmpUnordered: {
      const uint32_t bits = ctx_.function.expr(e.operand(0)).type.bits();
      const std::string name = std::format("fcmp_uno{}", bits);
      ctx_.helpers.insert(name);
      std::vector<HlExprPtr> args;
      args.push_back(toHlExpr(e.operand(0)));
      args.push_back(toHlExpr(e.operand(1)));
      auto call =
          HlExpr::makeCall(HlExpr::makeVar(std::format("xdec_{}", name)), std::move(args));
      call->isDirectCall = true;
      return hlStamp(id, std::move(call));
    }
    case il::ExprOp::FpConvert:
      return hlStamp(id, HlExpr::makeCast(e.type.bits() == 32 ? "float" : "double",
                                          toHlExpr(e.operand(0))));
    case il::ExprOp::FpToIntS:
      return hlStamp(id, HlExpr::makeCast(intType(e.type.bits(), true), toHlExpr(e.operand(0))));
    case il::ExprOp::FpToIntU:
      return hlStamp(id, HlExpr::makeCast(intType(e.type.bits(), false), toHlExpr(e.operand(0))));
    case il::ExprOp::IntToFpS: {
      const uint32_t srcBits = ctx_.function.expr(e.operand(0)).type.bits();
      return hlStamp(id, HlExpr::makeCast(e.type.bits() == 32 ? "float" : "double",
                                          hlSignedOperand(e.operand(0), srcBits)));
    }
    case il::ExprOp::IntToFpU:
      return hlStamp(id, HlExpr::makeCast(e.type.bits() == 32 ? "float" : "double",
                                          integerToHlExpr(e.operand(0))));
    default:
      return hlStamp(id, HlExpr::makeRaw(std::format("/*{}?*/0x0", il::toString(e.op))));
  }
}

}  // namespace xdec::emit
