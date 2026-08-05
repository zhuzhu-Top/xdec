// Bump-pointer arena with an attached string interner.
//
// Per-function IR lives in a per-function arena, which is what makes
// function-level analysis trivially parallel and lets the whole IR for a
// function be discarded in one step. Only trivially destructible types are
// allowed, so resetting is a pointer reset rather than a destructor walk.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "xdec/support/compiler.h"

namespace xdec {

class Arena {
 public:
  static constexpr std::size_t kDefaultChunkSize = 64 * 1024;

  explicit Arena(std::size_t chunkSize = kDefaultChunkSize);
  ~Arena();

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  Arena(Arena&& other) noexcept;
  Arena& operator=(Arena&& other) noexcept;

  [[nodiscard]] void* allocate(std::size_t bytes, std::size_t alignment);

  template <class T, class... Args>
  [[nodiscard]] T* create(Args&&... args) {
    static_assert(std::is_trivially_destructible_v<T>,
                  "Arena only stores trivially destructible types");
    void* storage = allocate(sizeof(T), alignof(T));
    return new (storage) T(std::forward<Args>(args)...);
  }

  /// Uninitialised array; the caller is responsible for filling it.
  template <class T>
  [[nodiscard]] std::span<T> allocateArray(std::size_t count) {
    static_assert(std::is_trivially_destructible_v<T>,
                  "Arena only stores trivially destructible types");
    if (count == 0) {
      return {};
    }
    void* storage = allocate(sizeof(T) * count, alignof(T));
    return std::span<T>{static_cast<T*>(storage), count};
  }

  /// Copies `values` into the arena.
  template <class T>
  [[nodiscard]] std::span<T> copyArray(std::span<const T> values) {
    static_assert(std::is_trivially_copyable_v<T>, "copyArray requires a trivial type");
    std::span<T> result = allocateArray<T>(values.size());
    if (!values.empty()) {
      std::memcpy(result.data(), values.data(), values.size() * sizeof(T));
    }
    return result;
  }

  /// Copies `text` into the arena and NUL-terminates it, so the result is also
  /// usable as a C string.
  [[nodiscard]] std::string_view saveString(std::string_view text);

  /// Like saveString but returns an identical view for equal inputs. Used for
  /// names that repeat heavily (register names, mnemonics, symbol names).
  [[nodiscard]] std::string_view internString(std::string_view text);

  /// Bytes handed out to callers.
  [[nodiscard]] std::size_t bytesUsed() const noexcept { return bytesUsed_; }
  /// Bytes obtained from the system, including padding and unused tail space.
  [[nodiscard]] std::size_t bytesReserved() const noexcept { return bytesReserved_; }
  [[nodiscard]] std::size_t chunkCount() const noexcept { return chunks_.size(); }

  /// Releases every chunk but the first and rewinds. Invalidates all pointers
  /// and interned strings previously handed out.
  void reset();

 private:
  struct Chunk {
    std::byte* data = nullptr;
    std::size_t size = 0;
  };

  void addChunk(std::size_t minimumBytes);
  void releaseChunks() noexcept;

  std::vector<Chunk> chunks_;
  std::byte* current_ = nullptr;
  std::byte* limit_ = nullptr;
  std::size_t chunkSize_ = kDefaultChunkSize;
  std::size_t bytesUsed_ = 0;
  std::size_t bytesReserved_ = 0;
  std::unordered_set<std::string_view> interned_;
};

}  // namespace xdec
