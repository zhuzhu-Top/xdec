// CompositeByteReader: one ByteReader answering for several images.
//
// Motivated by exactly one shape, described in
// docs/20-absd-entry-registers.md: an obfuscated entry reads bytes through a
// register the loader leaked (dyld's own `x22`), and those bytes live in
// dyld's own image, not the binary being decompiled. resolve_indirect (via
// analysis::ImageEval) only ever gets one ByteReader (see
// pass::Context::setImage); this is what lets a caller hand it a single
// reader that still answers once the address it is asked about is not in the
// primary image at all.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xdec/support/reader.h"

namespace xdec {

/// One image's bytes, reachable at a fixed translation from the composite's
/// own address space.
struct ImageRegion {
  std::string name;
  /// Reads this image's own bytes, in whatever coordinate space `reader`
  /// itself already uses (typically that file's own declared addresses).
  ByteReader reader;
  /// An address `va` in the composite space reads this region at
  /// `va - runtimeBase + fileBase`. Equal for a region with no translation
  /// at all -- the primary image, or a companion whose own file already
  /// reflects the addresses a binding's offset is meant to land on.
  uint64_t runtimeBase = 0;
  uint64_t fileBase = 0;
};

/// Routes a read across the regions added, trying each region's translated
/// address in the order they were added (primary first) and returning the
/// first that can answer. A region that cannot cover an address simply fails
/// to read it -- the same "unmapped" answer a single-image ByteReader gives
/// today -- so composing regions never turns a real gap into a wrong value.
class CompositeByteReader {
 public:
  void addRegion(ImageRegion region) { regions_.push_back(std::move(region)); }
  [[nodiscard]] bool empty() const noexcept { return regions_.empty(); }
  [[nodiscard]] std::size_t regionCount() const noexcept { return regions_.size(); }

  /// A ByteReader closing over a copy of the regions added so far, so it
  /// stays valid independent of this object's own lifetime.
  [[nodiscard]] ByteReader reader() const;

 private:
  std::vector<ImageRegion> regions_;
};

}  // namespace xdec
