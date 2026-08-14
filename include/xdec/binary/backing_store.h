// The physical files behind a MemoryMap.
//
// A single ELF or Mach-O ever needs exactly one file: FileBuffer plus
// MemoryMap::setBackingBytes is enough. The dyld shared cache is why this
// header exists -- a "shared cache" ships as a handful of sibling files (a
// main file plus numbered or suffixed subcache parts, sometimes a separate
// `.symbols` file), and one mapping's bytes can live in a different physical
// file than the mapping next to it in address space. BackingStore keeps each
// part's owning buffer together with a diagnostic name, in the order
// MemoryRegion::backingIndex refers to.
#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "xdec/support/result.h"

namespace xdec::binary {

/// Owns raw file bytes. The data address is stable across moves, so spans
/// handed to a MemoryMap stay valid when the buffer is later moved into an
/// image or a BackingStore.
class FileBuffer {
 public:
  FileBuffer() = default;

  static Result<FileBuffer> fromFile(const std::filesystem::path& path);

  /// Copies an in-memory image. Used by tests that synthesise binaries and by
  /// callers that already hold the bytes.
  static FileBuffer fromBytes(std::span<const std::byte> bytes);

  [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
    return std::span<const std::byte>{data_.get(), size_};
  }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

 private:
  std::unique_ptr<std::byte[]> data_;
  std::size_t size_ = 0;
};

/// One or more FileBuffers, addressable by index. A loader builds this once
/// while it still holds every part's bytes, points MemoryMap at its spans via
/// `MemoryMap::setBackingParts(store.spans())`, and then moves the whole
/// store into `ImageContents::store` -- the same "compute spans, then move
/// the owner" pattern a single-file loader already uses for its one
/// FileBuffer.
class BackingStore {
 public:
  struct Part {
    /// Diagnostic only -- typically the part's file name, e.g.
    /// "dyld_shared_cache_arm64.3".
    std::string name;
    FileBuffer buffer;
  };

  BackingStore() = default;

  /// Appends a part and returns its index, i.e. the value a MemoryRegion
  /// sourced from it should set as `backingIndex`.
  std::size_t addPart(std::string name, FileBuffer buffer) {
    const std::size_t index = parts_.size();
    parts_.push_back(Part{std::move(name), std::move(buffer)});
    return index;
  }

  [[nodiscard]] std::size_t partCount() const noexcept { return parts_.size(); }
  [[nodiscard]] const Part& part(std::size_t index) const noexcept { return parts_[index]; }
  [[nodiscard]] std::span<const std::byte> bytes(std::size_t index) const noexcept {
    return index < parts_.size() ? parts_[index].buffer.bytes() : std::span<const std::byte>{};
  }

  /// Spans in part order, ready for `MemoryMap::setBackingParts`.
  [[nodiscard]] std::vector<std::span<const std::byte>> spans() const {
    std::vector<std::span<const std::byte>> result;
    result.reserve(parts_.size());
    for (const Part& part : parts_) {
      result.push_back(part.buffer.bytes());
    }
    return result;
  }

  [[nodiscard]] std::size_t totalSize() const noexcept {
    std::size_t total = 0;
    for (const Part& part : parts_) {
      total += part.buffer.size();
    }
    return total;
  }

 private:
  std::vector<Part> parts_;
};

}  // namespace xdec::binary
