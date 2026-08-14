// dyld shared cache loading: a synthetic single-file cache (the historical
// shape) and a synthetic two-file split cache (main + one legacy-numbered
// sibling, `dyld_subcache_entry_v1`), since that is the shape the loader
// spends most of its logic on. Field offsets mirror
// src/binary/dyld_cache/dyld_cache_format.h exactly.
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "xdec/binary/dyld_cache_metadata.h"
#include "xdec/binary/image.h"

using namespace xdec;
using namespace xdec::binary;

namespace {

/// Writes dyld_cache_header fields (up through imagesCount, offset 456 --
/// see kHdrMinimumSize) plus a mapping table, an optional subCacheArray, and
/// an optional images table, all at caller-chosen offsets. Mirrors
/// MachOBuilder in test_macho.cpp: a fixed byte buffer with named `put*`
/// helpers, laid out explicitly rather than growing structurally.
class DyldCacheBuilder {
 public:
  explicit DyldCacheBuilder(std::size_t size) : bytes_(size, std::byte{0}) {
    putString(0, "dyld_v1   arm64");
  }

  void setMapping(uint32_t offset, uint32_t count) {
    put32(16, offset);
    put32(20, count);
  }
  void mapping(uint32_t tableOffset, uint32_t index, uint64_t va, uint64_t size, uint64_t fileOffset,
              uint32_t initProt) {
    const uint64_t base = tableOffset + static_cast<uint64_t>(index) * 32;
    put64(base + 0, va);
    put64(base + 8, size);
    put64(base + 16, fileOffset);
    put32(base + 24, initProt);  // maxProt
    put32(base + 28, initProt);  // initProt
  }

  void setUuid(uint8_t fill) { std::memset(bytes_.data() + 88, fill, 16); }
  void setCacheType(uint64_t type) { put64(104, type); }
  void setSharedRegion(uint64_t start, uint64_t size) {
    put64(224, start);
    put64(232, size);
  }
  void setSubCacheArray(uint32_t offset, uint32_t count) {
    put32(392, offset);
    put32(396, count);
  }
  void subCacheEntryV1(uint32_t tableOffset, uint32_t index, uint8_t uuidFill, uint64_t vmOffset) {
    const uint64_t base = tableOffset + static_cast<uint64_t>(index) * 24;
    for (uint64_t i = 0; i < 16; ++i) {
      bytes_[base + i] = static_cast<std::byte>(uuidFill);
    }
    put64(base + 16, vmOffset);
  }
  void setImages(uint32_t offset, uint32_t count) {
    put32(448, offset);
    put32(452, count);
  }
  void image(uint32_t tableOffset, uint32_t index, uint64_t address, uint32_t pathFileOffset) {
    const uint64_t base = tableOffset + static_cast<uint64_t>(index) * 32;
    put64(base + 0, address);
    put64(base + 8, 0);  // modTime
    put64(base + 16, 0);  // inode
    put32(base + 24, pathFileOffset);
  }
  void putString(uint64_t offset, std::string_view text) {
    std::memcpy(bytes_.data() + offset, text.data(), text.size());
  }
  void putByte(uint64_t offset, uint8_t value) { bytes_[offset] = static_cast<std::byte>(value); }
  void putU32At(uint64_t offset, uint32_t value) { put32(offset, value); }

  void writeTo(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes_.data()), static_cast<std::streamsize>(bytes_.size()));
  }

 private:
  void put32(uint64_t offset, uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
      bytes_[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xff);
    }
  }
  void put64(uint64_t offset, uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) {
      bytes_[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xff);
    }
  }

  std::vector<std::byte> bytes_;
};

std::filesystem::path testDir() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "xdec_test_dyld_cache";
  std::filesystem::create_directories(dir);
  return dir;
}

}  // namespace

TEST_CASE("dyld cache loader builds a memory map and image index from a single file", "[dyld_cache]") {
  // Header (456) + mapping table (2 * 32) + image table (1 * 32) + one path
  // string, then the two mapped regions themselves further out in the file.
  DyldCacheBuilder builder(0x4000);
  builder.setMapping(456, 2);
  builder.mapping(456, 0, 0x100000000, 0x1000, 0x1000, /*initProt=*/5);   // __TEXT r-x
  builder.mapping(456, 1, 0x100001000, 0x1000, 0x2000, /*initProt=*/3);  // __DATA rw-
  builder.setUuid(0xAB);
  builder.setCacheType(1);
  builder.setSharedRegion(0x100000000, 0x2000);
  builder.setImages(520, 1);
  builder.image(520, 0, 0x100000000, 552);
  builder.putString(552, "/usr/lib/libFoo.dylib");
  builder.putByte(0x1000, 0xCC);   // one recognisable byte inside __TEXT
  builder.putByte(0x2000, 0xDD);   // one recognisable byte inside __DATA

  const std::filesystem::path path = testDir() / "dyld_single_arm64";
  builder.writeTo(path);

  auto opened = openBinary(path);
  REQUIRE(opened);
  const BinaryImage& image = **opened;

  CHECK(image.format() == BinaryFormat::DyldCache);
  CHECK(image.arch() == Arch::AArch64);
  CHECK(image.pointerBits() == 64);
  REQUIRE(image.memory().regions().size() == 2);
  CHECK(image.isExecutable(0x100000000));
  CHECK_FALSE(image.isExecutable(0x100001000));
  CHECK(image.isWritable(0x100001000));

  std::byte value{};
  REQUIRE(image.read(0x100000000, std::span<std::byte>{&value, 1}));
  CHECK(std::to_integer<uint8_t>(value) == 0xCC);
  REQUIRE(image.read(0x100001000, std::span<std::byte>{&value, 1}));
  CHECK(std::to_integer<uint8_t>(value) == 0xDD);

  const DyldCacheMetadata* metadata = asDyldCacheMetadata(image);
  REQUIRE(metadata != nullptr);
  CHECK(metadata->cacheType == DyldCacheType::Production);
  CHECK(metadata->sharedRegionStart == 0x100000000);
  REQUIRE(metadata->images.size() == 1);
  CHECK(metadata->images[0].path == "/usr/lib/libFoo.dylib");
  CHECK(metadata->imageContaining(0x100000000) == &metadata->images[0]);
  CHECK(metadata->imageNamed("/usr/lib/libFoo.dylib") == &metadata->images[0]);
}

TEST_CASE("dyld cache loader discovers a legacy numbered subcache sibling", "[dyld_cache]") {
  // Main file: header + one TEXT-only mapping + a one-entry v1 subCacheArray
  // naming a sibling ".1" purely by cacheVMOffset (no fileSuffix).
  DyldCacheBuilder main(0x3000);
  main.setMapping(456, 1);
  main.mapping(456, 0, 0x180000000, 0x1000, 0x1000, /*initProt=*/5);
  main.setSharedRegion(0x180000000, 0x3000);
  main.setSubCacheArray(488, 1);
  main.subCacheEntryV1(488, 0, 0x11, 0x2000);  // sibling maps at sharedRegionStart + 0x2000
  main.putByte(0x1000, 0xEE);

  DyldCacheBuilder sibling(0x2000);
  sibling.setMapping(456, 1);
  sibling.mapping(456, 0, 0x180002000, 0x1000, 0x1000, /*initProt=*/3);
  sibling.putByte(0x1000, 0xFF);

  const std::filesystem::path mainPath = testDir() / "dyld_split_arm64";
  main.writeTo(mainPath);
  sibling.writeTo(std::filesystem::path{mainPath.string() + ".1"});

  auto opened = openBinary(mainPath);
  REQUIRE(opened);
  const BinaryImage& image = **opened;

  REQUIRE(image.memory().regions().size() == 2);
  std::byte value{};
  REQUIRE(image.read(0x180000000, std::span<std::byte>{&value, 1}));
  CHECK(std::to_integer<uint8_t>(value) == 0xEE);
  // This byte only exists in the sibling file: proves cross-file backing
  // (MemoryRegion::backingIndex) actually selects the sibling's bytes.
  REQUIRE(image.read(0x180002000, std::span<std::byte>{&value, 1}));
  CHECK(std::to_integer<uint8_t>(value) == 0xFF);

  const DyldCacheMetadata* metadata = asDyldCacheMetadata(image);
  REQUIRE(metadata != nullptr);
  REQUIRE(metadata->parts.size() == 2);
  CHECK(metadata->parts[0].fileName == "dyld_split_arm64");
  CHECK(metadata->parts[1].fileName == "dyld_split_arm64.1");
  CHECK(metadata->parts[1].vmOffset == 0x2000);
}

TEST_CASE("dyld cache loader rejects an unrecognised architecture suffix", "[dyld_cache]") {
  DyldCacheBuilder builder(0x400);
  builder.putString(0, "dyld_v1   x86_64");
  builder.setMapping(456, 0);

  const std::filesystem::path path = testDir() / "dyld_unsupported_arch";
  builder.writeTo(path);

  auto opened = openBinary(path);
  REQUIRE_FALSE(opened);
  CHECK(opened.error().code() == DiagCode::UnsupportedArch);
}

TEST_CASE("dyld cache loader fails loudly when a subcache sibling is missing", "[dyld_cache]") {
  DyldCacheBuilder main(0x2000);
  main.setMapping(456, 1);
  main.mapping(456, 0, 0x180000000, 0x1000, 0x1000, /*initProt=*/5);
  main.setSubCacheArray(488, 1);
  main.subCacheEntryV1(488, 0, 0x22, 0x2000);

  const std::filesystem::path path = testDir() / "dyld_missing_sibling_arm64";
  main.writeTo(path);
  const std::filesystem::path orphanSibling{path.string() + ".1"};
  std::filesystem::remove(orphanSibling);

  auto opened = openBinary(path);
  REQUIRE_FALSE(opened);
  CHECK(opened.error().code() == DiagCode::IoError);
}
