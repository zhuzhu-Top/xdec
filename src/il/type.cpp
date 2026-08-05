#include "xdec/il/type.h"

#include <charconv>
#include <format>

namespace xdec::il {

std::string_view toString(TypeKind kind) noexcept {
  switch (kind) {
    case TypeKind::Void:
      return "void";
    case TypeKind::Int:
      return "int";
    case TypeKind::Float:
      return "float";
    case TypeKind::Flags:
      return "flags";
    case TypeKind::IntVector:
      return "int-vector";
    case TypeKind::FloatVector:
      return "float-vector";
  }
  return "invalid";
}

std::string Type::toString() const {
  switch (kind_) {
    case TypeKind::Void:
      return "void";
    case TypeKind::Flags:
      return "flags";
    case TypeKind::Int:
      return std::format("i{}", elementBits_);
    case TypeKind::Float:
      return std::format("f{}", elementBits_);
    case TypeKind::IntVector:
      return std::format("i{}x{}", elementBits_, lanes_);
    case TypeKind::FloatVector:
      return std::format("f{}x{}", elementBits_, lanes_);
  }
  return "invalid";
}

namespace {

/// Parses a run of decimal digits, requiring at least one and rejecting values
/// that would not fit the field they are destined for.
bool parseUnsigned(std::string_view text, unsigned& out) noexcept {
  if (text.empty()) {
    return false;
  }
  unsigned value = 0;
  const auto* end = text.data() + text.size();
  const auto result = std::from_chars(text.data(), end, value, 10);
  if (result.ec != std::errc{} || result.ptr != end) {
    return false;
  }
  out = value;
  return true;
}

}  // namespace

bool Type::parse(std::string_view text, Type& out) noexcept {
  if (text == "void") {
    out = voidType();
    return true;
  }
  if (text == "flags") {
    out = flags();
    return true;
  }
  if (text.size() < 2) {
    return false;
  }

  const char prefix = text[0];
  if (prefix != 'i' && prefix != 'f') {
    return false;
  }
  std::string_view rest = text.substr(1);

  unsigned lanes = 1;
  if (const std::size_t cross = rest.find('x'); cross != std::string_view::npos) {
    if (!parseUnsigned(rest.substr(cross + 1), lanes)) {
      return false;
    }
    // `i32x1` is rejected rather than read as `i32`: one spelling per type keeps
    // the text canonical, and an explicit single lane means whatever produced it
    // believed the type was a vector.
    if (lanes == 1) {
      return false;
    }
    rest = rest.substr(0, cross);
  }

  unsigned elementBits = 0;
  if (!parseUnsigned(rest, elementBits)) {
    return false;
  }
  if (elementBits == 0 || elementBits > kMaxBits || lanes == 0 || lanes > kMaxLanes) {
    return false;
  }

  const Type parsed = lanes == 1
                          ? (prefix == 'i' ? integer(elementBits) : floating(elementBits))
                          : (prefix == 'i' ? intVector(elementBits, lanes)
                                           : floatVector(elementBits, lanes));
  if (!parsed.valid()) {
    return false;
  }
  out = parsed;
  return true;
}

}  // namespace xdec::il
