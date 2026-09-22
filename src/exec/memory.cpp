#include "xdec/exec/memory.h"

#include <algorithm>
#include <format>
#include <limits>
#include <utility>

#include "xdec/binary/image.h"

namespace xdec::exec {
namespace {

constexpr uint64_t pageBase(uint64_t address) {
  return address & ~(GuestAddressSpace::kPageSize - 1);
}

MemoryPermission convert(binary::MemoryPermissions permissions) {
  MemoryPermission result = MemoryPermission::None;
  if (binary::hasPermission(permissions, binary::MemoryPermissions::Read)) {
    result = result | MemoryPermission::Read;
  }
  if (binary::hasPermission(permissions, binary::MemoryPermissions::Write)) {
    result = result | MemoryPermission::Write;
  }
  if (binary::hasPermission(permissions, binary::MemoryPermissions::Execute)) {
    result = result | MemoryPermission::Execute;
  }
  return result;
}

}  // namespace

MemoryPatch MemoryPatch::write(uint64_t address,
                               std::vector<std::byte> bytes) {
  MemoryPatch patch;
  patch.kind = Kind::Write;
  patch.address = address;
  patch.size = bytes.size();
  patch.bytes = std::move(bytes);
  return patch;
}

MemoryPatch MemoryPatch::map(uint64_t address, uint64_t size,
                             MemoryPermission permissions,
                             std::vector<std::byte> initial) {
  MemoryPatch patch;
  patch.kind = Kind::Map;
  patch.address = address;
  patch.size = size;
  patch.permissions = permissions;
  patch.bytes = std::move(initial);
  return patch;
}

MemoryPatch MemoryPatch::protect(uint64_t address, uint64_t size,
                                 MemoryPermission permissions) {
  MemoryPatch patch;
  patch.kind = Kind::Protect;
  patch.address = address;
  patch.size = size;
  patch.permissions = permissions;
  return patch;
}

MemoryPatch MemoryPatch::unmap(uint64_t address, uint64_t size) {
  MemoryPatch patch;
  patch.kind = Kind::Unmap;
  patch.address = address;
  patch.size = size;
  return patch;
}

Result<void> GuestAddressSpace::map(uint64_t address, uint64_t size,
                                    MemoryPermission permissions) {
  address = canonicalize(address);
  if (size == 0) {
    return ok();
  }
  if (address > std::numeric_limits<uint64_t>::max() - (size - 1)) {
    return err(DiagCode::OutOfRange, "guest mapping wraps the address space");
  }
  const uint64_t first = pageBase(address);
  const uint64_t last = pageBase(address + size - 1);
  for (uint64_t page = first;; page += kPageSize) {
    if (pages_.contains(page)) {
      return err(Diag{DiagCode::BadFormat, "guest mapping overlaps an existing page"}
                     .at(page));
    }
    if (page == last) {
      break;
    }
  }
  for (uint64_t page = first;; page += kPageSize) {
    auto [it, inserted] = pages_.try_emplace(page);
    (void)inserted;
    it->second.permissions = permissions;
    it->second.generation = freshGeneration();
    if (page == last) {
      break;
    }
  }
  return ok();
}

Result<void> GuestAddressSpace::seed(uint64_t address,
                                     std::span<const std::byte> bytes,
                                     MemoryPermission permissions) {
  address = canonicalize(address);
  if (bytes.empty()) {
    return ok();
  }
  if (address >
      std::numeric_limits<uint64_t>::max() - (bytes.size() - 1)) {
    return err(DiagCode::OutOfRange, "guest seed wraps the address space");
  }
  const uint64_t first = pageBase(address);
  const uint64_t last = pageBase(address + bytes.size() - 1);
  for (uint64_t page = first;; page += kPageSize) {
    auto [it, inserted] = pages_.try_emplace(page);
    if (inserted) {
      it->second.permissions = permissions;
    } else {
      it->second.permissions = it->second.permissions | permissions;
    }
    it->second.generation = freshGeneration();
    if (page == last) {
      break;
    }
  }
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    Page* page = pageAt(address + index);
    page->bytes[(address + index) & (kPageSize - 1)] = bytes[index];
  }
  return ok();
}

Result<void> GuestAddressSpace::seedImage(const binary::BinaryImage& image) {
  std::array<std::byte, kPageSize> buffer{};
  for (const binary::MemoryRegion& region : image.memory().regions()) {
    uint64_t offset = 0;
    while (offset < region.size) {
      const uint64_t chunk =
          std::min<uint64_t>(buffer.size(), region.size - offset);
      XDEC_TRY_VOID(image.read(region.va + offset,
                               std::span<std::byte>{buffer}.first(chunk)));
      XDEC_TRY_VOID(seed(region.va + offset,
                         std::span<const std::byte>{buffer}.first(chunk),
                         convert(region.permissions)));
      offset += chunk;
    }
  }
  clearDirty();
  return ok();
}

Result<void> GuestAddressSpace::protect(uint64_t address, uint64_t size,
                                        MemoryPermission permissions) {
  address = canonicalize(address);
  XDEC_TRY_VOID(ensure(address, size, MemoryPermission::None));
  if (size == 0) {
    return ok();
  }
  const uint64_t first = pageBase(address);
  const uint64_t last = pageBase(address + size - 1);
  for (uint64_t page = first;; page += kPageSize) {
    Page& mappedPage = pages_.at(page);
    mappedPage.permissions = permissions;
    mappedPage.generation = freshGeneration();
    if (page == last) {
      break;
    }
  }
  return ok();
}

Result<void> GuestAddressSpace::unmap(uint64_t address, uint64_t size) {
  address = canonicalize(address);
  if (size == 0) {
    return ok();
  }
  XDEC_TRY_VOID(ensure(address, size, MemoryPermission::None));
  const uint64_t first = pageBase(address);
  const uint64_t last = pageBase(address + size - 1);
  for (uint64_t page = first;; page += kPageSize) {
    pages_.erase(page);
    if (page == last) {
      break;
    }
  }
  return ok();
}

bool GuestAddressSpace::mapped(uint64_t address, uint64_t size) const {
  address = canonicalize(address);
  if (size == 0) {
    return true;
  }
  if (address > std::numeric_limits<uint64_t>::max() - (size - 1)) {
    return false;
  }
  const uint64_t last = pageBase(address + size - 1);
  for (uint64_t page = pageBase(address);; page += kPageSize) {
    if (!pages_.contains(page)) {
      return false;
    }
    if (page == last) {
      return true;
    }
  }
}

Result<void> GuestAddressSpace::ensure(uint64_t address, uint64_t size,
                                       MemoryPermission permission) const {
  address = canonicalize(address);
  if (size == 0) {
    return ok();
  }
  if (address > std::numeric_limits<uint64_t>::max() - (size - 1)) {
    return err(DiagCode::OutOfRange, "guest memory access wraps the address space");
  }
  const uint64_t last = pageBase(address + size - 1);
  for (uint64_t page = pageBase(address);; page += kPageSize) {
    auto found = pages_.find(page);
    if (found == pages_.end() && provider_) {
      XDEC_TRY(auto supplied, provider_(page));
      if (supplied.has_value()) {
        Page materialized;
        materialized.bytes = supplied->bytes;
        materialized.permissions = supplied->permissions;
        materialized.generation = freshGeneration();
        found = pages_.emplace(page, std::move(materialized)).first;
      }
    }
    if (found == pages_.end()) {
      return err(Diag{DiagCode::UnmappedAddress, "guest memory page is unmapped"}
                     .at(page));
    }
    if (permission != MemoryPermission::None &&
        !hasPermission(found->second.permissions, permission)) {
      return err(Diag{DiagCode::OutOfRange,
                      std::format("guest page lacks {} permission",
                                  permission == MemoryPermission::Read
                                      ? "read"
                                      : permission == MemoryPermission::Write ? "write"
                                                                             : "execute")}
                     .at(page));
    }
    if (page == last) {
      break;
    }
  }
  return ok();
}

Result<void> GuestAddressSpace::readBytes(uint64_t address,
                                          std::span<std::byte> out,
                                          MemoryPermission permission) const {
  address = canonicalize(address);
  XDEC_TRY_VOID(ensure(address, out.size(), permission));
  for (std::size_t index = 0; index < out.size(); ++index) {
    const Page* page = pageAt(address + index);
    out[index] = page->bytes[(address + index) & (kPageSize - 1)];
  }
  return ok();
}

bool GuestAddressSpace::readResidentBytes(uint64_t address,
                                          std::span<std::byte> out) const {
  address = canonicalize(address);
  if (out.empty()) {
    return true;
  }
  if (address > std::numeric_limits<uint64_t>::max() - (out.size() - 1)) {
    return false;
  }
  const uint64_t last = pageBase(address + out.size() - 1);
  for (uint64_t page = pageBase(address);; page += kPageSize) {
    if (!pages_.contains(page)) {
      return false;
    }
    if (page == last) {
      break;
    }
  }
  for (std::size_t index = 0; index < out.size(); ++index) {
    const Page* page = pageAt(address + index);
    out[index] = page->bytes[(address + index) & (kPageSize - 1)];
  }
  return true;
}

Result<void> GuestAddressSpace::writeBytes(uint64_t address,
                                           std::span<const std::byte> bytes) {
  address = canonicalize(address);
  if (bytes.empty()) {
    return ok();
  }
  XDEC_TRY_VOID(ensure(address, bytes.size(), MemoryPermission::Write));
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    const uint64_t va = address + index;
    Page* page = pageAt(va);
    page->bytes[va & (kPageSize - 1)] = bytes[index];
    dirtyBytes_.push_back(va);
  }
  if (dirtyBytes_.size() >= dirtyCompactAt_) {
    compactDirty();
  }
  const uint64_t last = pageBase(address + bytes.size() - 1);
  for (uint64_t page = pageBase(address);; page += kPageSize) {
    pages_.at(page).generation = freshGeneration();
    if (page == last) {
      break;
    }
  }
  return ok();
}

Result<il::ConcreteValue> GuestAddressSpace::read(uint64_t address,
                                                  unsigned bytes) const {
  if (bytes == 0 || bytes > 16) {
    return err(DiagCode::Internal, "memory read of {} bytes", bytes);
  }
  std::array<std::byte, 16> storage{};
  XDEC_TRY_VOID(readBytes(address, std::span<std::byte>{storage}.first(bytes)));
  il::ConcreteValue value{};
  for (unsigned index = 0; index < bytes; ++index) {
    const uint64_t byte = static_cast<uint64_t>(storage[index]);
    if (index < 8) {
      value.lo |= byte << (index * 8);
    } else {
      value.hi |= byte << ((index - 8) * 8);
    }
  }
  if (observer_) {
    MemoryAccess access;
    access.instruction = instruction_;
    access.address = address;
    access.size = bytes;
    access.value = value;
    observer_(access);
  }
  return value;
}

Result<void> GuestAddressSpace::write(uint64_t address, unsigned bytes,
                                      il::ConcreteValue value) {
  if (bytes == 0 || bytes > 16) {
    return err(DiagCode::Internal, "memory write of {} bytes", bytes);
  }
  std::array<std::byte, 16> storage{};
  for (unsigned index = 0; index < bytes; ++index) {
    const uint64_t byte = index < 8 ? value.lo >> (index * 8)
                                    : value.hi >> ((index - 8) * 8);
    storage[index] = static_cast<std::byte>(byte & 0xFF);
  }
  XDEC_TRY_VOID(writeBytes(address, std::span<const std::byte>{storage}.first(bytes)));
  if (observer_) {
    MemoryAccess access;
    access.instruction = instruction_;
    access.address = address;
    access.size = bytes;
    access.write = true;
    access.value = value;
    observer_(access);
  }
  return ok();
}

Result<void> GuestAddressSpace::validate(
    std::span<const MemoryPatch> patches) const {
  std::unordered_map<uint64_t, std::optional<MemoryPermission>> overlay;
  const auto permissionAt = [&](uint64_t page)
      -> std::optional<MemoryPermission> {
    if (const auto changed = overlay.find(page); changed != overlay.end()) {
      return changed->second;
    }
    if (const auto existing = pages_.find(page); existing != pages_.end()) {
      return existing->second.permissions;
    }
    return std::nullopt;
  };

  for (const MemoryPatch& patch : patches) {
    const uint64_t address = canonicalize(patch.address);
    const uint64_t size =
        patch.kind == MemoryPatch::Kind::Write ? patch.bytes.size() : patch.size;
    if (patch.kind == MemoryPatch::Kind::Map && patch.bytes.size() > size) {
      return err(DiagCode::BadFormat,
                 "initial mapping bytes exceed the mapped size");
    }
    if (size == 0) {
      continue;
    }
    if (address > std::numeric_limits<uint64_t>::max() - (size - 1)) {
      return err(DiagCode::OutOfRange,
                 "memory patch wraps the guest address space");
    }
    const uint64_t last = pageBase(address + size - 1);
    for (uint64_t page = pageBase(address);; page += kPageSize) {
      std::optional<MemoryPermission> current = permissionAt(page);
      switch (patch.kind) {
        case MemoryPatch::Kind::Write:
          if (!current.has_value()) {
            const auto materialized =
                ensure(page, kPageSize, MemoryPermission::Write);
            if (materialized) current = permissionAt(page);
          }
          if (!current.has_value() ||
              !hasPermission(*current, MemoryPermission::Write)) {
            return err(Diag{DiagCode::OutOfRange,
                            std::format(
                                "memory patch writes an unmapped or read-only "
                                "page (write=0x{:x}, size=0x{:x})",
                                patch.address, size)}
                           .at(page));
          }
          break;
        case MemoryPatch::Kind::Map:
          if (current.has_value()) {
            return err(Diag{DiagCode::BadFormat,
                            "memory patch maps an existing page"}
                           .at(page));
          }
          overlay[page] = patch.permissions;
          break;
        case MemoryPatch::Kind::Protect:
          if (!current.has_value()) {
            return err(Diag{DiagCode::UnmappedAddress,
                            "memory patch protects an unmapped page"}
                           .at(page));
          }
          overlay[page] = patch.permissions;
          break;
        case MemoryPatch::Kind::Unmap:
          if (!current.has_value()) {
            return err(Diag{DiagCode::UnmappedAddress,
                            "memory patch unmaps an unmapped page"}
                           .at(page));
          }
          overlay[page] = std::nullopt;
          break;
      }
      if (page == last) {
        break;
      }
    }
  }
  return ok();
}

Result<void> GuestAddressSpace::apply(std::span<const MemoryPatch> patches) {
  XDEC_TRY_VOID(validate(patches));
  for (const MemoryPatch& patch : patches) {
    switch (patch.kind) {
      case MemoryPatch::Kind::Write:
        XDEC_TRY_VOID(writeBytes(patch.address, patch.bytes));
        break;
      case MemoryPatch::Kind::Map: {
        if (patch.bytes.size() > patch.size) {
          return err(DiagCode::BadFormat,
                     "initial mapping bytes exceed the mapped size");
        }
        XDEC_TRY_VOID(map(patch.address, patch.size, patch.permissions));
        const uint64_t address = canonicalize(patch.address);
        for (std::size_t index = 0; index < patch.bytes.size(); ++index) {
          Page* page = pageAt(address + index);
          page->bytes[(address + index) & (kPageSize - 1)] =
              patch.bytes[index];
        }
        break;
      }
      case MemoryPatch::Kind::Protect:
        XDEC_TRY_VOID(protect(patch.address, patch.size, patch.permissions));
        break;
      case MemoryPatch::Kind::Unmap:
        XDEC_TRY_VOID(unmap(patch.address, patch.size));
        break;
    }
  }
  return ok();
}

void GuestAddressSpace::compactDirty() {
  std::sort(dirtyBytes_.begin(), dirtyBytes_.end());
  dirtyBytes_.erase(std::unique(dirtyBytes_.begin(), dirtyBytes_.end()),
                    dirtyBytes_.end());
  // Leaving headroom proportional to what survived keeps compaction amortized:
  // a session whose distinct set already sits at the threshold would otherwise
  // re-sort on nearly every write.
  dirtyCompactAt_ = std::max(kDirtyCompactFloor, dirtyBytes_.size() * 2);
}

std::vector<MemoryRange> GuestAddressSpace::dirtyRanges() const {
  if (dirtyBytes_.empty()) {
    return {};
  }
  std::vector<uint64_t> sorted = dirtyBytes_;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  std::vector<MemoryRange> ranges;
  uint64_t start = sorted.front();
  uint64_t previous = start;
  for (const uint64_t address : sorted) {
    if (address > previous + 1) {
      ranges.push_back(MemoryRange{start, previous - start + 1});
      start = address;
    }
    previous = address;
  }
  ranges.push_back(MemoryRange{start, previous - start + 1});
  return ranges;
}

uint64_t GuestAddressSpace::generationAt(uint64_t address) const {
  const Page* page = pageAt(canonicalize(address));
  return page == nullptr ? 0 : page->generation;
}

GuestAddressSpace::Page* GuestAddressSpace::pageAt(uint64_t address) {
  const auto found = pages_.find(pageBase(address));
  return found == pages_.end() ? nullptr : &found->second;
}

const GuestAddressSpace::Page* GuestAddressSpace::pageAt(uint64_t address) const {
  const auto found = pages_.find(pageBase(address));
  return found == pages_.end() ? nullptr : &found->second;
}

}  // namespace xdec::exec
