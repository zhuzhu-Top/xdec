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
// reasoning this mirrors). What supplies a binding is the platform profile's
// own offset table (see binary::TargetProfile::entryRegOffsets/
// entryRegLiterals) -- formulas and literals true of the platform in
// general.
//
// Resolving a BasePlusOffset binding needs a companion image's base address
// (dyld's own, in the one case this exists for): SessionContext::open is
// where that image is actually opened and the base is learned, so this
// header only models the *fact*, not how it was obtained.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace xdec::analysis {

enum class EntryRegKind : uint8_t {
  /// No fact at all -- resolve() answers nullopt, same as before this
  /// existed. ImageEval treats such an EntryReg leaf as top, as it always has.
  Unknown,
  /// A fixed value, known outright (a platform literal like "the kernel
  /// hands this over as zero").
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

/// Register name -> its entry-time binding, resolved to a concrete address
/// wherever a companion's base is known. Built once per session from a
/// platform profile and the companion images that were actually opened (see
/// SessionContext::open); consulted by ImageEval and by the C emitter (see
/// COptions::entryRegs, CContext::entryRegs).
class EntryRegFacts {
 public:
  void setBinding(std::string regName, EntryRegBinding binding) {
    bindings_[std::move(regName)] = std::move(binding);
  }
  void setCompanionBase(std::string companionName, uint64_t base) {
    companionBases_[std::move(companionName)] = base;
  }

  [[nodiscard]] bool empty() const noexcept { return bindings_.empty(); }

  /// The binding recorded for `regName`, or nullptr when nothing was ever
  /// said about it (as opposed to Unknown, which a caller can also record
  /// explicitly to shadow a platform default -- both read the same here).
  [[nodiscard]] const EntryRegBinding* bindingFor(std::string_view regName) const;

  /// The concrete value `regName` holds at entry, or nullopt when its
  /// binding is Unknown/absent, or BasePlusOffset over a companion whose base
  /// was never resolved.
  [[nodiscard]] std::optional<uint64_t> resolve(std::string_view regName) const;

  [[nodiscard]] std::optional<uint64_t> companionBase(std::string_view name) const;

 private:
  std::unordered_map<std::string, EntryRegBinding> bindings_;
  std::unordered_map<std::string, uint64_t> companionBases_;
};

}  // namespace xdec::analysis
