#include "xdec/support/arena.h"

#include <algorithm>

#include "xdec/support/bits.h"

namespace xdec {

Arena::Arena(std::size_t chunkSize)
    : chunkSize_(std::max<std::size_t>(chunkSize, 1024)) {}

Arena::~Arena() { releaseChunks(); }

Arena::Arena(Arena&& other) noexcept
    : chunks_(std::move(other.chunks_)),
      current_(other.current_),
      limit_(other.limit_),
      chunkSize_(other.chunkSize_),
      bytesUsed_(other.bytesUsed_),
      bytesReserved_(other.bytesReserved_),
      interned_(std::move(other.interned_)) {
  other.chunks_.clear();
  other.current_ = nullptr;
  other.limit_ = nullptr;
  other.bytesUsed_ = 0;
  other.bytesReserved_ = 0;
  other.interned_.clear();
}

Arena& Arena::operator=(Arena&& other) noexcept {
  if (this != &other) {
    releaseChunks();
    chunks_ = std::move(other.chunks_);
    current_ = other.current_;
    limit_ = other.limit_;
    chunkSize_ = other.chunkSize_;
    bytesUsed_ = other.bytesUsed_;
    bytesReserved_ = other.bytesReserved_;
    interned_ = std::move(other.interned_);
    other.chunks_.clear();
    other.current_ = nullptr;
    other.limit_ = nullptr;
    other.bytesUsed_ = 0;
    other.bytesReserved_ = 0;
    other.interned_.clear();
  }
  return *this;
}

void Arena::releaseChunks() noexcept {
  for (const Chunk& chunk : chunks_) {
    ::operator delete(chunk.data, std::align_val_t{alignof(std::max_align_t)});
  }
  chunks_.clear();
  current_ = nullptr;
  limit_ = nullptr;
}

void Arena::addChunk(std::size_t minimumBytes) {
  const std::size_t size = std::max(chunkSize_, minimumBytes);
  auto* data = static_cast<std::byte*>(
      ::operator new(size, std::align_val_t{alignof(std::max_align_t)}));
  chunks_.push_back(Chunk{data, size});
  current_ = data;
  limit_ = data + size;
  bytesReserved_ += size;

  // Grow geometrically so that a long-lived arena does not degenerate into one
  // system allocation per object, but cap it to keep peak waste bounded.
  chunkSize_ = std::min<std::size_t>(chunkSize_ * 2, 8u * 1024u * 1024u);
}

void* Arena::allocate(std::size_t bytes, std::size_t alignment) {
  XDEC_ASSERT(isPowerOfTwo(alignment), "arena alignment must be a power of two");
  if (bytes == 0) {
    bytes = 1;
  }

  auto aligned = [alignment](std::byte* pointer) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    const auto adjusted = static_cast<std::uintptr_t>(
        alignUp(static_cast<uint64_t>(address), static_cast<uint64_t>(alignment)));
    return pointer + (adjusted - address);
  };

  std::byte* start = current_ != nullptr ? aligned(current_) : nullptr;
  if (start == nullptr || start + bytes > limit_) {
    // The requested block plus worst-case alignment padding must fit.
    addChunk(bytes + alignment);
    start = aligned(current_);
    XDEC_ASSERT(start + bytes <= limit_, "fresh arena chunk too small");
  }

  current_ = start + bytes;
  bytesUsed_ += bytes;
  return start;
}

std::string_view Arena::saveString(std::string_view text) {
  auto* storage = static_cast<char*>(allocate(text.size() + 1, alignof(char)));
  if (!text.empty()) {
    std::memcpy(storage, text.data(), text.size());
  }
  storage[text.size()] = '\0';
  return std::string_view{storage, text.size()};
}

std::string_view Arena::internString(std::string_view text) {
  if (auto it = interned_.find(text); it != interned_.end()) {
    return *it;
  }
  const std::string_view saved = saveString(text);
  interned_.insert(saved);
  return saved;
}

void Arena::reset() {
  interned_.clear();
  if (chunks_.empty()) {
    return;
  }
  // Keep the first chunk so that a reset-and-reuse cycle does not thrash the
  // allocator; release the rest.
  for (std::size_t index = 1; index < chunks_.size(); ++index) {
    ::operator delete(chunks_[index].data, std::align_val_t{alignof(std::max_align_t)});
  }
  const Chunk first = chunks_.front();
  chunks_.clear();
  chunks_.push_back(first);
  current_ = first.data;
  limit_ = first.data + first.size;
  bytesUsed_ = 0;
  bytesReserved_ = first.size;
}

}  // namespace xdec
