#include "xdec/binary/backing_store.h"

#include <cstdio>
#include <cstring>
#include <format>

namespace xdec::binary {

Result<FileBuffer> FileBuffer::fromFile(const std::filesystem::path& path) {
  std::error_code error;
  const auto fileSize = std::filesystem::file_size(path, error);
  if (error) {
    return err(DiagCode::IoError,
               std::format("cannot stat '{}': {}", path.string(), error.message()));
  }
  if (fileSize == 0) {
    return err(DiagCode::BadFormat, std::format("'{}' is empty", path.string()));
  }

  std::FILE* handle = std::fopen(path.string().c_str(), "rb");
  if (handle == nullptr) {
    return err(DiagCode::IoError, std::format("cannot open '{}'", path.string()));
  }

  FileBuffer buffer;
  buffer.size_ = static_cast<std::size_t>(fileSize);
  buffer.data_ = std::make_unique<std::byte[]>(buffer.size_);
  const std::size_t read = std::fread(buffer.data_.get(), 1, buffer.size_, handle);
  std::fclose(handle);

  if (read != buffer.size_) {
    return err(DiagCode::IoError,
               std::format("short read on '{}': got {} of {} bytes", path.string(), read,
                           buffer.size_));
  }
  return buffer;
}

FileBuffer FileBuffer::fromBytes(std::span<const std::byte> bytes) {
  FileBuffer buffer;
  buffer.size_ = bytes.size();
  buffer.data_ = std::make_unique<std::byte[]>(buffer.size_ == 0 ? 1 : buffer.size_);
  if (!bytes.empty()) {
    std::memcpy(buffer.data_.get(), bytes.data(), bytes.size());
  }
  return buffer;
}

}  // namespace xdec::binary
