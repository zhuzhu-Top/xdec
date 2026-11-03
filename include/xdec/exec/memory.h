// Permissioned, page-based guest memory for concrete execution.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xdec/il/interp.h"
#include "xdec/support/result.h"

namespace xdec::binary {
class BinaryImage;
}

namespace xdec::exec {

enum class MemoryPermission : uint8_t {
  None = 0,
  Read = 1u << 0,
  Write = 1u << 1,
  Execute = 1u << 2,
};

[[nodiscard]] constexpr MemoryPermission operator|(MemoryPermission lhs,
                                                   MemoryPermission rhs) noexcept {
  return static_cast<MemoryPermission>(static_cast<uint8_t>(lhs) |
                                       static_cast<uint8_t>(rhs));
}

[[nodiscard]] constexpr bool hasPermission(MemoryPermission permissions,
                                           MemoryPermission query) noexcept {
  return (static_cast<uint8_t>(permissions) & static_cast<uint8_t>(query)) ==
         static_cast<uint8_t>(query);
}

struct MemoryRange {
  uint64_t address = 0;
  uint64_t size = 0;
};

struct MemoryAccess {
  uint64_t instruction = 0;
  uint64_t address = 0;
  unsigned size = 0;
  bool write = false;
  il::ConcreteValue value;
  uint64_t contextAddress = 0;
  std::vector<std::byte> context;
};

struct PageSeed {
  MemoryPermission permissions = MemoryPermission::None;
  std::array<std::byte, il::ExecMemory::kPageSize> bytes{};
};

using PageProvider =
    std::function<Result<std::optional<PageSeed>>(uint64_t pageAddress)>;
using MemoryAccessObserver = std::function<void(const MemoryAccess&)>;
using AddressCanonicalizer = std::function<uint64_t(uint64_t address)>;

struct MemoryPatch {
  enum class Kind : uint8_t {
    Write,
    Map,
    Protect,
    Unmap,
  };

  Kind kind = Kind::Write;
  uint64_t address = 0;
  uint64_t size = 0;
  MemoryPermission permissions = MemoryPermission::None;
  std::vector<std::byte> bytes;

  static MemoryPatch write(uint64_t address, std::vector<std::byte> bytes);
  static MemoryPatch map(uint64_t address, uint64_t size,
                         MemoryPermission permissions,
                         std::vector<std::byte> initial = {});
  static MemoryPatch protect(uint64_t address, uint64_t size,
                             MemoryPermission permissions);
  static MemoryPatch unmap(uint64_t address, uint64_t size);
};

/// The session-owned guest address space.
///
/// Reads and writes enforce permissions. A page provider may populate a missing
/// page, but never silently turns a provider failure into zero-filled memory.
class GuestAddressSpace final : public il::ExecMemoryBackend {
 public:
  static constexpr uint64_t kPageSize = il::ExecMemory::kPageSize;

  explicit GuestAddressSpace(AddressCanonicalizer canonicalizer = {})
      : canonicalizer_(std::move(canonicalizer)) {}

  [[nodiscard]] Result<void> map(uint64_t address, uint64_t size,
                                 MemoryPermission permissions);
  [[nodiscard]] Result<void> seed(uint64_t address, std::span<const std::byte> bytes,
                                  MemoryPermission permissions);
  [[nodiscard]] Result<void> seedImage(const binary::BinaryImage& image);
  [[nodiscard]] Result<void> protect(uint64_t address, uint64_t size,
                                     MemoryPermission permissions);
  [[nodiscard]] Result<void> unmap(uint64_t address, uint64_t size);

  void setPageProvider(PageProvider provider) { provider_ = std::move(provider); }
  void setAccessObserver(MemoryAccessObserver observer) {
    observer_ = std::move(observer);
  }
  void setInstructionContext(uint64_t pc) noexcept { instruction_ = pc; }

  [[nodiscard]] bool mapped(uint64_t address, uint64_t size) const override;
  [[nodiscard]] Result<il::ConcreteValue> read(uint64_t address,
                                               unsigned bytes) const override;
  [[nodiscard]] Result<void> write(uint64_t address, unsigned bytes,
                                   il::ConcreteValue value) override;

  [[nodiscard]] Result<void> readBytes(uint64_t address,
                                       std::span<std::byte> out,
                                       MemoryPermission permission =
                                           MemoryPermission::Read) const;
  /// Reads only pages already resident, never asking the page provider to
  /// materialize one, and reports failure rather than faulting. Observational
  /// reads use this: capturing the bytes around an access to describe it must
  /// not itself fetch memory, which would cost a round trip per access and
  /// pull in pages the program never touched. Permissions are not checked --
  /// looking at a page is not the program reading it.
  [[nodiscard]] bool readResidentBytes(uint64_t address,
                                       std::span<std::byte> out) const;
  [[nodiscard]] Result<void> writeBytes(uint64_t address,
                                        std::span<const std::byte> bytes);
  [[nodiscard]] Result<void> validate(
      std::span<const MemoryPatch> patches) const;
  [[nodiscard]] Result<void> apply(std::span<const MemoryPatch> patches);

  /// Overwrites already-resident pages covering [address, address+size) with
  /// the page provider's current bytes. Missing pages are left alone: their
  /// next access fetches them fresh anyway. This exists for memory the device
  /// wrote outside anything the trace modelled -- a run-loop callout's Mach
  /// message, delivered by the kernel while the guest was parked -- where a
  /// page cached in an earlier phase would otherwise be read as if it were the
  /// guest's own state. Without a provider it does nothing.
  [[nodiscard]] Result<void> refresh(uint64_t address, uint64_t size);

  /// Every byte written through writeBytes since the last clearDirty, as
  /// sorted, coalesced ranges. Bytes on pages unmapped since then are gone
  /// with the page and are not reported.
  [[nodiscard]] std::vector<MemoryRange> dirtyRanges() const;
  void clearDirty();
  [[nodiscard]] uint64_t generationAt(uint64_t address) const;

 private:
  static constexpr std::size_t kDirtyWordBits = 64;
  static_assert(kPageSize % kDirtyWordBits == 0);
  static constexpr std::size_t kDirtyWords = kPageSize / kDirtyWordBits;

  struct Page {
    std::array<std::byte, kPageSize> bytes{};
    MemoryPermission permissions = MemoryPermission::None;
    uint64_t generation = 0;
    /// One bit per byte written since the last clearDirty. Marking a write
    /// costs the same however much has been written before it, which a log of
    /// written addresses cannot offer: that log grows with the trace, and
    /// every operation on it grew with it.
    std::array<uint64_t, kDirtyWords> dirty{};
    /// Whether this page is already in dirtyPages_, so it is listed once.
    bool dirtyListed = false;
  };

  /// The page the previous access resolved. Accesses are overwhelmingly local
  /// -- a stack frame, a structure, a buffer being filled -- so most of them
  /// skip the hash lookup. A copy starts empty rather than pointing into the
  /// source's map.
  struct PageCache {
    static constexpr uint64_t kNoPage = 1;  // never page-aligned
    uint64_t base = kNoPage;
    Page* page = nullptr;

    PageCache() = default;
    PageCache(const PageCache&) noexcept {}
    PageCache& operator=(const PageCache&) noexcept {
      forget();
      return *this;
    }
    void forget() noexcept {
      base = kNoPage;
      page = nullptr;
    }
  };

  [[nodiscard]] uint64_t freshGeneration() const noexcept {
    return nextGeneration_++;
  }
  [[nodiscard]] uint64_t canonicalize(uint64_t address) const {
    return canonicalizer_ ? canonicalizer_(address) : address;
  }
  [[nodiscard]] Result<void> ensure(uint64_t address, uint64_t size,
                                    MemoryPermission permission) const;
  /// The page at `page` (a page base), materialized from the provider when it
  /// is missing and checked for `permission`. The single lookup every access
  /// path goes through.
  [[nodiscard]] Result<Page*> residentPage(uint64_t page,
                                           MemoryPermission permission) const;
  /// The resident page at `page` (a page base), or null. Never fetches.
  [[nodiscard]] Page* lookup(uint64_t page) const;
  [[nodiscard]] Page* pageAt(uint64_t address);
  [[nodiscard]] const Page* pageAt(uint64_t address) const;
  void markDirty(Page& page, uint64_t base, std::size_t offset,
                 std::size_t size);

  mutable std::unordered_map<uint64_t, Page> pages_;
  mutable PageCache cache_;
  AddressCanonicalizer canonicalizer_;
  PageProvider provider_;
  mutable MemoryAccessObserver observer_;
  uint64_t instruction_ = 0;
  /// Pages with at least one dirty bit, in the order they became dirty. May
  /// name a page that was unmapped, or name one twice if it was unmapped and
  /// mapped again; readers skip the former and deduplicate the latter.
  std::vector<uint64_t> dirtyPages_;
  mutable uint64_t nextGeneration_ = 1;
};

}  // namespace xdec::exec
