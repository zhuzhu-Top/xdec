#include "xdec/types/syscall_table.h"

#include <charconv>
#include <filesystem>
#include <format>

#ifndef XDEC_TYPES_DIR
#define XDEC_TYPES_DIR "types"
#endif

namespace xdec::types {

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
