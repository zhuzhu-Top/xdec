// The unified memory view.
//
// Every consumer of binary data goes through this, and it is deliberately the
// runtime view rather than the file view. A section-header-driven reader
// returns nothing for `.bss` because SHT_NOBITS has no file bytes; that is a
// bug, not a feature, because obfuscated code routinely branches on `.bss`
// globals whose true value at analysis time is zero. Here a region may declare
// `fileSize < size`, and the tail reads as zeros.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xdec/support/result.h"

namespace xdec::binary {

enum class MemoryPermissions : uint8_t {
  None = 0,
  Read = 1u << 0,
  Write = 1u << 1,
  Execute = 1u << 2,
};

[[nodiscard]] constexpr MemoryPermissions operator|(MemoryPermissions a,
                                                    MemoryPermissions b) noexcept {
  return static_cast<MemoryPermissions>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

[[nodiscard]] constexpr MemoryPermissions operator&(MemoryPermissions a,
                                                    MemoryPermissions b) noexcept {
  return static_cast<MemoryPermissions>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

constexpr MemoryPermissions& operator|=(MemoryPermissions& a, MemoryPermissions b) noexcept {
  a = a | b;
  return a;
}

[[nodiscard]] constexpr bool hasPermission(MemoryPermissions set,
                                           MemoryPermissions query) noexcept {
  return (static_cast<uint8_t>(set) & static_cast<uint8_t>(query)) != 0;
}

/// Renders as an `rwx` triple with `-` for absent bits.
[[nodiscard]] std::string toString(MemoryPermissions permissions);

struct MemoryRegion {
  /// Start of the region in the analysis address space (link-time addresses,
  /// i.e. a load bias of zero).
  uint64_t va = 0;
  /// In-memory size. Bytes in `[fileSize, size)` read as zero.
  uint64_t size = 0;
  /// Offset of the first byte in the backing buffer.
  uint64_t fileOffset = 0;
  /// Number of bytes actually backed by the file; never greater than `size`.
  uint64_t fileSize = 0;
  MemoryPermissions permissions = MemoryPermissions::None;
  /// Originating segment or section name; diagnostics only.
  std::string name;

  [[nodiscard]] uint64_t endVa() const noexcept { return va + size; }
  [[nodiscard]] bool contains(uint64_t address) const noexcept {
    return address >= va && address < va + size;
  }
};

class MemoryMap {
 public:
  MemoryMap() = default;

  /// The buffer that `fileOffset`/`fileSize` refer to. Must outlive the map and
  /// keep a stable address.
  void setBackingBytes(std::span<const std::byte> bytes) noexcept { backing_ = bytes; }

  void addRegion(MemoryRegion region);

  /// Sorts regions and validates them. Rejects overlapping regions, because an
  /// address with two possible values would make every downstream read
  /// ambiguous.
  Result<void> finalize();

  [[nodiscard]] std::span<const MemoryRegion> regions() const noexcept { return regions_; }
  [[nodiscard]] bool empty() const noexcept { return regions_.empty(); }

  [[nodiscard]] const MemoryRegion* regionAt(uint64_t va) const noexcept;
  [[nodiscard]] bool isMapped(uint64_t va) const noexcept { return regionAt(va) != nullptr; }

  [[nodiscard]] uint64_t lowestAddress() const noexcept;
  /// Exclusive upper bound of the mapped range.
  [[nodiscard]] uint64_t highestAddress() const noexcept;

  /// Fills `out` from the runtime view. Fails if any byte of the range falls
  /// outside every region; succeeds across a file-backed to zero-filled
  /// transition and across adjacent regions.
  Result<void> read(uint64_t va, std::span<std::byte> out) const;

  /// Zero-copy view, or an empty span when the range is not entirely backed by
  /// contiguous file bytes within a single region. Callers that get an empty
  /// span must fall back to `read`.
  [[nodiscard]] std::span<const std::byte> directView(uint64_t va, uint64_t size) const noexcept;

 private:
  std::span<const std::byte> backing_;
  std::vector<MemoryRegion> regions_;
  bool finalized_ = false;
};

}  // namespace xdec::binary
