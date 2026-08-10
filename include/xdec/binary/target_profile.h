// TargetProfile: what platform a binary is for, inferred rather than asked.
//
// `--types android-ndk` on every invocation is a parameter a user should
// never have to type twice: the binary itself already says it is an AArch64
// ELF, and on this project's one supported platform that fact alone decides
// which header preset, which syscall ABI, and which loader-name-to-header-name
// aliases apply (Bionic's `__errno` is the NDK header's `__errno_location`;
// see docs/10-import-resolution.md). This is the single place that inference
// lives, so a second platform (iOS: Mach-O + AArch64) is one more branch here,
// not a CLI flag stacked on top of the first one's.
//
// Deliberately declared in `xdec::binary`, not `xdec::types`, even though its
// fields describe type-import choices: the struct itself is plain strings,
// answerable from `BinaryImage` alone, and putting it here (mirroring
// support/target.h's own reasoning for `Arch`) means a consumer that only
// needs the struct -- analysis::calleeThroughImportSlot's alias lookup, say --
// never has to depend on the type system just to know a type preset's name.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace xdec::binary {

class BinaryImage;

/// What a target platform implies for type import and call resolution, all of
/// it named data a caller applies rather than a policy this struct enforces
/// itself.
struct TargetProfile {
  /// Header presets to load, in `--types` order, when the user gave none of
  /// their own. Empty when the platform is not one this project has a preset
  /// for yet -- then the pipeline runs exactly as it did before this existed.
  std::vector<std::string> typePresets;
  /// The syscall table to load by default, empty to defer to whatever the
  /// caller already falls back to (see types::SyscallTable::defaultName).
  std::string syscallTable;
  /// Loader/dynsym spelling -> header spelling, for the handful of libc
  /// entry points whose exported symbol name and declared name differ
  /// (Bionic's `__errno` vs the NDK header's `__errno_location`). Consulted
  /// after a name is resolved and before it is bound against a TypeDatabase;
  /// a name with no entry here passes through unchanged.
  std::map<std::string, std::string> symbolAliases;
};

/// Infers a `TargetProfile` from what the image itself says: format,
/// architecture, and (once a second platform is supported) its ABI markers.
/// Today this recognises exactly one shape -- AArch64 ELF, i.e. Android
/// native code, this project's only supported target -- and returns a default
/// (empty) profile for everything else, which is the same "no opinion, run as
/// before" behaviour the pipeline had when this function did not exist.
[[nodiscard]] TargetProfile inferTargetProfile(const BinaryImage& image);

}  // namespace xdec::binary
