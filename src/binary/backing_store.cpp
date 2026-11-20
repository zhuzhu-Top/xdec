#include "xdec/binary/backing_store.h"

#include <cstring>
#include <format>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace xdec::binary {

struct FileBuffer::Impl {
#ifdef _WIN32
  HANDLE file = INVALID_HANDLE_VALUE;
  HANDLE mapping = nullptr;
  const void* mappedView = nullptr;
#else
  void* mappedView = nullptr;
  std::size_t mappedSize = 0;
#endif
  std::unique_ptr<std::byte[]> heap;

  ~Impl() {
#ifdef _WIN32
    if (mappedView != nullptr) {
      UnmapViewOfFile(mappedView);
    }
    if (mapping != nullptr) {
      CloseHandle(mapping);
    }
    if (file != INVALID_HANDLE_VALUE) {
      CloseHandle(file);
    }
#else
    if (mappedView != nullptr) {
      munmap(mappedView, mappedSize);
    }
#endif
  }
};

FileBuffer::~FileBuffer() = default;
FileBuffer::FileBuffer(FileBuffer&&) noexcept = default;
FileBuffer& FileBuffer::operator=(FileBuffer&&) noexcept = default;

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

  FileBuffer buffer;
  buffer.impl_ = std::make_unique<Impl>();
  buffer.size_ = static_cast<std::size_t>(fileSize);

#ifdef _WIN32
  buffer.impl_->file =
      CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (buffer.impl_->file == INVALID_HANDLE_VALUE) {
    return err(DiagCode::IoError,
               std::format("cannot open '{}' (win32 error {})", path.string(), GetLastError()));
  }
  buffer.impl_->mapping =
      CreateFileMappingW(buffer.impl_->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (buffer.impl_->mapping == nullptr) {
    return err(DiagCode::IoError,
               std::format("cannot create file mapping for '{}' (win32 error {})",
                           path.string(), GetLastError()));
  }
  buffer.impl_->mappedView = MapViewOfFile(buffer.impl_->mapping, FILE_MAP_READ, 0, 0, 0);
  if (buffer.impl_->mappedView == nullptr) {
    return err(DiagCode::IoError,
               std::format("cannot map view of '{}' (win32 error {})", path.string(),
                           GetLastError()));
  }
  buffer.view_ = static_cast<const std::byte*>(buffer.impl_->mappedView);
#else
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return err(DiagCode::IoError, std::format("cannot open '{}'", path.string()));
  }
  void* ptr = ::mmap(nullptr, buffer.size_, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (ptr == MAP_FAILED) {
    return err(DiagCode::IoError, std::format("cannot mmap '{}'", path.string()));
  }
  buffer.impl_->mappedView = ptr;
  buffer.impl_->mappedSize = buffer.size_;
  buffer.view_ = static_cast<const std::byte*>(ptr);
#endif
  return buffer;
}

FileBuffer FileBuffer::fromBytes(std::span<const std::byte> bytes) {
  FileBuffer buffer;
  buffer.impl_ = std::make_unique<Impl>();
  buffer.size_ = bytes.size();
  buffer.impl_->heap = std::make_unique<std::byte[]>(buffer.size_ == 0 ? 1 : buffer.size_);
  if (!bytes.empty()) {
    std::memcpy(buffer.impl_->heap.get(), bytes.data(), bytes.size());
  }
  buffer.view_ = buffer.impl_->heap.get();
  return buffer;
}

}  // namespace xdec::binary
