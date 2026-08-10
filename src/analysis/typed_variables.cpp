// TypedVariables::recover (see the header for the shapes this walks).
#include "xdec/analysis/typed_variables.h"

#include <span>
#include <unordered_set>

#include "xdec/analysis/import_callee.h"
#include "xdec/passes/recover_syscall.h"
#include "xdec/types/database.h"

namespace xdec::analysis {

namespace {

/// Type evidence for a value loaded from a GOT/import slot naming a *data*
/// symbol: the slot holds the address of that symbol's own object in the
/// defining module, so the loaded value is a pointer to whatever the header
/// declared under that name -- the same "loader value names an import"
/// evidence calleeThroughImportSlot uses for a call target, applied to data
/// instead of a function. Invalid whenever nothing names the slot, the name
/// binds to a function instead of data, or no declaration anywhere already
/// spells a pointer to the type (see types::TypeDatabase::findPointerTo):
/// this analysis reads the database, it does not intern into it.
[[nodiscard]] types::TypeId globalPointerType(const types::TypeBinder& binder,
                                              const types::TypeDatabase& database,
                                              const MemoryFacts& memory, uint64_t slot) {
  const LoaderValue bound = memory.loaderValueAt(slot);
  if (bound.importName.empty()) {
    return {};
  }
  const types::Binding& binding = binder.forName(bound.importName);
  if (!binding.valid() || binding.isFunction) {
    return {};
  }
  return database.findPointerTo(binding.type).value_or(types::TypeId{});
}

/// The direct callee address, when the call target is a plain constant -- the
/// one shape CalleeSummaries can key on. Distinct from calleeOf's constant
/// case in that this does not need a binder or a database: a summary is
/// evidence about an address regardless of whether a header was imported for
/// this run.
[[nodiscard]] bool directCallAddress(const il::Function& function, il::ExprId target,
                                     uint64_t& address) {
  return function.asConstantThroughCasts(target, address);
}

/// Peels the width adjustments a `ret` operand picks up on its way out of the
/// callee -- the same shapes analysis::MergeWidth peels for the same reason
/// (see variables.cpp): a 32-bit call result widened to fill a 64-bit return
/// slot is still the same value.
[[nodiscard]] il::ExprId peelCasts(const il::Function& function, il::ExprId id) {
  for (unsigned depth = 0; depth < 8; ++depth) {
    const il::Expr& expr = function.expr(id);
    switch (expr.op) {
      case il::ExprOp::ZExt:
      case il::ExprOp::SExt:
      case il::ExprOp::Trunc:
      case il::ExprOp::Bitcast:
        id = expr.operand(0);
        continue;
      default:
        return id;
    }
  }
  return id;
}

/// Backward propagation: from a typed call/svc argument, down to the leaf the
/// evidence actually describes (see the header's class comment for the
/// rationale; this is the walk documented there).
class Builder {
 public:
  Builder(const il::Function& function, const StackFrame& frame, const types::TypeDatabase* database)
      : function_(function), frame_(frame), database_(database) {}

  void propagate(il::ExprId expr, types::TypeId type) {
    if (!type.valid()) {
      return;
    }
    std::unordered_set<uint32_t> visitedPhis;
    walk(expr, type, 0, visitedPhis);
  }

  /// Direct evidence about a value the walk does not need to trace further:
  /// a call or svc result, or the enclosing function's own return.
  void mergeValue(il::ValueId value, types::TypeId type) {
    if (type.valid()) {
      byValue_.try_emplace(value.index(), type);
    }
  }

  std::unordered_map<uint32_t, types::TypeId> byArgument_;
  std::unordered_map<uint32_t, types::TypeId> byValue_;
  std::unordered_map<int64_t, types::TypeId> byStackSlot_;

 private:
  static constexpr unsigned kMaxDepth = 32;

  [[nodiscard]] types::TypeId pointeeOf(types::TypeId type) const {
    if (database_ == nullptr) {
      return type;
    }
    const types::TypeEntry* entry = database_->get(database_->resolveTypedef(type));
    return entry != nullptr && entry->kind == types::TypeKind::Pointer ? entry->element : type;
  }

  void walk(il::ExprId expr, types::TypeId type, unsigned depth,
           std::unordered_set<uint32_t>& visitedPhis) {
    if (depth > kMaxDepth) {
      return;
    }
    // Stack-frame classification first: it already sees through the
    // `entry(sp) + delta` chain (and the bare `entry(sp)` case, delta zero),
    // so this one call covers both rows of the header's table that mention
    // the stack pointer without this walk re-deriving frameDelta itself.
    const AddressInfo info = frame_.classify(expr);
    if (info.kind == AddressKind::StackSlot) {
      // `type` is what the callee's parameter declares, and that parameter is
      // a pointer *to* the slot (`&var_tv`, matching `struct timeval*`) rather
      // than the slot's own contents -- unlike the EntryReg/Value cases below,
      // where the value itself is what the type describes. One level of
      // pointer is peeled so the evidence recorded is what actually lives at
      // this address.
      byStackSlot_.try_emplace(info.delta, pointeeOf(type));
      return;
    }

    const il::Expr& e = function_.expr(expr);
    switch (e.op) {
      case il::ExprOp::EntryReg:
        byArgument_.try_emplace(static_cast<uint32_t>(e.immediate), type);
        return;
      case il::ExprOp::Select:
        walk(e.operand(1), type, depth + 1, visitedPhis);
        walk(e.operand(2), type, depth + 1, visitedPhis);
        return;
      case il::ExprOp::Value: {
        const il::ValueId value{static_cast<uint32_t>(e.immediate)};
        if (!function_.hasValue(value)) {
          return;
        }
        const il::OpId definition = function_.value(value).definition;
        if (!definition.valid()) {
          return;
        }
        const il::Op& def = function_.op(definition);
        if (def.code == il::OpCode::Phi) {
          if (!visitedPhis.insert(value.index()).second) {
            return;  // a loop-carried phi adds nothing its operands do not
          }
          for (const il::ExprId incoming : function_.operands(def)) {
            walk(incoming, type, depth + 1, visitedPhis);
          }
          return;
        }
        // A load, another call's result, or anything else already reduced to
        // one SSA value: evidence about the value itself, not traced further
        // back. This is also where a call's own result lands (see
        // recover()), so a value passed through unchanged from one typed call
        // into another still ends up typed.
        byValue_.try_emplace(value.index(), type);
        return;
      }
      default:
        // Arithmetic, a cast, a load address: none of the shapes the header
        // documents, so nothing is claimed. Guessing through a `+` would be
        // exactly the kind of instruction-not-evidence import::binder.h warns
        // against -- the code may have advanced the pointer, or it may not be
        // a pointer at all.
        return;
    }
  }

  const il::Function& function_;
  const StackFrame& frame_;
  const types::TypeDatabase* database_;
};

}  // namespace

TypedVariables TypedVariables::recover(const il::Function& function, const StackFrame& frame,
                                       const types::TypeDatabase* database,
                                       const types::SyscallTable* syscalls,
                                       const types::NameAt& names,
                                       const CalleeSummaries& summaries,
                                       const MemoryFacts& memory,
                                       const binary::TargetProfile* profile) {
  TypedVariables result;
  if (database == nullptr && syscalls == nullptr) {
    return result;  // no evidence source at all: every lookup below is unset
  }

  Builder builder(function, frame, database);
  std::optional<types::TypeBinder> binder;
  const types::TypeEntry* self = nullptr;
  if (database != nullptr) {
    binder.emplace(*database, names);
    self = binder->prototypeAt(function.block(function.entryBlock()).va);
  }

  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      if (op.code == il::OpCode::Call) {
        const auto operands = function.operands(op);
        if (operands.empty()) {
          continue;
        }
        const types::TypeEntry* callee =
            binder ? resolveCallee(function, *binder, self, memory, operands[0], profile)
                  : nullptr;
        std::span<const types::TypeId> paramTypes;
        types::TypeId returnType;
        if (callee != nullptr && !callee->variadic) {
          returnType = callee->returnType;
        } else if (uint64_t address = 0; directCallAddress(function, operands[0], address)) {
          if (const auto found = summaries.find(address); found != summaries.end()) {
            paramTypes = found->second.paramTypes;
            returnType = found->second.returnType;
          }
        }
        for (std::size_t index = 1; index < operands.size(); ++index) {
          const std::size_t position = index - 1;
          types::TypeId paramType;
          if (callee != nullptr && !callee->variadic && position < callee->params.size()) {
            paramType = callee->params[position].type;
            if (!binder->registerShaped(paramType)) {
              continue;
            }
          } else if (position < paramTypes.size()) {
            paramType = paramTypes[position];
          }
          builder.propagate(operands[index], paramType);
        }
        if (op.result.valid() && returnType.valid() &&
            (!binder.has_value() || callee == nullptr || binder->registerShaped(returnType))) {
          builder.mergeValue(op.result, returnType);
        }
      } else if (syscalls != nullptr && op.code == il::OpCode::Intrinsic &&
                function.nameOf(op.payload) == passes::kSyscallIntrinsic) {
        const auto operands = function.operands(op);
        if (operands.size() <= passes::kSyscallNumberOperand) {
          continue;
        }
        uint64_t number = 0;
        if (!function.asConstantThroughCasts(operands[passes::kSyscallNumberOperand], number)) {
          continue;
        }
        const types::SyscallInfo* info = syscalls->find(static_cast<uint32_t>(number));
        if (info == nullptr) {
          continue;
        }
        for (std::size_t index = 0; index < info->argTypeIds.size(); ++index) {
          const std::size_t operandIndex = passes::kSyscallFirstArgOperand + index;
          if (operandIndex >= operands.size() || !info->argTypeIds[index].valid()) {
            continue;
          }
          builder.propagate(operands[operandIndex], info->argTypeIds[index]);
        }
        if (op.result.valid()) {
          builder.mergeValue(op.result, info->returnTypeId);
        }
      } else if (binder.has_value() && op.code == il::OpCode::Load && op.result.valid()) {
        const auto operands = function.operands(op);
        uint64_t slot = 0;
        if (!operands.empty() && function.asConstantThroughCasts(operands[0], slot)) {
          if (const types::TypeId pointerType =
                  globalPointerType(*binder, *database, memory, slot);
              pointerType.valid()) {
            builder.mergeValue(op.result, pointerType);
          }
        }
      }
    }
  }

  result.byArgument_ = std::move(builder.byArgument_);
  result.byValue_ = std::move(builder.byValue_);
  result.byStackSlot_ = std::move(builder.byStackSlot_);

  // The function's own return: only when every `ret` that carries a value
  // traces to the same already-typed value, so a function with one error path
  // returning a raw error code and another returning a typed call's result
  // does not have the error code's absence of evidence overridden by the
  // other path's presence of it, nor the reverse.
  bool anyReturn = false;
  bool disagreement = false;
  types::TypeId agreed;
  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      if (op.code != il::OpCode::Return) {
        continue;
      }
      const auto operands = function.operands(op);
      if (operands.empty()) {
        continue;
      }
      anyReturn = true;
      const il::Expr& source = function.expr(peelCasts(function, operands[0]));
      types::TypeId found;
      if (source.op == il::ExprOp::Value) {
        const auto hit = result.byValue_.find(static_cast<uint32_t>(source.immediate));
        if (hit != result.byValue_.end()) {
          found = hit->second;
        }
      }
      if (!found.valid()) {
        disagreement = true;
        continue;
      }
      if (!agreed.valid()) {
        agreed = found;
      } else if (agreed != found) {
        disagreement = true;
      }
    }
  }
  if (anyReturn && !disagreement && agreed.valid()) {
    result.returnType_ = agreed;
  }

  return result;
}

std::optional<types::TypeId> TypedVariables::forArgument(il::RegId root) const {
  const auto found = byArgument_.find(root.index());
  return found == byArgument_.end() ? std::nullopt : std::make_optional(found->second);
}

std::optional<types::TypeId> TypedVariables::forValue(il::ValueId value) const {
  const auto found = byValue_.find(value.index());
  return found == byValue_.end() ? std::nullopt : std::make_optional(found->second);
}

std::optional<types::TypeId> TypedVariables::forStackSlot(int64_t delta) const {
  const auto found = byStackSlot_.find(delta);
  return found == byStackSlot_.end() ? std::nullopt : std::make_optional(found->second);
}

CalleeSummary summaryOf(const VariableTable& variables, const TypedVariables& typed) {
  CalleeSummary summary;
  for (const Variable& arg : variables.arguments()) {
    // "aN": the same naming VariableTable::recover gives every argument
    // leaf, parsed back out rather than carried alongside it because nothing
    // else here needs the calling-convention position as a number.
    if (arg.name.size() != 2 || arg.name[0] != 'a' || arg.name[1] < '0' || arg.name[1] > '9') {
      continue;
    }
    const std::size_t index = static_cast<std::size_t>(arg.name[1] - '0');
    if (summary.paramTypes.size() <= index) {
      summary.paramTypes.resize(index + 1);
    }
    if (arg.importedType.has_value()) {
      summary.paramTypes[index] = *arg.importedType;
    }
  }
  summary.returnType = typed.returnType().value_or(types::TypeId{});
  return summary;
}

}  // namespace xdec::analysis
