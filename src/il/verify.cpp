#include "xdec/il/verify.h"

#include <algorithm>
#include <format>
#include <unordered_set>

#include "xdec/support/bits.h"

namespace xdec::il {
namespace {

class Verifier {
 public:
  Verifier(const Function& function, Maturity level) : function_(function), level_(level) {}

  VerifyReport run() {
    checkEntry();
    checkExpressions();
    checkBlocks();
    checkValues();
    checkEdges();
    checkReachability();
    return std::move(report_);
  }

 private:
  void error(std::string message, uint64_t address = kNoAddress) {
    Diag diag{DiagCode::VerifyFailure, std::move(message)};
    if (address != kNoAddress) {
      diag.at(address);
    }
    report_.errors.push_back(std::move(diag));
  }

  void warn(std::string message, uint64_t address = kNoAddress) {
    Diag diag{DiagCode::VerifyFailure, std::move(message)};
    if (address != kNoAddress) {
      diag.at(address);
    }
    report_.warnings.push_back(std::move(diag));
  }

  [[nodiscard]] bool atLeast(Maturity level) const {
    return static_cast<unsigned>(level_) >= static_cast<unsigned>(level);
  }

  void checkEntry() {
    if (function_.blockCount() == 0) {
      error("function has no blocks");
      return;
    }
    if (!function_.hasBlock(function_.entryBlock())) {
      error("function has no valid entry block");
    }
  }

  void checkExpressions() {
    for (const ExprId id : function_.exprHandles()) {
      const Expr& expr = function_.expr(id);
      const ExprOpInfo& opInfo = info(expr.op);

      if (expr.operandCount < opInfo.minArity || expr.operandCount > opInfo.maxArity) {
        error(std::format("expression #{} '{}' has {} operands but takes {}..{}", id.index(),
                          opInfo.text, expr.operandCount, opInfo.minArity, opInfo.maxArity));
        continue;
      }
      if (!expr.type.valid()) {
        error(std::format("expression #{} '{}' has an invalid type", id.index(), opInfo.text));
      }

      bool operandsValid = true;
      for (unsigned index = 0; index < expr.operandCount; ++index) {
        const ExprId operand = expr.operands[index];
        if (!function_.hasExpr(operand)) {
          error(std::format("expression #{} '{}' operand {} is not a valid expression",
                            id.index(), opInfo.text, index));
          operandsValid = false;
          continue;
        }
        // Interning always creates a node after its operands, so an operand
        // index at or above the node's own index would mean a cycle. Checking it
        // this cheaply is the reason the pool is append-only.
        if (operand.index() >= id.index()) {
          error(std::format("expression #{} '{}' operand {} refers forward to #{}: the value "
                            "graph must be acyclic",
                            id.index(), opInfo.text, index, operand.index()));
          operandsValid = false;
        }
      }
      if (!operandsValid) {
        continue;
      }

      checkExpressionTypes(id, expr, opInfo);
    }
  }

  void checkExpressionTypes(ExprId id, const Expr& expr, const ExprOpInfo& opInfo) {
    const auto operandType = [this, &expr](unsigned index) {
      return function_.expr(expr.operands[index]).type;
    };

    switch (opInfo.result) {
      case ResultRule::SameAsOperands:
        for (unsigned index = 0; index < expr.operandCount; ++index) {
          if (operandType(index) != expr.type) {
            error(std::format("expression #{} '{}' result type {} does not match operand {} "
                              "type {}",
                              id.index(), opInfo.text, expr.type.toString(), index,
                              operandType(index).toString()));
          }
        }
        break;
      case ResultRule::Boolean:
        if (!expr.type.isBoolean()) {
          error(std::format("expression #{} '{}' must produce i1 but produces {}", id.index(),
                            opInfo.text, expr.type.toString()));
        }
        // Comparison operands must agree with each other even though they do not
        // agree with the result.
        if (expr.operandCount == 2 && operandType(0) != operandType(1)) {
          error(std::format("expression #{} '{}' compares {} with {}", id.index(), opInfo.text,
                            operandType(0).toString(), operandType(1).toString()));
        }
        break;
      case ResultRule::FlagsResult:
        if (!expr.type.isFlags()) {
          error(std::format("expression #{} '{}' must produce flags but produces {}", id.index(),
                            opInfo.text, expr.type.toString()));
        }
        break;
      case ResultRule::SameAsLastTwo:
        if (expr.operandCount == 3) {
          if (!operandType(0).isBoolean()) {
            error(std::format("expression #{} 'select' condition must be i1 but is {}",
                              id.index(), operandType(0).toString()));
          }
          if (operandType(1) != operandType(2) || operandType(1) != expr.type) {
            error(std::format("expression #{} 'select' arms have types {} and {} but the result "
                              "is {}",
                              id.index(), operandType(1).toString(), operandType(2).toString(),
                              expr.type.toString()));
          }
        }
        break;
      case ResultRule::Explicit:
        break;
    }

    switch (expr.op) {
      case ExprOp::Const:
        // A constant wider than its type would be silently truncated later.
        if (expr.type.isScalarInteger() && expr.type.bits() < 64 &&
            expr.immediate != zeroExtend(expr.immediate, expr.type.bits())) {
          error(std::format("expression #{} const 0x{:x} does not fit in {}", id.index(),
                            expr.immediate, expr.type.toString()));
        }
        break;
      case ExprOp::Value: {
        const ValueId value{static_cast<uint32_t>(expr.immediate)};
        if (!function_.hasValue(value)) {
          error(std::format("expression #{} refers to undefined value %{}", id.index(),
                            expr.immediate));
        } else if (function_.value(value).type != expr.type) {
          error(std::format("expression #{} reads value %{} as {} but it is {}", id.index(),
                            expr.immediate, expr.type.toString(),
                            function_.value(value).type.toString()));
        }
        break;
      }
      case ExprOp::EntryReg: {
        const RegId reg{static_cast<uint32_t>(expr.immediate)};
        if (!function_.registers().contains(reg)) {
          error(std::format("expression #{} names register {} which is not in the register "
                            "file", id.index(), expr.immediate));
        } else if (function_.registers()[reg].type() != expr.type) {
          error(std::format("expression #{} reads entry register '{}' as {} but it is {}",
                            id.index(), function_.registers().nameOf(reg),
                            expr.type.toString(),
                            function_.registers()[reg].type().toString()));
        }
        break;
      }
      case ExprOp::FlagCond:
      case ExprOp::FlagBit:
        if (!function_.expr(expr.operands[0]).type.isFlags()) {
          error(std::format("expression #{} '{}' operand must be flags but is {}", id.index(),
                            info(expr.op).text,
                            function_.expr(expr.operands[0]).type.toString()));
        }
        break;
      case ExprOp::Extract: {
        const Type source = function_.expr(expr.operands[0]).type;
        if (expr.immediate + expr.type.bits() > source.bits()) {
          error(std::format("expression #{} extracts {} bits at offset {} from a {} bit value",
                            id.index(), expr.type.bits(), expr.immediate, source.bits()));
        }
        break;
      }
      case ExprOp::Concat: {
        const unsigned total = function_.expr(expr.operands[0]).type.bits() +
                               function_.expr(expr.operands[1]).type.bits();
        if (total != expr.type.bits()) {
          error(std::format("expression #{} concatenates {} bits but is typed {}", id.index(),
                            total, expr.type.toString()));
        }
        break;
      }
      case ExprOp::ZExt:
      case ExprOp::SExt: {
        const Type source = function_.expr(expr.operands[0]).type;
        if (source.bits() > expr.type.bits()) {
          error(std::format("expression #{} '{}' narrows {} to {}", id.index(),
                            info(expr.op).text, source.toString(), expr.type.toString()));
        }
        break;
      }
      case ExprOp::Trunc: {
        const Type source = function_.expr(expr.operands[0]).type;
        if (source.bits() < expr.type.bits()) {
          error(std::format("expression #{} 'trunc' widens {} to {}", id.index(),
                            source.toString(), expr.type.toString()));
        }
        break;
      }
      case ExprOp::FlagDef: {
        const FlagOp flagOp = flagDefOp(expr.immediate);
        // A logical op has no second operand to compare against, and a flags
        // constant has no operands at all beyond the value it stands for.
        const unsigned expected = [flagOp] {
          switch (flagOp) {
            case FlagOp::Logical:
            case FlagOp::Const:
              return 1u;
            case FlagOp::AddCarry:
            case FlagOp::SubCarry:
              return 3u;
            default:
              return 2u;
          }
        }();
        if (expr.operandCount != expected) {
          error(std::format("expression #{} flagdef.{} takes {} operands but has {}", id.index(),
                            toString(flagOp), expected, expr.operandCount));
        }
        if (flagDefWidth(expr.immediate) == 0) {
          error(std::format("expression #{} flagdef has no operand width", id.index()));
        }
        break;
      }
      default:
        break;
    }
  }

  void checkBlocks() {
    std::unordered_set<uint64_t> blockAddresses;
    for (const BlockId blockId : function_.blockHandles()) {
      const Block& block = function_.block(blockId);

      if (block.va != 0 && !blockAddresses.insert(block.va).second) {
        error(std::format("two blocks start at 0x{:x}", block.va), block.va);
      }

      if (block.ops.empty()) {
        // External stubs are legitimately empty: they stand for code outside
        // the function, so there is nothing to put in them.
        if (!block.external) {
          error(std::format("block b{} is empty", blockId.index()), block.va);
        }
        continue;
      }

      bool seenPhiEnd = false;
      for (std::size_t index = 0; index < block.ops.size(); ++index) {
        const OpId opId = block.ops[index];
        if (!function_.hasOp(opId)) {
          error(std::format("block b{} references invalid op #{}", blockId.index(),
                            opId.index()));
          continue;
        }
        const Op& op = function_.op(opId);
        const bool isLast = index + 1 == block.ops.size();

        if (op.isTerminator() && !isLast) {
          error(std::format("block b{} has terminator '{}' at position {} of {}",
                            blockId.index(), toString(op.code), index, block.ops.size()),
                op.va);
        }
        if (!op.isTerminator() && isLast && atLeast(Maturity::Cfg)) {
          error(std::format("block b{} ends with non-terminator '{}'", blockId.index(),
                            toString(op.code)),
                op.va);
        }

        // Phis must precede everything else, so that a block's merge points are
        // a prefix and nothing observable happens before them.
        if (op.code == OpCode::Phi) {
          if (seenPhiEnd) {
            error(std::format("block b{} has a phi after a non-phi op", blockId.index()), op.va);
          }
        } else {
          seenPhiEnd = true;
        }

        checkOp(blockId, opId, op);
      }
    }
  }

  void checkOp(BlockId blockId, OpId opId, const Op& op) {
    if (op.origin >= function_.passCount()) {
      error(std::format("op #{} claims origin pass {} but only {} passes are registered",
                        opId.index(), op.origin, function_.passCount()));
    }
    // Provenance is what makes a wrong result traceable to an instruction, so at
    // the level that is one-to-one with machine code it cannot be missing.
    if (op.va == kNoOpAddress && level_ == Maturity::Lifted) {
      error(std::format("op #{} '{}' has no source address at lifted maturity", opId.index(),
                        toString(op.code)));
    }

    const std::span<const ExprId> operands = function_.operands(op);
    const std::span<const BlockId> targets = function_.targets(op);

    for (const ExprId operand : operands) {
      if (!function_.hasExpr(operand)) {
        error(std::format("op #{} '{}' has an invalid operand", opId.index(),
                          toString(op.code)),
              op.va);
      }
    }
    for (const BlockId target : targets) {
      if (!function_.hasBlock(target)) {
        error(std::format("op #{} '{}' branches to an invalid block", opId.index(),
                          toString(op.code)),
              op.va);
      }
    }

    const auto expectOperands = [&](std::size_t count) {
      if (operands.size() != count) {
        error(std::format("op #{} '{}' takes {} operands but has {}", opId.index(),
                          toString(op.code), count, operands.size()),
              op.va);
        return false;
      }
      return true;
    };
    const auto expectTargets = [&](std::size_t count) {
      if (targets.size() != count) {
        error(std::format("op #{} '{}' takes {} targets but has {}", opId.index(),
                          toString(op.code), count, targets.size()),
              op.va);
      }
    };

    switch (op.code) {
      case OpCode::ReadReg:
        expectOperands(0);
        checkRegister(opId, op);
        if (function_.registers().contains(op.reg())) {
          const Type expected = function_.registers()[op.reg()].type();
          if (expected != op.type) {
            error(std::format("op #{} reads register '{}' of type {} as {}", opId.index(),
                              function_.registers().nameOf(op.reg()), expected.toString(),
                              op.type.toString()),
                  op.va);
          }
        }
        break;
      case OpCode::WriteReg:
        if (expectOperands(1)) {
          checkRegister(opId, op);
          if (function_.registers().contains(op.reg())) {
            const Type expected = function_.registers()[op.reg()].type();
            const Type actual = function_.expr(operands[0]).type;
            if (expected != actual) {
              error(std::format("op #{} writes {} to register '{}' of type {}", opId.index(),
                                actual.toString(),
                                function_.registers().nameOf(op.reg()), expected.toString()),
                    op.va);
            }
          }
        }
        break;
      case OpCode::Load:
        if (expectOperands(1) && op.type.isVoid()) {
          error(std::format("op #{} 'load' has no result type", opId.index()), op.va);
        }
        break;
      case OpCode::Store:
        if (expectOperands(2)) {
          const Type valueType = function_.expr(operands[1]).type;
          if (valueType != op.type) {
            error(std::format("op #{} stores {} through a {} access", opId.index(),
                              valueType.toString(), op.type.toString()),
                  op.va);
          }
        }
        break;
      case OpCode::Branch:
        expectOperands(0);
        expectTargets(1);
        break;
      case OpCode::CondBranch:
        if (expectOperands(1) && !function_.expr(operands[0]).type.isBoolean()) {
          error(std::format("op #{} branch condition is {} but must be i1", opId.index(),
                            function_.expr(operands[0]).type.toString()),
                op.va);
        }
        expectTargets(2);
        break;
      case OpCode::IndirectBranch:
        // The target, plus — while the branch is unresolved and from Ssa on —
        // the calling-convention argument versions SSA construction recorded in
        // case this branch is a tail call. Same shape as a call's operands, for
        // the same reason.
        if (atLeast(Maturity::Ssa) && targets.empty()) {
          if (operands.empty() || operands.size() > 9) {
            error(std::format("op #{} indirect branch has {} operands, want 1..9",
                              opId.index(), operands.size()),
                  op.va);
          }
        } else {
          expectOperands(1);
        }
        // Zero targets is legal and meaningful before resolution, but by the
        // Resolved level an unresolved computed branch is a hole in the CFG.
        //
        // Only in a block the entry can reach, though. Lifting a function
        // sweeps forward through whatever follows the code it was asked for, so
        // a block can exist without anything branching to it — and such a block
        // is not part of the function: nothing constrains what it computes,
        // because it never computes. Demanding its branches resolve is
        // demanding an answer about code that does not run, and it cannot be
        // given: SSA construction skips unreachable blocks by the same
        // reasoning, so their register reads never even become values, leaving
        // the branch target an expression no analysis can see through. One
        // stray tail of swept-up data would otherwise fail an entire
        // decompilation.
        if (targets.empty() && atLeast(Maturity::Resolved) && isReachable(blockId)) {
          error(std::format("op #{} indirect branch is still unresolved at {} maturity",
                            opId.index(), toString(level_)),
                op.va);
        }
        break;
      case OpCode::Call:
        // Below Ssa a call carries only its target; SSA construction appends
        // the calling-convention argument values (target plus up to eight).
        if (atLeast(Maturity::Ssa)) {
          if (operands.empty() || operands.size() > 9) {
            error(std::format("op #{} call has {} operands, want 1..9", opId.index(),
                              operands.size()),
                  op.va);
          }
        } else {
          expectOperands(1);
        }
        break;
      case OpCode::Return:
        // SSA construction annotates the return value as operand zero.
        if (atLeast(Maturity::Ssa)) {
          if (operands.size() > 1) {
            error(std::format("op #{} return has {} operands, want 0..1", opId.index(),
                              operands.size()),
                  op.va);
          }
        } else {
          expectOperands(0);
        }
        expectTargets(0);
        break;
      case OpCode::Nop:
      case OpCode::Unreachable:
        expectOperands(0);
        expectTargets(0);
        break;
      case OpCode::Unimplemented:
        if (function_.nameOf(op.payload).empty()) {
          warn(std::format("op #{} 'unimplemented' does not name the instruction",
                           opId.index()),
               op.va);
        }
        break;
      case OpCode::Intrinsic:
        if (function_.nameOf(op.payload).empty()) {
          error(std::format("op #{} 'intrinsic' has no name", opId.index()), op.va);
        }
        break;
      case OpCode::Phi: {
        const std::size_t predecessorCount = function_.block(blockId).predecessors.size();
        if (atLeast(Maturity::Ssa) && operands.size() != predecessorCount) {
          error(std::format("op #{} phi has {} operands but block b{} has {} predecessors",
                            opId.index(), operands.size(), blockId.index(), predecessorCount),
                op.va);
        }
        for (const ExprId operand : operands) {
          if (function_.hasExpr(operand) && function_.expr(operand).type != op.type) {
            error(std::format("op #{} phi operand type {} does not match the result type {}",
                              opId.index(), function_.expr(operand).type.toString(),
                              op.type.toString()),
                  op.va);
          }
        }
        break;
      }
      case OpCode::Count:
        error(std::format("op #{} has an invalid opcode", opId.index()), op.va);
        break;
    }

    // Intrinsics and calls define a value according to their type rather
    // than the opcode alone: void means no result.
    const bool shouldDefine = op.code == OpCode::Intrinsic || op.code == OpCode::Call
                                  ? !op.type.isVoid()
                                  : op.definesValue();
    if (shouldDefine && !op.result.valid()) {
      error(std::format("op #{} '{}' defines a value but has no result", opId.index(),
                        toString(op.code)),
            op.va);
    } else if (!shouldDefine && op.result.valid()) {
      error(std::format("op #{} '{}' has a result but does not define one", opId.index(),
                        toString(op.code)),
            op.va);
    }
  }

  void checkRegister(OpId opId, const Op& op) {
    if (!function_.registers().contains(op.reg())) {
      error(std::format("op #{} '{}' names register {} which is not in the register file",
                        opId.index(), toString(op.code), op.payload),
            op.va);
    }
  }

  void checkValues() {
    std::vector<unsigned> definitionCount(function_.valueCount(), 0);

    for (const BlockId blockId : function_.blockHandles()) {
      for (const OpId opId : function_.block(blockId).ops) {
        if (!function_.hasOp(opId)) {
          continue;
        }
        const Op& op = function_.op(opId);
        if (!op.result.valid()) {
          continue;
        }
        if (!function_.hasValue(op.result)) {
          error(std::format("op #{} defines invalid value %{}", opId.index(),
                            op.result.index()),
                op.va);
          continue;
        }
        ++definitionCount[op.result.asSize()];

        const ValueInfo& value = function_.value(op.result);
        if (value.definition != opId) {
          error(std::format("value %{} records op #{} as its definition but op #{} defines it",
                            op.result.index(), value.definition.index(), opId.index()),
                op.va);
        }
        if (value.block != blockId) {
          error(std::format("value %{} records block b{} but is defined in b{}",
                            op.result.index(), value.block.index(), blockId.index()),
                op.va);
        }
        if (value.type != op.type) {
          error(std::format("value %{} is typed {} but its definition produces {}",
                            op.result.index(), value.type.toString(), op.type.toString()),
                op.va);
        }
      }
    }

    for (std::size_t index = 0; index < definitionCount.size(); ++index) {
      if (definitionCount[index] == 0) {
        // A tombstone — invalid definition — is a value removed with its op,
        // not a corrupt one.
        if (function_.value(ValueId{static_cast<uint32_t>(index)}).definition.valid()) {
          error(std::format("value %{} is never defined", index));
        }
      } else if (definitionCount[index] > 1) {
        error(std::format("value %{} is defined {} times; values are single assignment", index,
                          definitionCount[index]));
      }
    }

    checkValueUseOrder();
  }

  /// Below SSA, values are block-local by construction: an expression may only
  /// read a value defined earlier in the same block. That is what allows the
  /// expression pool to be hash-consed while register contents still change over
  /// time.
  void checkValueUseOrder() {
    if (atLeast(Maturity::Ssa)) {
      return;
    }
    for (const BlockId blockId : function_.blockHandles()) {
      std::unordered_set<uint32_t> availableValues;
      for (const OpId opId : function_.block(blockId).ops) {
        if (!function_.hasOp(opId)) {
          continue;
        }
        const Op& op = function_.op(opId);
        for (const ExprId operand : function_.operands(op)) {
          collectValueUses(operand, [&](ValueId used) {
            if (!function_.hasValue(used)) {
              return;
            }
            if (function_.value(used).block != blockId) {
              error(std::format("op #{} in b{} reads value %{} defined in b{}; values are "
                                "block-local below ssa maturity",
                                opId.index(), blockId.index(), used.index(),
                                function_.value(used).block.index()),
                    op.va);
            } else if (!availableValues.contains(used.index())) {
              error(std::format("op #{} reads value %{} before its definition", opId.index(),
                                used.index()),
                    op.va);
            }
          });
        }
        if (op.result.valid()) {
          availableValues.insert(op.result.index());
        }
      }
    }
  }

  template <class Callback>
  void collectValueUses(ExprId id, const Callback& callback) {
    // Iterative: optimised expression DAGs are far deeper than any call stack
    // a verifier may rely on (a substituted chain is one long spine, and the
    // visit would overflow on exactly the functions it must check).
    std::vector<ExprId> work{id};
    std::unordered_set<uint32_t> seen;
    while (!work.empty()) {
      const ExprId at = work.back();
      work.pop_back();
      if (!function_.hasExpr(at) || !seen.insert(at.index()).second) {
        continue;
      }
      const Expr& expr = function_.expr(at);
      if (expr.op == ExprOp::Value) {
        callback(ValueId{static_cast<uint32_t>(expr.immediate)});
        continue;
      }
      for (unsigned index = 0; index < expr.operandCount; ++index) {
        work.push_back(expr.operands[index]);
      }
    }
  }

  /// The stored edges must equal what the terminators imply. They are cached for
  /// speed, and a stale cache is a class of bug that is otherwise very hard to
  /// see.
  void checkEdges() {
    for (const BlockId blockId : function_.blockHandles()) {
      const Block& block = function_.block(blockId);
      if (block.ops.empty()) {
        continue;
      }
      const OpId terminatorId = block.ops.back();
      if (!function_.hasOp(terminatorId)) {
        continue;
      }
      const Op& terminator = function_.op(terminatorId);
      if (!terminator.isTerminator()) {
        continue;
      }

      std::vector<BlockId> expected;
      for (const BlockId target : function_.targets(terminator)) {
        if (function_.hasBlock(target)) {
          expected.push_back(target);
        }
      }
      if (expected != block.successors) {
        error(std::format("block b{} stores {} successors but its terminator implies {}; "
                          "rebuildEdges was not called after the last edit",
                          blockId.index(), block.successors.size(), expected.size()),
              block.va);
      }
    }

    // Every stored predecessor edge must have a matching successor edge.
    for (const BlockId blockId : function_.blockHandles()) {
      for (const BlockId predecessor : function_.block(blockId).predecessors) {
        if (!function_.hasBlock(predecessor)) {
          error(std::format("block b{} lists an invalid predecessor", blockId.index()));
          continue;
        }
        const std::vector<BlockId>& successors = function_.block(predecessor).successors;
        if (std::find(successors.begin(), successors.end(), blockId) == successors.end()) {
          error(std::format("block b{} lists b{} as a predecessor but b{} has no edge to it",
                            blockId.index(), predecessor.index(), predecessor.index()));
        }
      }
    }
  }

  void checkReachability() {
    if (function_.blockCount() == 0 || !function_.hasBlock(function_.entryBlock())) {
      return;
    }
    if (reachable().size() == function_.blockCount()) {
      return;
    }
    for (const BlockId blockId : function_.blockHandles()) {
      if (!isReachable(blockId)) {
        // A warning rather than an error: a block can legitimately become
        // unreachable mid-pipeline, before a cleanup pass removes it.
        warn(std::format("block b{} is unreachable from the entry", blockId.index()),
             function_.block(blockId).va);
      }
    }
  }

  /// Blocks the entry can reach, computed once. Several checks ask, and the
  /// answer cannot change during a verification.
  [[nodiscard]] const std::unordered_set<uint32_t>& reachable() {
    if (!reachable_.has_value()) {
      reachable_.emplace();
      if (function_.hasBlock(function_.entryBlock())) {
        for (const BlockId blockId : function_.reversePostOrder()) {
          reachable_->insert(blockId.index());
        }
      }
    }
    return *reachable_;
  }

  [[nodiscard]] bool isReachable(BlockId blockId) {
    return reachable().contains(blockId.index());
  }

  const Function& function_;
  Maturity level_;
  VerifyReport report_;
  std::optional<std::unordered_set<uint32_t>> reachable_;
};

}  // namespace

std::string VerifyReport::format() const {
  std::string text;
  for (const Diag& diag : errors) {
    text += std::format("error: {}\n", diag.format());
  }
  for (const Diag& diag : warnings) {
    text += std::format("warning: {}\n", diag.format());
  }
  if (text.empty()) {
    text = "ok\n";
  }
  return text;
}

VerifyReport verify(const Function& function) { return verify(function, function.maturity()); }

VerifyReport verify(const Function& function, Maturity level) {
  Verifier verifier{function, level};
  return verifier.run();
}

Result<void> verifyOrFail(const Function& function) {
  VerifyReport report = verify(function);
  if (report.ok()) {
    return ok();
  }
  Diag diag{DiagCode::VerifyFailure,
            std::format("IL verification failed with {} error(s)", report.errors.size())};
  for (const Diag& entry : report.errors) {
    diag.note(entry.format());
  }
  return err(std::move(diag));
}

}  // namespace xdec::il
