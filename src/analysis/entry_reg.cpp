// EntryRegFacts (see the header for what this is and why the CLI never
// names it).
#include "xdec/analysis/entry_reg.h"

#include <charconv>
#include <system_error>

#include "xdec/support/json.h"

namespace xdec::analysis {

namespace {

/// Accepts both `"0x104fe0310"`/`"12345"` (a sidecar's own spelling, matching
/// this project's hex-string convention for addresses everywhere else --
/// samples/manifest.json, CLI arguments) and a bare JSON number, since a
/// hand-written sidecar reaches for whichever is convenient.
[[nodiscard]] std::optional<uint64_t> parseValue(const json::Value& value) {
  if (value.isNumber()) {
    return static_cast<uint64_t>(value.asInt());
  }
  if (!value.isString()) {
    return std::nullopt;
  }
  std::string_view text = value.asString();
  int base = 10;
  if (text.starts_with("0x") || text.starts_with("0X")) {
    text.remove_prefix(2);
    base = 16;
  }
  if (text.empty()) {
    return std::nullopt;
  }
  uint64_t out = 0;
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto result = std::from_chars(begin, end, out, base);
  if (result.ec != std::errc{} || result.ptr != end) {
    return std::nullopt;
  }
  return out;
}

}  // namespace

std::optional<std::filesystem::path> discoverEntrySidecar(
    const std::filesystem::path& binaryPath) {
  std::filesystem::path candidate = binaryPath;
  candidate += ".entry.json";
  std::error_code ec;
  if (std::filesystem::is_regular_file(candidate, ec) && !ec) {
    return candidate;
  }
  return std::nullopt;
}

Result<EntrySidecar> loadEntrySidecar(const std::filesystem::path& path) {
  XDEC_TRY(const json::Value document, json::parseFile(path.string()));
  if (!document.isObject()) {
    return err(DiagCode::BadFormat,
                     "entry sidecar {} is not a JSON object", path.string());
  }
  EntrySidecar sidecar;
  if (const json::Value* literals = document.find("literal")) {
    if (!literals->isObject()) {
      return err(DiagCode::BadFormat,
                       "entry sidecar {}: 'literal' must be an object", path.string());
    }
    for (const auto& [name, value] : literals->members()) {
      const std::optional<uint64_t> parsed = parseValue(value);
      if (!parsed.has_value()) {
        return err(DiagCode::BadFormat,
                         "entry sidecar {}: literal '{}' is not a number or hex string",
                         path.string(), name);
      }
      sidecar.literals.emplace(name, *parsed);
    }
  }
  if (const json::Value* companions = document.find("companions")) {
    if (!companions->isArray()) {
      return err(DiagCode::BadFormat,
                       "entry sidecar {}: 'companions' must be an array", path.string());
    }
    for (const json::Value& entry : companions->items()) {
      const std::optional<std::string> name = entry.stringAt("name");
      const std::optional<std::string> companionPath = entry.stringAt("path");
      if (!name.has_value() || !companionPath.has_value()) {
        return err(DiagCode::BadFormat,
                         "entry sidecar {}: each companion needs a 'name' and a 'path'",
                         path.string());
      }
      EntryCompanion companion;
      companion.name = *name;
      companion.path = *companionPath;
      if (const json::Value* base = entry.find("base")) {
        const std::optional<uint64_t> parsed = parseValue(*base);
        if (!parsed.has_value()) {
          return err(DiagCode::BadFormat,
                           "entry sidecar {}: companion '{}' has a 'base' that is not a "
                           "number or hex string",
                           path.string(), *name);
        }
        companion.runtimeBase = *parsed;
      }
      sidecar.companions.push_back(std::move(companion));
    }
  }
  if (const json::Value* memory = document.find("memory")) {
    if (!memory->isArray()) {
      return err(DiagCode::BadFormat, "entry sidecar {}: 'memory' must be an array", path.string());
    }
    for (const json::Value& entry : memory->items()) {
      const json::Value* addressField = entry.find("address");
      const json::Value* valueField = entry.find("value");
      if (addressField == nullptr || valueField == nullptr) {
        return err(DiagCode::BadFormat,
                   "entry sidecar {}: each memory seed needs an 'address' and a 'value'",
                   path.string());
      }
      const std::optional<uint64_t> address = parseValue(*addressField);
      const std::optional<uint64_t> value = parseValue(*valueField);
      if (!address.has_value() || !value.has_value()) {
        return err(DiagCode::BadFormat,
                   "entry sidecar {}: memory seed 'address'/'value' must be a number or hex "
                   "string",
                   path.string());
      }
      MemorySeed seed;
      seed.address = *address;
      seed.value = *value;
      if (const json::Value* width = entry.find("width")) {
        if (!width->isNumber()) {
          return err(DiagCode::BadFormat, "entry sidecar {}: memory seed 'width' must be a number",
                     path.string());
        }
        seed.width = static_cast<unsigned>(width->asInt());
      }
      if (seed.width != 1 && seed.width != 2 && seed.width != 4 && seed.width != 8) {
        return err(DiagCode::BadFormat,
                   "entry sidecar {}: memory seed at 0x{:x} has width {} (must be 1/2/4/8)",
                   path.string(), seed.address, seed.width);
      }
      sidecar.memorySeeds.push_back(seed);
    }
  }
  return sidecar;
}

const EntryRegBinding* EntryRegFacts::bindingFor(std::string_view regName) const {
  const auto found = bindings_.find(std::string{regName});
  return found == bindings_.end() ? nullptr : &found->second;
}

std::optional<uint64_t> EntryRegFacts::companionBase(std::string_view name) const {
  const auto found = companionBases_.find(std::string{name});
  return found == companionBases_.end() ? std::optional<uint64_t>{} : found->second;
}

std::optional<uint64_t> EntryRegFacts::memoryValueAt(uint64_t address, unsigned width) const {
  for (const MemorySeed& seed : memorySeeds_) {
    if (seed.address == address && seed.width == width) {
      return seed.value;
    }
  }
  return std::nullopt;
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
