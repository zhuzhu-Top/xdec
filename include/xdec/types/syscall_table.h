// The syscall number -> name/signature table.
//
// An `svc #0` says nothing about which kernel entry point it reaches; the
// number is in x8 and the meaning of the number is a property of the kernel,
// not of the binary. So the mapping is data, shipped as
// `types/syscall/aarch64-linux.json` and overridable with `--syscall-table`,
// rather than a table compiled into the decompiler that would silently be
// wrong for a different ABI.
//
// Two levels of knowledge are represented, and the difference matters at the
// emitter. A number with `args` prints as a call with named, typed arguments.
// A number with only `argc` prints the right number of raw register values. A
// number that is not in the table at all prints as `__xdec_syscall(nr, ...)` —
// less useful, but never a guess.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "xdec/support/json.h"
#include "xdec/support/result.h"
#include "xdec/types/type.h"

namespace xdec::types {

class TypeDatabase;

struct SyscallInfo {
  uint32_t number = 0;
  std::string name;
  /// C spellings of the parameters, when the table records them. Empty means
  /// only the arity is known.
  std::vector<std::string> argTypes;
  /// How many of x0..x5 the kernel reads. Always known for a listed number.
  unsigned argCount = 0;
  /// C spelling of the return type; empty means `long`, which is what the
  /// raw syscall interface returns.
  std::string returnType;
  /// `exit`, `exit_group`, `rt_sigreturn`: control does not come back, so the
  /// emitter must not print an assignment of the result.
  bool noreturn = false;

  /// `argTypes`/`returnType` resolved against an imported TypeDatabase (see
  /// SyscallTable::resolveTypes), for passes that need an actual TypeId
  /// rather than a spelling to cast argument text with — propagate-types in
  /// particular. Invalid when no database was resolved against, or when the
  /// spelling named something the database never declared (a header-less run,
  /// or a tag like `struct timeval` no imported header defines): the table's
  /// spelling still prints in a comment, but nothing here can type it.
  std::vector<TypeId> argTypeIds;
  TypeId returnTypeId;

  [[nodiscard]] bool hasSignature() const noexcept { return !argTypes.empty(); }
};

class SyscallTable {
 public:
  /// An empty table. Every lookup misses, which is exactly the behaviour of a
  /// pipeline run without `--syscall-table`.
  SyscallTable() = default;

  [[nodiscard]] static Result<SyscallTable> fromJson(const json::Value& value);
  [[nodiscard]] static Result<SyscallTable> loadFile(const std::string& path);

  /// Resolves a name ("aarch64-linux") or a path. Mirrors resolveHeaderPath.
  [[nodiscard]] static Result<std::string> resolvePath(std::string_view nameOrPath);
  /// The table shipped for the architecture this project decompiles.
  [[nodiscard]] static constexpr std::string_view defaultName() noexcept {
    return "aarch64-linux";
  }

  [[nodiscard]] const SyscallInfo* find(uint32_t number) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return byNumber_.size(); }
  [[nodiscard]] bool empty() const noexcept { return byNumber_.empty(); }
  [[nodiscard]] const std::string& arch() const noexcept { return arch_; }

  /// Resolves every entry's `argTypes`/`returnType` spellings into `database`,
  /// filling in `argTypeIds`/`returnTypeId`. A no-op until this runs — the
  /// table on its own only ever held spellings, precisely so it could load
  /// (and this class stay independent of TypeDatabase) whether or not a
  /// `--types` header was given; a caller wires the two together once both are
  /// loaded, the same way TypeBinder connects a TypeDatabase to an image.
  void resolveTypes(const TypeDatabase& database);

 private:
  std::map<uint32_t, SyscallInfo> byNumber_;
  std::string arch_;
};

}  // namespace xdec::types
