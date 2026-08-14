#include "xdec/binary/dyld_cache_metadata.h"

#include <algorithm>

#include "xdec/binary/image.h"

namespace xdec::binary {

std::string_view toString(DyldCacheType type) noexcept {
  switch (type) {
    case DyldCacheType::Development:
      return "development";
    case DyldCacheType::Production:
      return "production";
    case DyldCacheType::Universal:
      return "universal";
    case DyldCacheType::Unknown:
      return "unknown";
  }
  return "unknown";
}

const DyldCacheImageRecord* DyldCacheMetadata::imageContaining(uint64_t va) const noexcept {
  // `images` is sorted by loadAddress; the record covering `va` is the last
  // one starting at or before it, bounded by that image's __TEXT size when
  // known. Falls back to "starts before va" when the size is unknown (image
  // list without matching imagesText entries), which still identifies the
  // owning dylib for any address after its load address and before the next
  // image's.
  const auto it = std::upper_bound(
      images.begin(), images.end(), va,
      [](uint64_t address, const DyldCacheImageRecord& image) { return address < image.loadAddress; });
  if (it == images.begin()) {
    return nullptr;
  }
  const DyldCacheImageRecord& candidate = *std::prev(it);
  if (candidate.textSegmentSize != 0 && va >= candidate.loadAddress + candidate.textSegmentSize) {
    return nullptr;
  }
  return &candidate;
}

const DyldCacheImageRecord* DyldCacheMetadata::imageNamed(std::string_view path) const noexcept {
  for (const DyldCacheImageRecord& image : images) {
    if (image.path == path) {
      return &image;
    }
  }
  return nullptr;
}

const DyldCacheMetadata* asDyldCacheMetadata(const BinaryImage& image) noexcept {
  if (image.format() != BinaryFormat::DyldCache) {
    return nullptr;
  }
  return dynamic_cast<const DyldCacheMetadata*>(image.formatMetadata());
}

}  // namespace xdec::binary
