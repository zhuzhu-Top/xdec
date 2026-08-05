// Bounds-checked field reads over a byte buffer.
//
// Binary headers are parsed as a group of field reads followed by one failure
// check, rather than a Result per field. A malformed file therefore cannot read
// out of bounds, but the parser still reads like a struct layout description.
// Failed reads yield zero, so a caller that forgets the check gets defined
// (wrong) values rather than undefined behaviour.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "xdec/support/bits.h"
#include "xdec/support/target.h"

namespace xdec::binary {

class FieldReader {
 public:
  FieldReader(std::span<const std::byte> data, Endian endian) noexcept
      : data_(data), endian_(endian) {}

  /// Reads an unsigned integer of `bytes` width (1, 2, 4 or 8).
  uint64_t read(uint64_t offset, unsigned bytes) noexcept {
    if (bytes == 0 || bytes > 8 || offset > data_.size() || data_.size() - offset < bytes) {
      failed_ = true;
      return 0;
    }
    const std::byte* source = data_.data() + offset;
    uint64_t value = 0;
    if (endian_ == Endian::Little) {
      for (unsigned index = bytes; index-- > 0;) {
        value = (value << 8) | static_cast<uint64_t>(std::to_integer<uint8_t>(source[index]));
      }
    } else {
      for (unsigned index = 0; index < bytes; ++index) {
        value = (value << 8) | static_cast<uint64_t>(std::to_integer<uint8_t>(source[index]));
      }
    }
    return value;
  }

  uint64_t u8(uint64_t offset) noexcept { return read(offset, 1); }
  uint64_t u16(uint64_t offset) noexcept { return read(offset, 2); }
  uint64_t u32(uint64_t offset) noexcept { return read(offset, 4); }
  uint64_t u64(uint64_t offset) noexcept { return read(offset, 8); }

  /// Signed read of `bytes` width.
  int64_t signedRead(uint64_t offset, unsigned bytes) noexcept {
    return signExtend(read(offset, bytes), bytes * 8);
  }

  std::span<const std::byte> slice(uint64_t offset, uint64_t size) noexcept {
    if (offset > data_.size() || data_.size() - offset < size) {
      failed_ = true;
      return {};
    }
    return data_.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
  }

  /// NUL-terminated string starting at `offset`. Returns empty and marks
  /// failure if there is no terminator before the end of the buffer.
  std::string_view cstring(uint64_t offset) noexcept {
    if (offset >= data_.size()) {
      failed_ = true;
      return {};
    }
    const auto* begin = reinterpret_cast<const char*>(data_.data() + offset);
    const std::size_t available = data_.size() - static_cast<std::size_t>(offset);
    const std::size_t length = std::string_view{begin, available}.find('\0');
    if (length == std::string_view::npos) {
      failed_ = true;
      return {};
    }
    return std::string_view{begin, length};
  }

  [[nodiscard]] bool failed() const noexcept { return failed_; }
  void clearFailure() noexcept { failed_ = false; }
  [[nodiscard]] uint64_t size() const noexcept { return data_.size(); }
  [[nodiscard]] std::span<const std::byte> data() const noexcept { return data_; }
  [[nodiscard]] Endian endian() const noexcept { return endian_; }

 private:
  std::span<const std::byte> data_;
  Endian endian_;
  bool failed_ = false;
};

}  // namespace xdec::binary
