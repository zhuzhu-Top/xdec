#include "xdec/il/maturity.h"

namespace xdec::il {

std::string_view toString(Maturity maturity) noexcept {
  switch (maturity) {
    case Maturity::Lifted:
      return "lifted";
    case Maturity::Local:
      return "local";
    case Maturity::Cfg:
      return "cfg";
    case Maturity::Ssa:
      return "ssa";
    case Maturity::Resolved:
      return "resolved";
    case Maturity::Optimized:
      return "optimized";
    case Maturity::Vars:
      return "vars";
    case Maturity::Structured:
      return "structured";
    case Maturity::Typed:
      return "typed";
  }
  return "unknown";
}

bool parseMaturity(std::string_view text, Maturity& out) noexcept {
  if (text == "lifted") {
    out = Maturity::Lifted;
  } else if (text == "local") {
    out = Maturity::Local;
  } else if (text == "cfg") {
    out = Maturity::Cfg;
  } else if (text == "ssa") {
    out = Maturity::Ssa;
  } else if (text == "resolved") {
    out = Maturity::Resolved;
  } else if (text == "optimized") {
    out = Maturity::Optimized;
  } else if (text == "vars") {
    out = Maturity::Vars;
  } else if (text == "structured") {
    out = Maturity::Structured;
  } else if (text == "typed") {
    out = Maturity::Typed;
  } else {
    return false;
  }
  return true;
}

std::string_view describe(Maturity maturity) noexcept {
  switch (maturity) {
    case Maturity::Lifted:
      return "one-to-one with machine instructions; block-local values; nothing simplified";
    case Maturity::Local:
      return "block-local folding and dead value elimination done";
    case Maturity::Cfg:
      return "direct control flow complete; edges consistent; unresolved exits marked";
    case Maturity::Ssa:
      return "static single assignment over registers and memory";
    case Maturity::Resolved:
      return "indirect branches and calls resolved or explicitly recorded as unresolvable";
    case Maturity::Optimized:
      return "constant and copy propagation, dead code elimination and algebraic "
             "simplification at a fixed point";
    case Maturity::Vars:
      return "stack slots and registers promoted to variables; calling convention known";
    case Maturity::Structured:
      return "control flow structured into a high-level AST";
    case Maturity::Typed:
      return "types inferred";
  }
  return "unknown";
}

}  // namespace xdec::il
