// EntryRegFacts: what a register holds at function entry, when the platform
// (not the image) is the one that put it there.
//
// Most `EntryReg` leaves (see il/expr.h) are honestly unknown — a caller's
// argument register carries whatever the caller chose, and no static fact
// about the callee says what that is. A handful are different: on a Mach-O
// target, dyld's own `start()` leaves x21/x22 pointing at its own globals and
// the kernel leaves x28 as process-launch residue, all of it *before* the
// program's own entry ever runs (see docs/20-absd-entry-registers.md). An
// obfuscator reading one of those is not reading an unknowable argument, it
// is reading a fact about the platform xdec can simply be told.
//
// This is that fact, kept out of the IL on purpose (an `EntryReg` leaf still
// denotes exactly what it always did) and out of the CLI on purpose too:
// nobody should have to spell `--entry-regs`/`--platform-profile` on every
// invocation any more than they spell `--types ios-sdk` on every one of a
// Mach-O binary's (see binary::TargetProfile, whose one-inference-site
// reasoning this mirrors). What supplies a binding is, in order of
// preference:
//
//   1. A sidecar file next to the binary (see discoverEntrySidecar) --
//      per-binary, per-capture measurements, e.g. a device's kernel-handoff
//      residue in x28, which is real but not architecturally guaranteed.
//   2. The platform profile's own offset table (see
//      binary::TargetProfile::entryRegOffsets/entryRegLiterals) -- formulas
//      and literals true of the platform in general.
//
// Resolving a BasePlusOffset binding needs a companion image's base address
// (dyld's own, in the one case this exists for): SessionContext::open is
// where that image is actually opened and the base is learned, so this
// header only models the *fact*, not how it was obtained.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xdec/support/result.h"

namespace xdec::analysis {

enum class EntryRegKind : uint8_t {
  /// No fact at all -- resolve() answers nullopt, same as before this
  /// existed. ImageEval treats such an EntryReg leaf as top, as it always has.
  Unknown,
  /// A fixed value, known outright (a sidecar's measured x28, or a platform
  /// literal like "the kernel hands this over as zero").
  Literal,
  /// A companion image's own base plus a fixed offset (dyld's `ADRL
  /// sConfigBuffer`/`_NSConcreteStackBlock`, say). Resolves only once that
  /// companion's base is known -- see EntryRegFacts::setCompanionBase.
  BasePlusOffset,
};

struct EntryRegBinding {
  EntryRegKind kind = EntryRegKind::Unknown;
  uint64_t literal = 0;
  /// Matched against a name passed to setCompanionBase; empty for Literal.
  std::string companion;
  uint64_t offset = 0;

  [[nodiscard]] static EntryRegBinding fromLiteral(uint64_t value) noexcept {
    EntryRegBinding binding;
    binding.kind = EntryRegKind::Literal;
    binding.literal = value;
    return binding;
  }
  [[nodiscard]] static EntryRegBinding fromBase(std::string companionName,
                                                uint64_t regOffset) {
    EntryRegBinding binding;
    binding.kind = EntryRegKind::BasePlusOffset;
    binding.companion = std::move(companionName);
    binding.offset = regOffset;
    return binding;
  }
  [[nodiscard]] bool known() const noexcept { return kind != EntryRegKind::Unknown; }
};

/// One more image a sidecar names, beyond the binary being decompiled --
/// dyld, in this project's one case so far. Where to read its bytes, and
/// where it was mapped when the sidecar's own registers were captured.
struct EntryCompanion {
  std::string name;
  std::filesystem::path path;
  /// The address this image was loaded at, at capture time (see
  /// docs/20-absd-entry-registers.md §7.4's measured dyld base). Absent
  /// leaves the companion's own file-declared base to stand in for it, which
  /// is only correct when the file already reflects the addressing a
  /// binding's offset is meant to land on -- a raw same-session capture
  /// typically does, a stock extract typically does not.
  std::optional<uint64_t> runtimeBase;
};

/// A single captured fact about memory the image itself does not contain --
/// a byte range read off a live process at a specific address, at a specific
/// moment (typically alongside the EntryReg literal for the register that
/// pointed at it). Exists for the same reason EntryReg literals do: some
/// facts are only ever knowable by measurement, not by static analysis, and
/// a decompiler that refuses to accept a measured fact just leaves that
/// dependent chain unresolved forever instead of resolved-but-not-reproven.
///
/// The motivating case is a shared-cache dispatch function indexing a jump
/// table by `regValue ^ hash(*regValue)`, where `regValue` (an argument
/// pointer, not a platform-leaked register) is itself only known from one
/// captured call -- see docs/22-dyld-shared-cache.md's target function. The
/// pointer's own value is an EntryReg literal (x0 = 0x...); what it *points
/// to* is a MemorySeed.
struct MemorySeed {
  uint64_t address = 0;
  uint64_t value = 0;
  /// Byte width of `value`; a Load whose width does not match this exactly
  /// is not answered by this seed (see EntryRegFacts::memoryValueAt).
  unsigned width = 8;
};

/// What a sidecar file says, before its companions are opened and its
/// literals are turned into bindings (see SessionContext::open).
struct EntrySidecar {
  /// Register name (e.g. "x28") -> a literal value, overriding whatever the
  /// platform profile would otherwise have said.
  std::unordered_map<std::string, uint64_t> literals;
  std::vector<EntryCompanion> companions;
  /// Captured memory bytes at fixed addresses (typically bytes an
  /// EntryReg-literal pointer read from during capture) -- see MemorySeed.
  std::vector<MemorySeed> memorySeeds;
};

/// `<binary path>.entry.json` next to the binary, if it exists. Not finding
/// one is not an error -- see docs/21-entry-reg-platform.md's schema and
/// SessionContext::open, which falls back to the platform profile alone.
[[nodiscard]] std::optional<std::filesystem::path> discoverEntrySidecar(
    const std::filesystem::path& binaryPath);

/// Parses a sidecar file (see docs/21-entry-reg-platform.md for the schema).
[[nodiscard]] Result<EntrySidecar> loadEntrySidecar(const std::filesystem::path& path);

/// Register name -> its entry-time binding, resolved to a concrete address
/// wherever a companion's base is known. Built once per session from a
/// platform profile, an optional sidecar, and the companion images that were
/// actually opened (see SessionContext::open); consulted by ImageEval and by
/// the C emitter (see COptions::entryRegs, CContext::entryRegs).
class EntryRegFacts {
 public:
  void setBinding(std::string regName, EntryRegBinding binding) {
    bindings_[std::move(regName)] = std::move(binding);
  }
  void setCompanionBase(std::string companionName, uint64_t base) {
    companionBases_[std::move(companionName)] = base;
  }
  void addMemorySeed(MemorySeed seed) { memorySeeds_.push_back(seed); }

  [[nodiscard]] bool empty() const noexcept {
    return bindings_.empty() && memorySeeds_.empty();
  }

  /// The binding recorded for `regName`, or nullptr when nothing was ever
  /// said about it (as opposed to Unknown, which a caller can also record
  /// explicitly to shadow a platform default -- both read the same here).
  [[nodiscard]] const EntryRegBinding* bindingFor(std::string_view regName) const;

  /// The concrete value `regName` holds at entry, or nullopt when its
  /// binding is Unknown/absent, or BasePlusOffset over a companion whose base
  /// was never resolved.
  [[nodiscard]] std::optional<uint64_t> resolve(std::string_view regName) const;

  [[nodiscard]] std::optional<uint64_t> companionBase(std::string_view name) const;

  /// A captured value for a `width`-byte read at `address`, or nullopt when
  /// no seed matches -- most callers (ImageEval::loadFrom) then fall back to
  /// reading the image's own bytes, exactly as if no seed existed. Width
  /// must match exactly: a seed captured as 8 bytes does not answer a 4-byte
  /// read at the same address, since silently truncating/widening a captured
  /// value is exactly the kind of guess this project's fail-loud rule
  /// exists to avoid.
  [[nodiscard]] std::optional<uint64_t> memoryValueAt(uint64_t address, unsigned width) const;

 private:
  std::unordered_map<std::string, EntryRegBinding> bindings_;
  std::unordered_map<std::string, uint64_t> companionBases_;
  std::vector<MemorySeed> memorySeeds_;
};

}  // namespace xdec::analysis
