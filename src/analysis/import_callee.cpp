// calleeThroughImportSlot / resolveCallee (see the header for why these are
// shared rather than restated per caller).
#include "xdec/analysis/import_callee.h"

namespace xdec::analysis {

namespace {

[[nodiscard]] std::string aliasOf(const std::string& name, const binary::TargetProfile* profile) {
  if (profile == nullptr) {
    return name;
  }
  const auto found = profile->symbolAliases.find(name);
  return found == profile->symbolAliases.end() ? name : found->second;
}

}  // namespace

int calleeArgumentIndex(const il::Function& function, il::RegId root) {
  const std::string_view name = function.registers().nameOf(root);
  if (name.size() == 2 && name[0] == 'x' && name[1] >= '0' && name[1] <= '7') {
    return name[1] - '0';
  }
  return -1;
}

std::optional<std::string> importNameThroughSlot(const il::Function& function,
                                                  const MemoryFacts& memory, il::ExprId target,
                                                  const binary::TargetProfile* profile) {
  const il::Expr& expr = function.expr(target);
  if (expr.op != il::ExprOp::Value) {
    return std::nullopt;
  }
  const il::ValueId value{static_cast<uint32_t>(expr.immediate)};
  if (!function.hasValue(value)) {
    return std::nullopt;
  }
  const il::OpId definition = function.value(value).definition;
  if (!definition.valid() || function.op(definition).code != il::OpCode::Load) {
    return std::nullopt;
  }
  const auto address = function.operands(function.op(definition));
  uint64_t slot = 0;
  if (address.empty() || !function.asConstantThroughCasts(address[0], slot)) {
    return std::nullopt;
  }
  const LoaderValue bound = memory.loaderValueAt(slot);
  if (bound.importName.empty()) {
    return std::nullopt;
  }
  return aliasOf(bound.importName, profile);
}

const types::TypeEntry* calleeThroughImportSlot(const il::Function& function,
                                                const types::TypeBinder& binder,
                                                const MemoryFacts& memory, il::ExprId target,
                                                const binary::TargetProfile* profile) {
  const std::optional<std::string> name = importNameThroughSlot(function, memory, target, profile);
  if (!name.has_value()) {
    return nullptr;
  }
  return binder.prototypeFor(binder.forName(*name));
}

const types::TypeEntry* resolveCallee(const il::Function& function, const types::TypeBinder& binder,
                                      const types::TypeEntry* self, const MemoryFacts& memory,
                                      il::ExprId target, const binary::TargetProfile* profile) {
  uint64_t address = 0;
  if (function.asConstantThroughCasts(target, address)) {
    return binder.prototypeAt(address);
  }
  if (const types::TypeEntry* imported =
          calleeThroughImportSlot(function, binder, memory, target, profile);
      imported != nullptr) {
    return imported;
  }
  if (self == nullptr) {
    return nullptr;
  }
  const il::Expr& expr = function.expr(target);
  if (expr.op != il::ExprOp::EntryReg) {
    return nullptr;
  }
  const int index = calleeArgumentIndex(function, il::RegId{static_cast<uint32_t>(expr.immediate)});
  if (index < 0 || static_cast<std::size_t>(index) >= self->params.size()) {
    return nullptr;
  }
  return binder.pointeeFunction(self->params[static_cast<std::size_t>(index)].type);
}

}  // namespace xdec::analysis
