// CompositeByteReader (see the header for what this composes and why).
#include "xdec/support/composite_reader.h"

namespace xdec {

ByteReader CompositeByteReader::reader() const {
  std::vector<ImageRegion> regions = regions_;
  return [regions](uint64_t va, std::span<std::byte> out) -> Result<void> {
    for (const ImageRegion& region : regions) {
      if (!region.reader) {
        continue;
      }
      const uint64_t translated = va - region.runtimeBase + region.fileBase;
      if (Result<void> result = region.reader(translated, out)) {
        return result;
      }
    }
    return err(DiagCode::UnmappedAddress,
               "address {:#x} is outside every region this reader knows about", va);
  };
}

}  // namespace xdec
