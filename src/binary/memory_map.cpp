#include "xdec/binary/memory_map.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <limits>

#include "xdec/support/log.h"

namespace xdec::binary {

XDEC_DEFINE_LOG_CATEGORY(logMemory, "memory")

std::string toString(MemoryPermissions permissions) {
  std::string text;
  text += hasPermission(permissions, MemoryPermissions::Read) ? 'r' : '-';
  text += hasPermission(permissions, MemoryPermissions::Write) ? 'w' : '-';
  text += hasPermission(permissions, MemoryPermissions::Execute) ? 'x' : '-';
  return text;
}

void MemoryMap::addRegion(MemoryRegion region) {
  XDEC_ASSERT(!finalized_, "MemoryMap::addRegion after finalize");
  if (region.size == 0) {
    return;
  }
  regions_.push_back(std::move(region));
}

Result<void> MemoryMap::finalize() {
  XDEC_ASSERT(!finalized_, "MemoryMap::finalize called twice");

  std::sort(regions_.begin(), regions_.end(),
            [](const MemoryRegion& a, const MemoryRegion& b) { return a.va < b.va; });

  const uint64_t backingSize = backing_.size();
  for (const MemoryRegion& region : regions_) {
    if (region.va > std::numeric_limits<uint64_t>::max() - region.size) {
      return err(DiagCode::BadFormat,
                 std::format("region '{}' at 0x{:x} size 0x{:x} wraps the address space",
                             region.name, region.va, region.size));
    }
    if (region.fileSize > region.size) {
      return err(DiagCode::BadFormat,
                 std::format("region '{}' at 0x{:x} declares file size 0x{:x} > memory size 0x{:x}",
                             region.name, region.va, region.fileSize, region.size));
    }
    if (region.fileSize != 0 &&
        (region.fileOffset > backingSize || backingSize - region.fileOffset < region.fileSize)) {
      return err(DiagCode::BadFormat,
                 std::format("region '{}' at 0x{:x} reads file range [0x{:x}, 0x{:x}) "
                             "beyond the {} byte file",
                             region.name, region.va, region.fileOffset,
                             region.fileOffset + region.fileSize, backingSize));
    }
  }

  for (std::size_t index = 1; index < regions_.size(); ++index) {
    const MemoryRegion& previous = regions_[index - 1];
    const MemoryRegion& current = regions_[index];
    if (current.va < previous.endVa()) {
      return err(DiagCode::BadFormat,
                 std::format("regions '{}' [0x{:x}, 0x{:x}) and '{}' [0x{:x}, 0x{:x}) overlap",
                             previous.name, previous.va, previous.endVa(), current.name,
                             current.va, current.endVa()));
    }
  }

  finalized_ = true;
  XDEC_LOG_DEBUG(logMemory(), "finalized {} regions covering [0x{:x}, 0x{:x})", regions_.size(),
                 lowestAddress(), highestAddress());
  return ok();
}

const MemoryRegion* MemoryMap::regionAt(uint64_t va) const noexcept {
  // Regions are sorted and non-overlapping, so the candidate is the last region
  // whose start is at or below `va`.
  const auto it = std::upper_bound(
      regions_.begin(), regions_.end(), va,
      [](uint64_t address, const MemoryRegion& region) { return address < region.va; });
  if (it == regions_.begin()) {
    return nullptr;
  }
  const MemoryRegion& candidate = *std::prev(it);
  return candidate.contains(va) ? &candidate : nullptr;
}

uint64_t MemoryMap::lowestAddress() const noexcept {
  return regions_.empty() ? 0 : regions_.front().va;
}

uint64_t MemoryMap::highestAddress() const noexcept {
  return regions_.empty() ? 0 : regions_.back().endVa();
}

Result<void> MemoryMap::read(uint64_t va, std::span<std::byte> out) const {
  std::size_t written = 0;
  uint64_t cursor = va;

  while (written < out.size()) {
    const MemoryRegion* region = regionAt(cursor);
    if (region == nullptr) {
      return err(Diag{DiagCode::UnmappedAddress,
                      std::format("no mapped region covers 0x{:x} (while reading {} bytes "
                                  "from 0x{:x})",
                                  cursor, out.size(), va)}
                     .at(cursor));
    }

    const uint64_t offsetInRegion = cursor - region->va;
    const uint64_t remainingInRegion = region->size - offsetInRegion;
    const uint64_t wanted =
        std::min<uint64_t>(remainingInRegion, static_cast<uint64_t>(out.size() - written));

    // Bytes below fileSize come from the file; the tail is the zero-initialised
    // part of the segment, which is where .bss lives.
    const uint64_t fileAvailable =
        offsetInRegion < region->fileSize ? region->fileSize - offsetInRegion : 0;
    const uint64_t fromFile = std::min(fileAvailable, wanted);

    if (fromFile != 0) {
      std::memcpy(out.data() + written,
                  backing_.data() + region->fileOffset + offsetInRegion,
                  static_cast<std::size_t>(fromFile));
    }
    if (wanted > fromFile) {
      std::memset(out.data() + written + fromFile, 0,
                  static_cast<std::size_t>(wanted - fromFile));
    }

    written += static_cast<std::size_t>(wanted);
    cursor += wanted;
  }

  return ok();
}

std::span<const std::byte> MemoryMap::directView(uint64_t va, uint64_t size) const noexcept {
  if (size == 0) {
    return {};
  }
  const MemoryRegion* region = regionAt(va);
  if (region == nullptr) {
    return {};
  }
  const uint64_t offsetInRegion = va - region->va;
  if (offsetInRegion >= region->fileSize || region->fileSize - offsetInRegion < size) {
    // Either past the file-backed part or spilling into the zero tail or the
    // next region; the caller must go through read().
    return {};
  }
  return backing_.subspan(static_cast<std::size_t>(region->fileOffset + offsetInRegion),
                          static_cast<std::size_t>(size));
}

}  // namespace xdec::binary
