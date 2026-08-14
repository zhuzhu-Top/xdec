// CachePointerDecoder: tagged 64-bit table entries seen in dyld shared cache
// dispatch tables.
//
// Empirical, not a documented Apple format: reading
// `dyld_shared_cache_arm64`'s own on-disk bytes at a jump table an
// obfuscated `com.apple.absd` dispatch function indexes (see
// docs/22-dyld-shared-cache.md) gives values like `0x20192464723` where the
// low 36 bits (`0x192464723`) are exactly the function's own address and the
// high bits (`0x2`, `0x4`, `0x401`, ...) carry no fixed meaning this project
// has decoded -- they are not a PAC diversifier, not an arm64e chained-
// pointer auth field (those have a different bit layout entirely, see
// dyld_cache_slide_info3/5 in dyld_cache_format.h), and they are not the
// dyld cache's own address space at all: sharedRegionStart fits in 33 bits,
// so a real cache pointer never needs 36. Whatever emitted this table (the
// same custom obfuscator this project already has "-2 thunk"-shaped
// evidence for, from the standalone absd binary) chose 36 bits to leave
// headroom for its own per-entry tag, not because the cache needed it.
//
// Scope: this decodes *that* table shape, not a general "every arm64e
// shared-cache pointer" scheme. Nothing in this project applies it
// unconditionally -- see passes/resolve_indirect.cpp's entriesFor, which
// only retries a table entry through this decoder after the entry already
// failed to look like code as-is, and only keeps the decoded address if
// that one does look like code. A plain, already-valid pointer table is
// never touched.
#pragma once

#include <cstdint>

namespace xdec::binary {

class CachePointerDecoder {
 public:
  /// 36 low bits hold the address; the rest is the tag. Overridable per call
  /// site for the day a second table turns up with a different split --
  /// nothing so far has needed one.
  explicit constexpr CachePointerDecoder(unsigned valueBits = 36) noexcept
      : valueMask_(valueBits >= 64 ? ~uint64_t{0} : (uint64_t{1} << valueBits) - 1) {}

  [[nodiscard]] constexpr uint64_t decode(uint64_t raw) const noexcept { return raw & valueMask_; }
  [[nodiscard]] constexpr uint64_t tag(uint64_t raw) const noexcept { return raw & ~valueMask_; }

 private:
  uint64_t valueMask_;
};

}  // namespace xdec::binary
