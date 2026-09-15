// Permissioned, page-based guest memory for concrete execution.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
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
  [[nodiscard]] Result<void> writeBytes(uint64_t address,
                                        std::span<const std::byte> bytes);
  [[nodiscard]] Result<void> validate(
      std::span<const MemoryPatch> patches) const;
  [[nodiscard]] Result<void> apply(std::span<const MemoryPatch> patches);

  [[nodiscard]] std::vector<MemoryRange> dirtyRanges() const;
  void clearDirty() { dirtyBytes_.clear(); }
  [[nodiscard]] uint64_t generationAt(uint64_t address) const;

 private:
  struct Page {
    std::array<std::byte, kPageSize> bytes{};
    MemoryPermission permissions = MemoryPermission::None;
    uint64_t generation = 0;
  };

  [[nodiscard]] uint64_t freshGeneration() const noexcept {
    return nextGeneration_++;
  }
  [[nodiscard]] Result<void> ensure(uint64_t address, uint64_t size,
                                    MemoryPermission permission) const;
  [[nodiscard]] Page* pageAt(uint64_t address);
  [[nodiscard]] const Page* pageAt(uint64_t address) const;

  mutable std::unordered_map<uint64_t, Page> pages_;
  PageProvider provider_;
  mutable MemoryAccessObserver observer_;
  uint64_t instruction_ = 0;
  std::vector<uint64_t> dirtyBytes_;
  mutable uint64_t nextGeneration_ = 1;
};

}  // namespace xdec::exec
