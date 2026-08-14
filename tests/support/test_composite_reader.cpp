// CompositeByteReader: routing a read across several images by translated
// address, primary first (see support/composite_reader.h).
#include <catch2/catch_test_macros.hpp>

#include <cstring>

#include "xdec/support/composite_reader.h"

using xdec::ByteReader;
using xdec::CompositeByteReader;
using xdec::ImageRegion;

namespace {

[[nodiscard]] ByteReader mapOf(std::initializer_list<std::pair<uint64_t, uint8_t>> bytes) {
  auto map = std::make_shared<std::vector<std::pair<uint64_t, uint8_t>>>(bytes);
  return [map](uint64_t va, std::span<std::byte> out) -> xdec::Result<void> {
    for (std::size_t index = 0; index < out.size(); ++index) {
      bool found = false;
      for (const auto& [address, value] : *map) {
        if (address == va + index) {
          out[index] = static_cast<std::byte>(value);
          found = true;
          break;
        }
      }
      if (!found) {
        return xdec::err(xdec::DiagCode::UnmappedAddress, "not mapped");
      }
    }
    return {};
  };
}

}  // namespace

TEST_CASE("an empty composite answers nothing, the same as an unmapped address",
          "[support][composite-reader]") {
  CompositeByteReader composite;
  CHECK(composite.empty());
  std::byte out{};
  CHECK_FALSE(composite.reader()(0x1000, std::span<std::byte>(&out, 1)).hasValue());
}

TEST_CASE("the primary region is tried first and needs no translation",
          "[support][composite-reader]") {
  CompositeByteReader composite;
  composite.addRegion(ImageRegion{.name = "primary", .reader = mapOf({{0x1000, 0xAB}})});

  std::byte out{};
  const auto result = composite.reader()(0x1000, std::span<std::byte>(&out, 1));
  REQUIRE(result.hasValue());
  CHECK(out == std::byte{0xAB});
}

TEST_CASE("a companion region translates the composite address into its own file space",
          "[support][composite-reader]") {
  CompositeByteReader composite;
  // Primary image has nothing at 0x2000...
  composite.addRegion(ImageRegion{.name = "primary", .reader = mapOf({{0x1000, 0xAB}})});
  // ...but a companion mapped at runtime base 0x2000, whose own file bytes
  // start at 0 (a fresh capture with no ASLR slide relative to itself),
  // does: composite VA 0x2000 + 4 should read the companion's own byte 4.
  composite.addRegion(ImageRegion{.name = "dyld",
                                  .reader = mapOf({{4, 0xCD}}),
                                  .runtimeBase = 0x2000,
                                  .fileBase = 0});

  std::byte out{};
  const auto result = composite.reader()(0x2004, std::span<std::byte>(&out, 1));
  REQUIRE(result.hasValue());
  CHECK(out == std::byte{0xCD});

  // An address the primary cannot answer and that falls outside every
  // companion's own mapped bytes fails, rather than reading garbage.
  std::byte missing{};
  CHECK_FALSE(composite.reader()(0x9999, std::span<std::byte>(&missing, 1)).hasValue());
}
