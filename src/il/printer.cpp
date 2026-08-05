#include "xdec/il/printer.h"

#include <format>

#include "xdec/support/bits.h"

namespace xdec::il {
namespace {

/// Constants print as signed hexadecimal when the value looks like a small
/// negative number in its declared width, because `-0x60` is far easier to
/// recognise in a stack offset than `0xffffffffffffffa0`.
std::string formatConstant(uint64_t value, Type type) {
  const unsigned width = type.bits();
  if (type.isInteger() && width > 1 && width <= 64) {
    const int64_t signedValue = signExtend(value, width);
    if (signedValue < 0 && signedValue > -0x10000) {
      return std::format("-0x{:x}", -static_cast<uint64_t>(signedValue));
    }
  }
  return std::format("0x{:x}", value);
}

/// A note is one line of prose (see Function::annotate), so the only characters
/// that need escaping are the two that would otherwise end the string early or
/// swallow the next one.
std::string escapeNote(std::string_view note) {
  std::string out;
  out.reserve(note.size());
  for (const char character : note) {
    if (character == '\\' || character == '"') {
      out += '\\';
    }
    out += character;
  }
  return out;
}

class Printer {
 public:
  Printer(const Function& function, const PrintOptions& options)
      : function_(function), options_(options) {}

  void printFunction() {
    out_ += std::format("function @0x{:x} name=\"{}\" arch={} maturity={} {{\n",
                        function_.entryVa(), function_.name(), toString(function_.arch()),
                        toString(function_.maturity()));
    for (const BlockId blockId : function_.blockHandles()) {
      printBlockBody(blockId, 1);
    }
    out_ += "}\n";
  }

  void printBlockBody(BlockId blockId, unsigned depth) {
    const Block& block = function_.block(blockId);
    indent(depth);
    out_ += std::format("block b{} @0x{:x}..0x{:x}", blockId.index(), block.va, block.endVa);
    if (blockId == function_.entryBlock()) {
      out_ += " entry";
    }
    if (block.external) {
      out_ += " external";
    }
    if (options_.includePredecessors) {
      out_ += " preds=[";
      for (std::size_t index = 0; index < block.predecessors.size(); ++index) {
        out_ += index == 0 ? "" : ", ";
        out_ += std::format("b{}", block.predecessors[index].index());
      }
      out_ += "]";
    }
    out_ += " {\n";

    // Tracked separately from the address so that the first op in a block always
    // gets a marker, even when it has no source address: `@none` must be visible
    // rather than implied by its absence.
    bool haveMarker = false;
    uint64_t currentAddress = kNoOpAddress;
    for (const OpId opId : block.ops) {
      const Op& op = function_.op(opId);
      if (!haveMarker || op.va != currentAddress) {
        haveMarker = true;
        currentAddress = op.va;
        indent(depth + 1);
        if (currentAddress == kNoOpAddress) {
          out_ += "@none\n";
        } else {
          out_ += std::format("@0x{:x}\n", currentAddress);
        }
      }
      indent(depth + 1);
      appendOp(opId);
      out_ += "\n";
    }

    indent(depth);
    out_ += "}\n";
  }

  void appendOp(OpId opId) {
    const Op& op = function_.op(opId);

    // Keyed on the result rather than on the opcode's flags, because a non-void
    // intrinsic defines a value while a void one does not.
    if (op.result.valid()) {
      out_ += std::format("%{} = ", op.result.index());
    }
    out_ += toString(op.code);

    // A result type suffix is only meaningful where the opcode does not already
    // determine it.
    switch (op.code) {
      case OpCode::Load:
      case OpCode::Store:
      case OpCode::Phi:
        out_ += std::format(":{}", op.type.toString());
        break;
      case OpCode::Call:
        // The type suffix names the result register's type; bare otherwise.
        if (op.result.valid()) {
          out_ += std::format(":{}", op.type.toString());
        }
        break;
      case OpCode::Intrinsic:
        if (!op.type.isVoid()) {
          out_ += std::format(":{}", op.type.toString());
        }
        break;
      default:
        break;
    }

    const std::span<const ExprId> operands = function_.operands(op);
    const std::span<const BlockId> targets = function_.targets(op);

    switch (op.code) {
      case OpCode::ReadReg:
        out_ += std::format(" {}", function_.registers().nameOf(op.reg()));
        break;
      case OpCode::WriteReg:
        out_ += std::format(" {}, ", function_.registers().nameOf(op.reg()));
        appendExpr(operands[0]);
        break;
      case OpCode::Load:
        out_ += " ";
        appendExpr(operands[0]);
        break;
      case OpCode::Store:
        out_ += " ";
        appendExpr(operands[0]);
        out_ += ", ";
        appendExpr(operands[1]);
        break;
      case OpCode::Branch:
        out_ += std::format(" b{}", targets[0].index());
        break;
      case OpCode::CondBranch:
        out_ += " ";
        appendExpr(operands[0]);
        out_ += std::format(", b{}, b{}", targets[0].index(), targets[1].index());
        break;
      case OpCode::IndirectBranch:
        out_ += " ";
        appendExpr(operands[0]);
        // An empty target list is a real state, not an omission: it says the
        // computed destination has not been resolved yet.
        out_ += targets.empty() ? " -> unresolved" : " -> [";
        for (std::size_t index = 0; index < targets.size(); ++index) {
          out_ += index == 0 ? "" : ", ";
          out_ += std::format("b{}", targets[index].index());
        }
        if (!targets.empty()) {
          out_ += "]";
        }
        break;
      case OpCode::Call:
        out_ += " ";
        appendExpr(operands[0]);
        if (operands.size() > 1) {
          // SSA-level calling-convention annotations (see ssa-construct).
          out_ += "(";
          for (std::size_t index = 1; index < operands.size(); ++index) {
            out_ += index == 1 ? "" : ", ";
            appendExpr(operands[index]);
          }
          out_ += ")";
        }
        break;
      case OpCode::Return:
        if (!operands.empty()) {
          out_ += " ";
          appendExpr(operands[0]);
        }
        break;
      case OpCode::Unimplemented:
        out_ += std::format(" \"{}\"", function_.nameOf(op.payload));
        break;
      case OpCode::Intrinsic:
        out_ += std::format(" \"{}\"(", function_.nameOf(op.payload));
        for (std::size_t index = 0; index < operands.size(); ++index) {
          out_ += index == 0 ? "" : ", ";
          appendExpr(operands[index]);
        }
        out_ += ")";
        break;
      case OpCode::Phi:
        out_ += "(";
        for (std::size_t index = 0; index < operands.size(); ++index) {
          out_ += index == 0 ? "" : ", ";
          appendExpr(operands[index]);
        }
        out_ += ")";
        break;
      case OpCode::Nop:
      case OpCode::Unreachable:
      case OpCode::Count:
        break;
    }

    // Provenance beyond the address: which pass produced this op. Absent means
    // the lifter, which is the overwhelmingly common case.
    if (op.origin != kPassLifter) {
      out_ += std::format(" !from({})", function_.passName(op.origin));
    }

    // Prose, but printed inside the grammar rather than after a `;`, because a
    // trailing comment would be dropped on parse and the exact round trip this
    // format promises would stop holding for any annotated function.
    if (const std::string_view note = function_.noteOn(opId); !note.empty()) {
      out_ += std::format(" !note(\"{}\")", escapeNote(note));
    }
  }

  void appendExpr(ExprId id) {
    if (!function_.hasExpr(id)) {
      out_ += "<invalid>";
      return;
    }
    const Expr& expr = function_.expr(id);
    out_ += toString(expr.op);

    // Modifier after the colon: a type for most ops, an op-specific spelling for
    // the flag ops.
    switch (expr.op) {
      case ExprOp::FlagDef:
        out_ += std::format(":{}.{}", toString(flagDefOp(expr.immediate)),
                            flagDefWidth(expr.immediate));
        break;
      case ExprOp::FlagCond:
        out_ += std::format(":{}", toString(static_cast<ConditionCode>(expr.immediate)));
        break;
      case ExprOp::FlagBit:
        out_ += std::format(":{}", toString(static_cast<FlagBitIndex>(expr.immediate)));
        break;
      default:
        out_ += std::format(":{}", expr.type.toString());
        break;
    }

    out_ += "(";
    switch (expr.op) {
      case ExprOp::Const:
        out_ += formatConstant(expr.immediate, expr.type);
        break;
      case ExprOp::Value:
        out_ += std::format("%{}", expr.immediate);
        break;
      case ExprOp::Undef:
        break;
      case ExprOp::EntryReg:
        out_ += function_.registers().nameOf(RegId{static_cast<uint32_t>(expr.immediate)});
        break;
      case ExprOp::Extract:
        appendExpr(expr.operands[0]);
        out_ += std::format(", {}", expr.immediate);
        break;
      default:
        for (unsigned index = 0; index < expr.operandCount; ++index) {
          out_ += index == 0 ? "" : ", ";
          appendExpr(expr.operands[index]);
        }
        break;
    }
    out_ += ")";
  }

  void indent(unsigned depth) { out_.append(depth * options_.indent, ' '); }

  [[nodiscard]] std::string take() { return std::move(out_); }

 private:
  const Function& function_;
  const PrintOptions& options_;
  std::string out_;
};

}  // namespace

std::string print(const Function& function, const PrintOptions& options) {
  Printer printer{function, options};
  printer.printFunction();
  return printer.take();
}

std::string printBlock(const Function& function, BlockId block, const PrintOptions& options) {
  Printer printer{function, options};
  printer.printBlockBody(block, 0);
  return printer.take();
}

std::string printExpr(const Function& function, ExprId expr) {
  const PrintOptions options;
  Printer printer{function, options};
  printer.appendExpr(expr);
  return printer.take();
}

std::string printOp(const Function& function, OpId op) {
  const PrintOptions options;
  Printer printer{function, options};
  printer.appendOp(op);
  return printer.take();
}

}  // namespace xdec::il
