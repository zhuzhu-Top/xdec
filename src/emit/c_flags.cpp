// printFlagCond (see the header for the stance on unresolved defs).
#include "c_flags.h"

#include <format>
#include <string_view>

#include "c_expr.h"

namespace xdec::emit {

namespace {

/// The overflow-exact conditions go to xdec_helpers.h's `cc_<cond><width>`:
/// N and V from a subtraction cannot be spelled as one C comparison without
/// repeating the operands, and repeating them would double any
/// side-effect-free but large expression at every use.
///
/// The header only defines the six conditions real code actually asks for
/// here (ge/lt/gt/le/vs/vc — see printFlagCond's Sub/Logical/Add cases).
/// Always/Never reaching this point would mean a condition that does not
/// depend on flags at all showed up as one anyway: nothing upstream should
/// produce that, so rather than call a function the header does not
/// declare, it falls back to the same labelled stub an entirely unmodelled
/// flagOp gets below.
[[nodiscard]] std::string ccHelper(CContext& context, il::ConditionCode code,
                                   unsigned width, const std::string& a,
                                   const std::string& b) {
  const std::string_view name = il::toString(code);
  constexpr std::string_view kKnown[] = {"ge", "lt", "gt", "le", "vs", "vc"};
  bool known = false;
  for (const std::string_view candidate : kKnown) {
    if (candidate == name) {
      known = true;
      break;
    }
  }
  if (!known) {
    context.helpers.insert("flagcond_stub");
    return std::format("xdec_flagcond_stub(/*{}*/ {}, {})", name, a, b);
  }
  context.helpers.insert(std::format("cc_{}{}", name, width));
  return std::format("cc_{}{}({}, {})", name, width, a, b);
}

}  // namespace

std::string flagFromNzcv(il::ConditionCode code, uint64_t nzcv) {
  const bool n = ((nzcv >> 3) & 1) != 0;
  const bool z = ((nzcv >> 2) & 1) != 0;
  const bool c = ((nzcv >> 1) & 1) != 0;
  const bool v = (nzcv & 1) != 0;
  bool result = false;
  switch (code) {
    case il::ConditionCode::Equal: result = z; break;
    case il::ConditionCode::NotEqual: result = !z; break;
    case il::ConditionCode::CarrySet: result = c; break;
    case il::ConditionCode::CarryClear: result = !c; break;
    case il::ConditionCode::Negative: result = n; break;
    case il::ConditionCode::NonNegative: result = !n; break;
    case il::ConditionCode::Overflow: result = v; break;
    case il::ConditionCode::NoOverflow: result = !v; break;
    case il::ConditionCode::UnsignedGreater: result = c && !z; break;
    case il::ConditionCode::UnsignedLessEqual: result = !c || z; break;
    case il::ConditionCode::SignedGreaterEqual: result = n == v; break;
    case il::ConditionCode::SignedLess: result = n != v; break;
    case il::ConditionCode::SignedGreater: result = !z && (n == v); break;
    case il::ConditionCode::SignedLessEqual: result = z || (n != v); break;
    case il::ConditionCode::Always: result = true; break;
    case il::ConditionCode::Never:
    case il::ConditionCode::Count: break;
  }
  return result ? "1" : "0";
}

std::string printFlagCond(CContext& context, ExprPrinter& expressions,
                          il::ExprId id) {
  const il::Expr& e = context.function.expr(id);
  const auto code = static_cast<il::ConditionCode>(e.immediate);
  const il::Expr& def = context.function.expr(e.operand(0));
  if (def.op != il::ExprOp::FlagDef) {
    return "/*flagcond*/0";
  }
  const il::FlagOp flagOp = il::flagDefOp(def.immediate);
  const unsigned width = il::flagDefWidth(def.immediate);
  if (flagOp == il::FlagOp::Const) {
    // Literal NZCV in bits 3..0 of the constant operand.
    const il::Expr& bundle = context.function.expr(def.operand(0));
    if (bundle.op == il::ExprOp::Const) {
      return flagFromNzcv(code, bundle.immediate & 0xF);
    }
    return "/*flagbundle*/0";
  }
  const std::string a =
      def.operandCount >= 1 ? expressions.integerOperand(def.operand(0)) : "0";
  const std::string b =
      def.operandCount >= 2 ? expressions.integerOperand(def.operand(1)) : "0";
  const std::string u = intType(width);
  const std::string s = intType(width, true);
  switch (flagOp) {
    case il::FlagOp::Sub:
      switch (code) {
        case il::ConditionCode::Equal:
          return std::format("({} == {})", a, b);
        case il::ConditionCode::NotEqual:
          return std::format("({} != {})", a, b);
        case il::ConditionCode::CarrySet:
          return std::format("(({}){} >= ({}){})", u, a, u, b);
        case il::ConditionCode::CarryClear:
          return std::format("(({}){} < ({}){})", u, a, u, b);
        case il::ConditionCode::UnsignedGreater:
          return std::format("(({}){} > ({}){})", u, a, u, b);
        case il::ConditionCode::UnsignedLessEqual:
          return std::format("(({}){} <= ({}){})", u, a, u, b);
        case il::ConditionCode::Negative:
          return std::format("(({})(({}){} - ({}){}) < 0)", s, u, a, u, b);
        case il::ConditionCode::NonNegative:
          return std::format("(({})(({}){} - ({}){}) >= 0)", s, u, a, u, b);
        default:
          return ccHelper(context, code, width, a, b);
      }
    case il::FlagOp::Logical:
      switch (code) {
        case il::ConditionCode::Equal:
          return std::format("({} == 0)", a);
        case il::ConditionCode::NotEqual:
          return std::format("({} != 0)", a);
        case il::ConditionCode::Negative:
          return std::format("(({}){} < 0)", s, a);
        case il::ConditionCode::NonNegative:
          return std::format("(({}){} >= 0)", s, a);
        case il::ConditionCode::CarrySet:
        case il::ConditionCode::Overflow:
          return "0 /*C,V=0*/";
        case il::ConditionCode::CarryClear:
        case il::ConditionCode::NoOverflow:
          return "1 /*C,V=0*/";
        default:
          return ccHelper(context, code, width, a, "0");
      }
    case il::FlagOp::Add:
      switch (code) {
        case il::ConditionCode::Equal:
          return std::format("(({})({} + {}) == 0)", u, a, b);
        case il::ConditionCode::NotEqual:
          return std::format("(({})({} + {}) != 0)", u, a, b);
        case il::ConditionCode::Negative:
          return std::format("(({})(({})({} + {})) < 0)", s, u, a, b);
        case il::ConditionCode::NonNegative:
          return std::format("(({})(({})({} + {})) >= 0)", s, u, a, b);
        default:
          return ccHelper(context, code, width, a, b);
      }
    default:
      context.helpers.insert("flagcond_stub");
      return std::format("xdec_flagcond_stub(/*{}*/ {}, {})",
                         il::toString(code), a, b);
  }
}

}  // namespace xdec::emit
