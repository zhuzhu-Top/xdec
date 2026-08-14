// Format-specific data for a loaded dyld shared cache.
//
// Attached to ImageContents::formatMetadata (see format_metadata.h) so that
// BinaryImage's core API stays format-independent while cache-specific
// consumers -- `xdec info`, `xdec images`, `xdec cache-locate`, and the
// cache pointer decoder -- can still get at it via
// `BinaryImage::formatMetadata()`.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xdec/binary/format_metadata.h"

namespace xdec::binary {

using CacheUuid = std::array<uint8_t, 16>;

/// One dylib/bundle known to the cache, from `dyld_cache_image_info` (path,
/// load address) merged with `dyld_cache_image_text_info` (UUID, __TEXT
/// size) when both are present.
struct DyldCacheImageRecord {
  std::string path;
  uint64_t loadAddress = 0;
  uint32_t textSegmentSize = 0;
  CacheUuid uuid{};
  uint32_t index = 0;
};

/// One physical file backing the cache: the main file plus its subcache and
/// symbols siblings. Parallels `BackingStore`'s parts by index -- part `i`
/// here describes `ImageContents::store.part(i)`.
struct DyldCachePartInfo {
  std::string fileName;
  CacheUuid uuid{};
  /// This part's VM offset from `sharedRegionStart`, i.e. its lowest mapped
  /// address is `sharedRegionStart + vmOffset`. Zero for the main file.
  uint64_t vmOffset = 0;
  /// True for the unmapped `.symbols` file, which contributes local symbols
  /// but no MemoryRegion.
  bool isSymbolsFile = false;
};

enum class DyldCacheType : uint8_t { Development, Production, Universal, Unknown };
[[nodiscard]] std::string_view toString(DyldCacheType type) noexcept;

/// Everything the dyld cache loader knows that a generic BinaryImage
/// consumer wouldn't: cache identity, the shared VM region every part maps
/// into, the physical parts, and the image (dylib) index.
class DyldCacheMetadata : public FormatMetadata {
 public:
  CacheUuid uuid{};
  DyldCacheType cacheType = DyldCacheType::Unknown;
  uint32_t platform = 0;
  uint32_t formatVersion = 0;
  bool builtFromChainedFixups = false;
  uint64_t sharedRegionStart = 0;
  uint64_t sharedRegionSize = 0;
  uint64_t maxSlide = 0;

  std::vector<DyldCachePartInfo> parts;
  /// Sorted by `loadAddress` so `imageContaining` can binary search.
  std::vector<DyldCacheImageRecord> images;

  [[nodiscard]] const DyldCacheImageRecord* imageContaining(uint64_t va) const noexcept;
  [[nodiscard]] const DyldCacheImageRecord* imageNamed(std::string_view path) const noexcept;
};

/// Convenience accessor: null when `image.format() != BinaryFormat::DyldCache`
/// or the loader attached no metadata.
[[nodiscard]] const DyldCacheMetadata* asDyldCacheMetadata(const class BinaryImage& image) noexcept;

}  // namespace xdec::binary
