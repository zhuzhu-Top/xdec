// ImageLiteralRecovery (see the header for the safety rules).
#include "xdec/analysis/image_literals.h"

namespace xdec::analysis {

std::optional<ImageLiteral> ImageLiteralRecovery::at(uint64_t va, bool allowEmpty) const {
  if (!reader_) {
    return std::nullopt;
  }
  std::string text;
  text.reserve(32);
  for (std::size_t offset = 0; offset < kMaxLength; ++offset) {
    const uint64_t byteVa = va + offset;
    if (!facts_.isImmutable(byteVa, 1)) {
      return std::nullopt;
    }
    std::byte byte{};
    if (!reader_(byteVa, std::span<std::byte>{&byte, 1})) {
      return std::nullopt;
    }
    const auto value = std::to_integer<uint8_t>(byte);
    if (value == 0) {
      // An empty string is not worth naming in general .rodata, where every
      // zero byte would look like `""`; dedicated string sections opt in via
      // `allowEmpty = true`.
      return (offset == 0 && !allowEmpty)
                 ? std::nullopt
                 : std::make_optional(ImageLiteral{ImageLiteralKind::CString, std::move(text)});
    }
    if (value < 0x20 || value > 0x7e) {
      return std::nullopt;  // not printable ASCII; do not guess the encoding
    }
    text.push_back(static_cast<char>(value));
  }
  return std::nullopt;  // no terminator within the bound
}

std::string quoteCString(std::string_view text) {
  std::string out = "\"";
  out.reserve(text.size() + 2);
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
    }
    out += c;
  }
  out += '"';
  return out;
}

}  // namespace xdec::analysis
