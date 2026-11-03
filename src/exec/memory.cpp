#include "xdec/exec/memory.h"

#include <algorithm>
#include <bit>
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
  cache_.forget();
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
    XDEC_TRY_VOID(residentPage(page, permission));
    if (page == last) {
      break;
    }
  }
  return ok();
}

Result<GuestAddressSpace::Page*> GuestAddressSpace::residentPage(
    uint64_t page, MemoryPermission permission) const {
  Page* found = lookup(page);
  if (found == nullptr && provider_) {
    XDEC_TRY(auto supplied, provider_(page));
    if (supplied.has_value()) {
      Page materialized;
      materialized.bytes = supplied->bytes;
      materialized.permissions = supplied->permissions;
      materialized.generation = freshGeneration();
      found = &pages_.emplace(page, std::move(materialized)).first->second;
      cache_.base = page;
      cache_.page = found;
    }
  }
  if (found == nullptr) {
    return err(Diag{DiagCode::UnmappedAddress, "guest memory page is unmapped"}
                   .at(page));
  }
  if (permission != MemoryPermission::None &&
      !hasPermission(found->permissions, permission)) {
    return err(Diag{DiagCode::OutOfRange,
                    std::format("guest page lacks {} permission",
                                permission == MemoryPermission::Read
                                    ? "read"
                                    : permission == MemoryPermission::Write ? "write"
                                                                           : "execute")}
                   .at(page));
  }
  return found;
}

Result<void> GuestAddressSpace::readBytes(uint64_t address,
                                          std::span<std::byte> out,
                                          MemoryPermission permission) const {
  address = canonicalize(address);
  const std::size_t offset = static_cast<std::size_t>(address & (kPageSize - 1));
  if (!out.empty() && offset + out.size() <= kPageSize) {
    XDEC_TRY(const Page* page, residentPage(pageBase(address), permission));
    std::copy_n(page->bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                out.size(), out.begin());
    return ok();
  }
  XDEC_TRY_VOID(ensure(address, out.size(), permission));
  std::size_t copied = 0;
  while (copied < out.size()) {
    const uint64_t cursor = address + copied;
    const std::size_t pageOffset =
        static_cast<std::size_t>(cursor & (kPageSize - 1));
    const std::size_t chunk =
        std::min(out.size() - copied,
                 static_cast<std::size_t>(kPageSize) - pageOffset);
    const Page* page = pageAt(cursor);
    std::copy_n(page->bytes.begin() +
                    static_cast<std::ptrdiff_t>(pageOffset),
                chunk,
                out.begin() + static_cast<std::ptrdiff_t>(copied));
    copied += chunk;
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
    if (lookup(page) == nullptr) {
      return false;
    }
    if (page == last) {
      break;
    }
  }
  std::size_t copied = 0;
  while (copied < out.size()) {
    const uint64_t cursor = address + copied;
    const std::size_t pageOffset =
        static_cast<std::size_t>(cursor & (kPageSize - 1));
    const std::size_t chunk =
        std::min(out.size() - copied,
                 static_cast<std::size_t>(kPageSize) - pageOffset);
    const Page* page = pageAt(cursor);
    std::copy_n(page->bytes.begin() +
                    static_cast<std::ptrdiff_t>(pageOffset),
                chunk,
                out.begin() + static_cast<std::ptrdiff_t>(copied));
    copied += chunk;
  }
  return true;
}

Result<void> GuestAddressSpace::refresh(uint64_t address, uint64_t size) {
  if (size == 0 || !provider_) {
    return ok();
  }
  address = canonicalize(address);
  if (address > std::numeric_limits<uint64_t>::max() - (size - 1)) {
    return err(DiagCode::OutOfRange, "guest refresh wraps the address space");
  }
  const uint64_t last = pageBase(address + size - 1);
  for (uint64_t page = pageBase(address);; page += kPageSize) {
    if (Page* found = lookup(page); found != nullptr) {
      XDEC_TRY(auto supplied, provider_(page));
      if (supplied.has_value()) {
        found->bytes = supplied->bytes;
        found->permissions = supplied->permissions;
        found->generation = freshGeneration();
      }
    }
    if (page == last) {
      break;
    }
  }
  return ok();
}

Result<void> GuestAddressSpace::writeBytes(uint64_t address,
                                           std::span<const std::byte> bytes) {
  address = canonicalize(address);
  if (bytes.empty()) {
    return ok();
  }
  const std::size_t offset = static_cast<std::size_t>(address & (kPageSize - 1));
  if (offset + bytes.size() <= kPageSize) {
    const uint64_t base = pageBase(address);
    XDEC_TRY(Page* page, residentPage(base, MemoryPermission::Write));
    std::copy_n(bytes.begin(), bytes.size(),
                page->bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    markDirty(*page, base, offset, bytes.size());
    page->generation = freshGeneration();
    return ok();
  }
  // Spanning pages: every page is checked before any is written, so a write
  // that faults on its second page leaves the first untouched.
  XDEC_TRY_VOID(ensure(address, bytes.size(), MemoryPermission::Write));
  std::size_t copied = 0;
  while (copied < bytes.size()) {
    const uint64_t cursor = address + copied;
    const std::size_t pageOffset =
        static_cast<std::size_t>(cursor & (kPageSize - 1));
    const std::size_t chunk =
        std::min(bytes.size() - copied,
                 static_cast<std::size_t>(kPageSize) - pageOffset);
    Page* page = pageAt(cursor);
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(copied), chunk,
                page->bytes.begin() +
                    static_cast<std::ptrdiff_t>(pageOffset));
    markDirty(*page, pageBase(cursor), pageOffset, chunk);
    page->generation = freshGeneration();
    copied += chunk;
  }
  return ok();
}

void GuestAddressSpace::markDirty(Page& page, uint64_t base,
                                  std::size_t offset, std::size_t size) {
  if (!page.dirtyListed) {
    page.dirtyListed = true;
    dirtyPages_.push_back(base);
  }
  while (size != 0) {
    const std::size_t word = offset / kDirtyWordBits;
    const std::size_t bit = offset % kDirtyWordBits;
    const std::size_t take = std::min(kDirtyWordBits - bit, size);
    const uint64_t ones =
        take == kDirtyWordBits ? ~uint64_t{0} : (uint64_t{1} << take) - 1;
    page.dirty[word] |= ones << bit;
    offset += take;
    size -= take;
  }
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

std::vector<MemoryRange> GuestAddressSpace::dirtyRanges() const {
  std::vector<uint64_t> pages = dirtyPages_;
  std::sort(pages.begin(), pages.end());
  pages.erase(std::unique(pages.begin(), pages.end()), pages.end());

  std::vector<MemoryRange> ranges;
  const auto emit = [&ranges](uint64_t address, uint64_t size) {
    // A run that ends on a page boundary continues on the next page when that
    // page's run starts at offset zero; reporting them as one range is what
    // the byte-at-a-time log produced.
    if (!ranges.empty() &&
        ranges.back().address + ranges.back().size == address) {
      ranges.back().size += size;
    } else {
      ranges.push_back(MemoryRange{address, size});
    }
  };

  for (const uint64_t base : pages) {
    const Page* page = lookup(base);
    if (page == nullptr) continue;
    std::size_t index = 0;
    while (index < kPageSize) {
      const std::size_t word = index / kDirtyWordBits;
      const uint64_t pending = page->dirty[word] >> (index % kDirtyWordBits);
      if (pending == 0) {
        index = (word + 1) * kDirtyWordBits;
        continue;
      }
      index += static_cast<std::size_t>(std::countr_zero(pending));
      const std::size_t start = index;
      while (index < kPageSize) {
        const std::size_t bit = index % kDirtyWordBits;
        const auto run = static_cast<std::size_t>(
            std::countr_one(page->dirty[index / kDirtyWordBits] >> bit));
        index += run;
        if (bit + run < kDirtyWordBits) break;
      }
      emit(base + start, index - start);
    }
  }
  return ranges;
}

void GuestAddressSpace::clearDirty() {
  for (const uint64_t base : dirtyPages_) {
    if (Page* page = lookup(base); page != nullptr) {
      page->dirty.fill(0);
      page->dirtyListed = false;
    }
  }
  dirtyPages_.clear();
}

uint64_t GuestAddressSpace::generationAt(uint64_t address) const {
  const Page* page = pageAt(canonicalize(address));
  return page == nullptr ? 0 : page->generation;
}

GuestAddressSpace::Page* GuestAddressSpace::lookup(uint64_t page) const {
  if (cache_.base == page) return cache_.page;
  const auto found = pages_.find(page);
  if (found == pages_.end()) return nullptr;
  cache_.base = page;
  cache_.page = &found->second;
  return cache_.page;
}

GuestAddressSpace::Page* GuestAddressSpace::pageAt(uint64_t address) {
  return lookup(pageBase(address));
}

const GuestAddressSpace::Page* GuestAddressSpace::pageAt(uint64_t address) const {
  return lookup(pageBase(address));
}

}  // namespace xdec::exec
