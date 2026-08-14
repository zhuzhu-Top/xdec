// Extension point for data that belongs to one binary format only.
//
// BinaryImage's public API is deliberately format-independent: every reader
// of a Section, Symbol or MemoryRegion works the same way regardless of
// whether the loader was ELF, Mach-O, or a dyld shared cache. Some formats
// still carry information nothing else needs -- a cache's UUID, platform, and
// per-subcache slide info being the motivating example -- and bolting that
// onto ImageContents would make every consumer of the format-independent
// fields pay for a field only one loader fills in. A loader that has such
// data attaches one FormatMetadata subclass instead; everyone else ignores
// it.
#pragma once

namespace xdec::binary {

class FormatMetadata {
 public:
  virtual ~FormatMetadata() = default;
};

}  // namespace xdec::binary
