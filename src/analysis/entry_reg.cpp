// EntryRegFacts (see the header for what this is and why the CLI never
// names it).
#include "xdec/analysis/entry_reg.h"

namespace xdec::analysis {

const EntryRegBinding* EntryRegFacts::bindingFor(std::string_view regName) const {
  const auto found = bindings_.find(std::string{regName});
  return found == bindings_.end() ? nullptr : &found->second;
}

std::optional<uint64_t> EntryRegFacts::companionBase(std::string_view name) const {
  const auto found = companionBases_.find(std::string{name});
  return found == companionBases_.end() ? std::optional<uint64_t>{} : found->second;
}

std::optional<uint64_t> EntryRegFacts::resolve(std::string_view regName) const {
  const EntryRegBinding* binding = bindingFor(regName);
  if (binding == nullptr) {
    return std::nullopt;
  }
  switch (binding->kind) {
    case EntryRegKind::Literal:
      return binding->literal;
    case EntryRegKind::BasePlusOffset: {
      const std::optional<uint64_t> base = companionBase(binding->companion);
      if (!base.has_value()) {
        return std::nullopt;
      }
      return *base + binding->offset;
    }
    case EntryRegKind::Unknown:
    default:
      return std::nullopt;
  }
}

}  // namespace xdec::analysis
