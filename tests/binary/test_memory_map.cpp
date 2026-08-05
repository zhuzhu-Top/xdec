#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

#include "xdec/binary/memory_map.h"

using namespace xdec;
using namespace xdec::binary;

namespace {

/// Backing bytes 0x00..0xFF repeating, so any read can be checked against its
/// file offset.
std::vector<std::byte> makeBacking(std::size_t size) {
  std::vector<std::byte> bytes(size);
  for (std::size_t index = 0; index < size; ++index) {
    bytes[index] = static_cast<std::byte>(index & 0xFF);
  }
  return bytes;
}

MemoryRegion region(uint64_t va, uint64_t size, uint64_t fileOffset, uint64_t fileSize,
                    MemoryPermissions permissions, std::string name) {
  MemoryRegion result;
  result.va = va;
  result.size = size;
  result.fileOffset = fileOffset;
  result.fileSize = fileSize;
  result.permissions = permissions;
  result.name = std::move(name);
  return result;
}

}  // namespace

TEST_CASE("permissions render as rwx", "[memory]") {
  CHECK(toString(MemoryPermissions::None) == "---");
  CHECK(toString(MemoryPermissions::Read) == "r--");
  CHECK(toString(MemoryPermissions::Read | MemoryPermissions::Execute) == "r-x");
  CHECK(toString(MemoryPermissions::Read | MemoryPermissions::Write) == "rw-");
}

TEST_CASE("memory map reads file-backed bytes", "[memory]") {
  const std::vector<std::byte> backing = makeBacking(0x1000);
  MemoryMap map;
  map.setBackingBytes(backing);
  map.addRegion(region(0x1000, 0x100, 0x40, 0x100, MemoryPermissions::Read, "data"));
  REQUIRE(map.finalize());

  std::byte out[4] = {};
  REQUIRE(map.read(0x1000, out));
  CHECK(std::to_integer<uint8_t>(out[0]) == 0x40);
  CHECK(std::to_integer<uint8_t>(out[3]) == 0x43);

  REQUIRE(map.read(0x1010, out));
  CHECK(std::to_integer<uint8_t>(out[0]) == 0x50);
}

TEST_CASE("memory map zero-fills the tail past file bytes", "[memory]") {
  // This is the .bss case: the region occupies 0x100 bytes of memory but only
  // the first 0x10 come from the file. A section-header-driven reader returns
  // nothing here; the runtime view must return zeros.
  const std::vector<std::byte> backing = makeBacking(0x1000);
  MemoryMap map;
  map.setBackingBytes(backing);
  map.addRegion(region(0x2000, 0x100, 0x80, 0x10, MemoryPermissions::Read, "data+bss"));
  REQUIRE(map.finalize());

  std::byte out[0x20] = {};
  REQUIRE(map.read(0x2000, out));
  // First 0x10 bytes come from the file...
  CHECK(std::to_integer<uint8_t>(out[0]) == 0x80);
  CHECK(std::to_integer<uint8_t>(out[0x0F]) == 0x8F);
  // ...and the rest is the zero-initialised tail, not a failure.
  for (std::size_t index = 0x10; index < 0x20; ++index) {
    CHECK(std::to_integer<uint8_t>(out[index]) == 0);
  }

  // A read starting entirely inside the tail also succeeds.
  std::byte tail[8] = {};
  REQUIRE(map.read(0x2050, tail));
  for (const std::byte value : tail) {
    CHECK(std::to_integer<uint8_t>(value) == 0);
  }
}

TEST_CASE("memory map reads across adjacent regions", "[memory]") {
  const std::vector<std::byte> backing = makeBacking(0x1000);
  MemoryMap map;
  map.setBackingBytes(backing);
  map.addRegion(region(0x3000, 0x10, 0x00, 0x10, MemoryPermissions::Read, "first"));
  map.addRegion(region(0x3010, 0x10, 0x80, 0x10, MemoryPermissions::Read, "second"));
  REQUIRE(map.finalize());

  std::byte out[0x20] = {};
  REQUIRE(map.read(0x3000, out));
  CHECK(std::to_integer<uint8_t>(out[0x0F]) == 0x0F);
  // Crossing the boundary must pick up the second region's file offset.
  CHECK(std::to_integer<uint8_t>(out[0x10]) == 0x80);
}

TEST_CASE("memory map fails loudly on unmapped addresses", "[memory]") {
  const std::vector<std::byte> backing = makeBacking(0x1000);
  MemoryMap map;
  map.setBackingBytes(backing);
  map.addRegion(region(0x4000, 0x10, 0x00, 0x10, MemoryPermissions::Read, "only"));
  REQUIRE(map.finalize());

  CHECK_FALSE(map.isMapped(0x3FFF));
  CHECK(map.isMapped(0x4000));
  CHECK(map.isMapped(0x400F));
  CHECK_FALSE(map.isMapped(0x4010));

  std::byte out[4] = {};
  auto belowStart = map.read(0x3FFC, out);
  REQUIRE_FALSE(belowStart);
  CHECK(belowStart.error().code() == DiagCode::UnmappedAddress);

  // A read that starts inside but runs off the end must fail, not truncate.
  auto runningOff = map.read(0x400E, out);
  REQUIRE_FALSE(runningOff);
  CHECK(runningOff.error().code() == DiagCode::UnmappedAddress);
  CHECK(runningOff.error().address() == 0x4010);
}

TEST_CASE("memory map rejects overlapping regions", "[memory]") {
  const std::vector<std::byte> backing = makeBacking(0x1000);
  MemoryMap map;
  map.setBackingBytes(backing);
  map.addRegion(region(0x5000, 0x100, 0x00, 0x100, MemoryPermissions::Read, "a"));
  map.addRegion(region(0x5080, 0x100, 0x00, 0x100, MemoryPermissions::Read, "b"));

  // Two regions claiming one address would make every read through it
  // ambiguous, so this has to be an error rather than a silent preference.
  auto finalized = map.finalize();
  REQUIRE_FALSE(finalized);
  CHECK(finalized.error().code() == DiagCode::BadFormat);
}

TEST_CASE("memory map rejects regions reaching past the file", "[memory]") {
  const std::vector<std::byte> backing = makeBacking(0x100);
  MemoryMap map;
  map.setBackingBytes(backing);
  map.addRegion(region(0x6000, 0x200, 0x80, 0x200, MemoryPermissions::Read, "too-big"));

  auto finalized = map.finalize();
  REQUIRE_FALSE(finalized);
  CHECK(finalized.error().code() == DiagCode::BadFormat);
}

TEST_CASE("memory map rejects a file size larger than the memory size", "[memory]") {
  const std::vector<std::byte> backing = makeBacking(0x1000);
  MemoryMap map;
  map.setBackingBytes(backing);
  map.addRegion(region(0x7000, 0x10, 0x00, 0x20, MemoryPermissions::Read, "inverted"));

  auto finalized = map.finalize();
  REQUIRE_FALSE(finalized);
  CHECK(finalized.error().code() == DiagCode::BadFormat);
}

TEST_CASE("directView is zero-copy only when fully file-backed", "[memory]") {
  const std::vector<std::byte> backing = makeBacking(0x1000);
  MemoryMap map;
  map.setBackingBytes(backing);
  map.addRegion(region(0x8000, 0x100, 0x200, 0x80, MemoryPermissions::Read | MemoryPermissions::Execute,
                       "code+zeros"));
  REQUIRE(map.finalize());

  const auto inside = map.directView(0x8000, 0x40);
  REQUIRE(inside.size() == 0x40);
  CHECK(std::to_integer<uint8_t>(inside[0]) == 0x00);
  CHECK(std::to_integer<uint8_t>(inside[1]) == 0x01);

  // Spilling into the zero-filled tail cannot be served as a view into the
  // file, so callers must be told to fall back rather than handed short data.
  CHECK(map.directView(0x8070, 0x20).empty());
  CHECK(map.directView(0x8080, 0x10).empty());
  CHECK(map.directView(0x9000, 0x10).empty());
  CHECK(map.directView(0x8000, 0).empty());
}

TEST_CASE("memory map reports its bounds and sorts regions", "[memory]") {
  const std::vector<std::byte> backing = makeBacking(0x1000);
  MemoryMap map;
  map.setBackingBytes(backing);
  // Added out of order on purpose.
  map.addRegion(region(0xB000, 0x10, 0x00, 0x10, MemoryPermissions::Read, "third"));
  map.addRegion(region(0x9000, 0x10, 0x00, 0x10, MemoryPermissions::Read, "first"));
  map.addRegion(region(0xA000, 0x10, 0x00, 0x10, MemoryPermissions::Read, "second"));
  REQUIRE(map.finalize());

  REQUIRE(map.regions().size() == 3);
  CHECK(map.regions()[0].name == "first");
  CHECK(map.regions()[1].name == "second");
  CHECK(map.regions()[2].name == "third");
  CHECK(map.lowestAddress() == 0x9000);
  CHECK(map.highestAddress() == 0xB010);

  // Zero-sized regions are dropped rather than confusing the ordering.
  MemoryMap other;
  other.setBackingBytes(backing);
  other.addRegion(region(0x100, 0, 0, 0, MemoryPermissions::Read, "empty"));
  REQUIRE(other.finalize());
  CHECK(other.empty());
}
