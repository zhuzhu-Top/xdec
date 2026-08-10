#include "xdec/types/syscall_table.h"

#include <charconv>
#include <filesystem>
#include <format>

#include "xdec/types/database.h"

#ifndef XDEC_TYPES_DIR
#define XDEC_TYPES_DIR "types"
#endif

namespace xdec::types {

namespace {

/// Reads the small subset of C type spellings the syscall table's `args`/`ret`
/// strings use: an optional `const`, a base name (`int`, `unsigned long`,
/// `size_t`, or `struct`/`union`/`enum <tag>`), then zero or more trailing
/// `*`. This is not the `.hdecl` grammar — no arrays, no function pointers, no
/// declarator nesting — because the table never spells anything more than
/// that; see types/syscall/aarch64-linux.json for the full vocabulary this
/// covers.
///
/// `const` is dropped rather than modelled: TypeDatabase has no const bit (see
/// types/type.h), and the printer already casts every syscall argument to the
/// table's own spelling regardless, so the qualifier would have nowhere to go.
[[nodiscard]] TypeId resolveSpelling(const TypeDatabase& database, std::string_view spelling) {
  const auto trim = [](std::string_view text) {
    while (!text.empty() && text.front() == ' ') {
      text.remove_prefix(1);
    }
    while (!text.empty() && text.back() == ' ') {
      text.remove_suffix(1);
    }
    return text;
  };

  std::string_view text = trim(spelling);
  unsigned pointerDepth = 0;
  while (!text.empty() && text.back() == '*') {
    text.remove_suffix(1);
    text = trim(text);
    ++pointerDepth;
  }
  constexpr std::string_view kConst = "const ";
  if (text.starts_with(kConst)) {
    text = trim(text.substr(kConst.size()));
  }

  TypeId base;
  bool matched = false;
  for (const std::string_view keyword : {"struct ", "union ", "enum "}) {
    if (text.starts_with(keyword)) {
      base = database.lookup(text.substr(keyword.size()), NameSpace::Tag);
      matched = true;
      break;
    }
  }
  if (!matched) {
    base = database.lookup(text);
  }
  if (!base.valid()) {
    return {};
  }

  TypeId result = base;
  for (unsigned level = 0; level < pointerDepth; ++level) {
    const std::optional<TypeId> pointer = database.findPointerTo(result);
    if (!pointer.has_value()) {
      // No earlier declaration ever needed exactly this pointer depth, and
      // resolving from a `const TypeDatabase&` cannot intern a new one (see
      // TypeDatabase::findPointerTo) -- so the spelling stays untyped rather
      // than silently stopping one level short of what it said.
      return {};
    }
    result = *pointer;
  }
  return result;
}

}  // namespace

void SyscallTable::resolveTypes(const TypeDatabase& database) {
  for (auto& [number, info] : byNumber_) {
    info.argTypeIds.clear();
    info.argTypeIds.reserve(info.argTypes.size());
    for (const std::string& spelling : info.argTypes) {
      info.argTypeIds.push_back(resolveSpelling(database, spelling));
    }
    info.returnTypeId = info.returnType.empty() ? TypeId{} : resolveSpelling(database, info.returnType);
  }
}

Result<SyscallTable> SyscallTable::fromJson(const json::Value& value) {
  const json::Value* syscalls = value.find("syscalls");
  if (syscalls == nullptr || !syscalls->isObject()) {
    return err(DiagCode::ParseError, "syscall table: missing 'syscalls' object");
  }

  SyscallTable table;
  table.arch_ = value.stringAt("arch").value_or("");
  for (const json::Member& member : syscalls->members()) {
    uint32_t number = 0;
    const char* first = member.first.data();
    const char* last = first + member.first.size();
    const auto parsed = std::from_chars(first, last, number);
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
      return err(DiagCode::ParseError,
                 std::format("syscall table: '{}' is not a syscall number", member.first));
    }
    const json::Value& entry = member.second;
    SyscallInfo info;
    info.number = number;
    info.name = entry.stringAt("name").value_or("");
    if (info.name.empty()) {
      return err(DiagCode::ParseError,
                 std::format("syscall table: entry {} has no name", number));
    }
    if (const json::Value* args = entry.find("args"); args != nullptr && args->isArray()) {
      for (const json::Value& arg : args->items()) {
        info.argTypes.push_back(arg.asString());
      }
      info.argCount = static_cast<unsigned>(info.argTypes.size());
    }
    if (const std::optional<int64_t> argc = entry.intAt("argc"); argc.has_value()) {
      if (*argc < 0 || *argc > 6) {
        return err(DiagCode::ParseError,
                   std::format("syscall table: entry {} has argc {}, outside 0..6", number,
                               *argc));
      }
      info.argCount = static_cast<unsigned>(*argc);
    }
    if (info.hasSignature() && info.argTypes.size() != info.argCount) {
      return err(DiagCode::ParseError,
                 std::format("syscall table: entry {} lists {} argument type(s) but argc {}",
                             number, info.argTypes.size(), info.argCount));
    }
    info.returnType = entry.stringAt("ret").value_or("");
    info.noreturn = entry.boolAt("noreturn").value_or(false);
    table.byNumber_.insert_or_assign(number, std::move(info));
  }
  return table;
}

Result<SyscallTable> SyscallTable::loadFile(const std::string& path) {
  XDEC_TRY(const json::Value document, json::parseFile(path));
  Result<SyscallTable> table = fromJson(document);
  if (!table) {
    return err(Diag{table.error()}.note(std::format("while reading '{}'", path)));
  }
  return table;
}

Result<std::string> SyscallTable::resolvePath(std::string_view nameOrPath) {
  namespace fs = std::filesystem;
  const fs::path given{nameOrPath};
  if (fs::exists(given)) {
    return given.string();
  }
  if (given.has_parent_path() || given.has_extension()) {
    return err(DiagCode::IoError, std::format("no such syscall table '{}'", nameOrPath));
  }
  const fs::path shipped =
      fs::path{XDEC_TYPES_DIR} / "syscall" / (std::string{nameOrPath} + ".json");
  if (fs::exists(shipped)) {
    return shipped.string();
  }
  return err(DiagCode::IoError,
             std::format("no syscall table named '{}' (looked in '{}')", nameOrPath,
                         shipped.string()));
}

const SyscallInfo* SyscallTable::find(uint32_t number) const noexcept {
  const auto found = byNumber_.find(number);
  return found == byNumber_.end() ? nullptr : &found->second;
}

}  // namespace xdec::types
