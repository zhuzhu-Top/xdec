// ImageLiteralRecovery: a constant address recovers as a CString literal
// only where the image proves it immutable, readable, printable ASCII, and
// NUL-terminated within the bound -- the same safety rules
// passes/const_fold_memory.h applies to folding a Load, just consumed here
// at the emit layer instead (see xdec/analysis/image_literals.h).
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xdec/analysis/image_literals.h"

using xdec::ByteReader;
using xdec::MemoryFacts;
using xdec::analysis::ImageLiteral;
using xdec::analysis::ImageLiteralKind;
using xdec::analysis::ImageLiteralRecovery;
using xdec::analysis::quoteCString;

namespace {

struct FakeImage {
  uint64_t base = 0;
  std::vector<std::byte> bytes;
  /// Addresses this claims are immutable, independent of what is mapped --
  /// lets a test put an address in range for `reader()` but outside this,
  /// the "mapped but writable" case a real `.data` byte would be.
  bool immutableOverride = true;

  [[nodiscard]] ByteReader reader() const {
    return [this](uint64_t va, std::span<std::byte> out) -> xdec::Result<void> {
      if (va < base || va + out.size() > base + bytes.size()) {
        return xdec::err(xdec::DiagCode::UnmappedAddress, "out of range");
      }
      std::memcpy(out.data(), bytes.data() + (va - base), out.size());
      return {};
    };
  }

  [[nodiscard]] MemoryFacts facts() const {
    MemoryFacts out;
    out.immutable = [this](uint64_t va, uint64_t size) {
      return immutableOverride && va >= base && va + size <= base + bytes.size();
    };
    return out;
  }
};

[[nodiscard]] std::vector<std::byte> asBytes(std::string_view text) {
  std::vector<std::byte> out;
  out.reserve(text.size());
  for (const char c : text) {
    out.push_back(static_cast<std::byte>(c));
  }
  return out;
}

}  // namespace

TEST_CASE("a NUL-terminated printable run in immutable memory recovers as a CString",
          "[analysis][image-literals]") {
  FakeImage image;
  image.base = 0x1000;
  image.bytes = asBytes(std::string_view("ro.arch\0", 8));

  const ImageLiteralRecovery recovery(image.reader(), image.facts());
  const std::optional<ImageLiteral> literal = recovery.at(0x1000);
  REQUIRE(literal.has_value());
  CHECK(literal->kind == ImageLiteralKind::CString);
  CHECK(literal->text == "ro.arch");
}

TEST_CASE("no reader at all recovers nothing, unconditionally",
          "[analysis][image-literals]") {
  const ImageLiteralRecovery recovery(ByteReader{}, MemoryFacts{});
  CHECK_FALSE(recovery.at(0x1000).has_value());
}

TEST_CASE("a run outside any immutable range does not recover, even when readable",
          "[analysis][image-literals]") {
  FakeImage image;
  image.base = 0x1000;
  image.bytes = asBytes(std::string_view("hi\0", 3));
  image.immutableOverride = false;

  const ImageLiteralRecovery recovery(image.reader(), image.facts());
  CHECK_FALSE(recovery.at(0x1000).has_value());
}

TEST_CASE("a non-ASCII byte declines recovery rather than guessing an encoding",
          "[analysis][image-literals]") {
  FakeImage image;
  image.base = 0x1000;
  image.bytes = {std::byte{'h'}, std::byte{0xff}, std::byte{0}};

  const ImageLiteralRecovery recovery(image.reader(), image.facts());
  CHECK_FALSE(recovery.at(0x1000).has_value());
}

TEST_CASE("an unterminated run past the image's own bound does not recover",
          "[analysis][image-literals]") {
  FakeImage image;
  image.base = 0x1000;
  image.bytes = asBytes(std::string_view("no terminator here"));

  const ImageLiteralRecovery recovery(image.reader(), image.facts());
  CHECK_FALSE(recovery.at(0x1000).has_value());
}

TEST_CASE("an address that is itself the terminator recovers nothing, not an empty string",
          "[analysis][image-literals]") {
  FakeImage image;
  image.base = 0x1000;
  image.bytes = {std::byte{0}};

  const ImageLiteralRecovery recovery(image.reader(), image.facts());
  CHECK_FALSE(recovery.at(0x1000).has_value());
}

TEST_CASE("quoteCString escapes quotes and backslashes", "[analysis][image-literals]") {
  CHECK(quoteCString("plain") == "\"plain\"");
  CHECK(quoteCString(R"(a"b)") == R"("a\"b")");
  CHECK(quoteCString(R"(a\b)") == R"("a\\b")");
}
