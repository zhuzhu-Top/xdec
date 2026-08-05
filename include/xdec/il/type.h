// IL value types.
//
// Types at the LIR and MIR levels describe machine values only: an integer of a
// given bit width, a float, an opaque lazy-flag bundle, or a vector of those.
// Source-level notions (pointers, structs, signedness) belong to the typed HIR
// level and deliberately do not exist here -- an `add` does not care, and
// pretending otherwise this early invents information the binary does not have.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "xdec/support/compiler.h"

namespace xdec::il {

enum class TypeKind : uint8_t {
  Void = 0,
  /// Integer of `elementBits` width. Width 1 is the boolean produced by
  /// comparisons; widths need not be powers of two.
  Int,
  Float,
  /// An opaque condition-flag bundle produced by FlagDef and consumed by
  /// FlagCond. Keeping it opaque is what makes flags lazy: the individual bits
  /// are never materialised unless something actually asks for one.
  Flags,
  /// `lanes` integer lanes of `elementBits`.
  IntVector,
  /// `lanes` float lanes of `elementBits`.
  FloatVector,
};

[[nodiscard]] std::string_view toString(TypeKind kind) noexcept;

class Type {
 public:
  static constexpr unsigned kMaxBits = 2048;
  static constexpr unsigned kMaxLanes = 255;

  constexpr Type() = default;

  [[nodiscard]] static constexpr Type voidType() noexcept { return Type{}; }

  [[nodiscard]] static constexpr Type integer(unsigned bits) noexcept {
    return Type{TypeKind::Int, static_cast<uint16_t>(bits), 1};
  }

  [[nodiscard]] static constexpr Type boolean() noexcept { return integer(1); }

  [[nodiscard]] static constexpr Type floating(unsigned bits) noexcept {
    return Type{TypeKind::Float, static_cast<uint16_t>(bits), 1};
  }

  [[nodiscard]] static constexpr Type flags() noexcept { return Type{TypeKind::Flags, 0, 1}; }

  [[nodiscard]] static constexpr Type intVector(unsigned elementBits, unsigned lanes) noexcept {
    return Type{TypeKind::IntVector, static_cast<uint16_t>(elementBits),
                static_cast<uint8_t>(lanes)};
  }

  [[nodiscard]] static constexpr Type floatVector(unsigned elementBits, unsigned lanes) noexcept {
    return Type{TypeKind::FloatVector, static_cast<uint16_t>(elementBits),
                static_cast<uint8_t>(lanes)};
  }

  [[nodiscard]] constexpr TypeKind kind() const noexcept { return kind_; }
  [[nodiscard]] constexpr unsigned elementBits() const noexcept { return elementBits_; }
  [[nodiscard]] constexpr unsigned lanes() const noexcept { return lanes_; }

  /// Total width in bits. Void and Flags have no width.
  [[nodiscard]] constexpr unsigned bits() const noexcept {
    return static_cast<unsigned>(elementBits_) * static_cast<unsigned>(lanes_);
  }

  [[nodiscard]] constexpr bool isVoid() const noexcept { return kind_ == TypeKind::Void; }
  [[nodiscard]] constexpr bool isInteger() const noexcept { return kind_ == TypeKind::Int; }
  [[nodiscard]] constexpr bool isFloat() const noexcept { return kind_ == TypeKind::Float; }
  [[nodiscard]] constexpr bool isFlags() const noexcept { return kind_ == TypeKind::Flags; }
  [[nodiscard]] constexpr bool isVector() const noexcept {
    return kind_ == TypeKind::IntVector || kind_ == TypeKind::FloatVector;
  }
  [[nodiscard]] constexpr bool isBoolean() const noexcept {
    return kind_ == TypeKind::Int && elementBits_ == 1;
  }

  /// True for types a 64-bit constant can represent exactly.
  [[nodiscard]] constexpr bool isScalarInteger() const noexcept {
    return kind_ == TypeKind::Int && elementBits_ > 0 && elementBits_ <= 64;
  }

  [[nodiscard]] constexpr bool valid() const noexcept {
    switch (kind_) {
      case TypeKind::Void:
      case TypeKind::Flags:
        return elementBits_ == 0 && lanes_ == 1;
      case TypeKind::Int:
      case TypeKind::Float:
        return elementBits_ > 0 && elementBits_ <= kMaxBits && lanes_ == 1;
      case TypeKind::IntVector:
      case TypeKind::FloatVector:
        return elementBits_ > 0 && lanes_ > 1 && bits() <= kMaxBits;
    }
    return false;
  }

  friend constexpr bool operator==(Type lhs, Type rhs) noexcept {
    return lhs.kind_ == rhs.kind_ && lhs.elementBits_ == rhs.elementBits_ &&
           lhs.lanes_ == rhs.lanes_;
  }
  friend constexpr bool operator!=(Type lhs, Type rhs) noexcept { return !(lhs == rhs); }

  /// Round-trippable spelling: `void`, `i1`, `i64`, `f64`, `flags`, `i32x4`.
  [[nodiscard]] std::string toString() const;

  /// Parses the spelling produced by toString(). Returns false on failure.
  [[nodiscard]] static bool parse(std::string_view text, Type& out) noexcept;

  /// Packed form for hashing and serialisation.
  [[nodiscard]] constexpr uint32_t packed() const noexcept {
    return (static_cast<uint32_t>(kind_) << 24) | (static_cast<uint32_t>(elementBits_) << 8) |
           static_cast<uint32_t>(lanes_);
  }

 private:
  constexpr Type(TypeKind kind, uint16_t elementBits, uint8_t lanes) noexcept
      : elementBits_(elementBits), kind_(kind), lanes_(lanes) {}

  // Ordered so the whole type fits in four bytes with no padding: it is stored
  // inline in every expression node.
  uint16_t elementBits_ = 0;
  TypeKind kind_ = TypeKind::Void;
  uint8_t lanes_ = 1;
};

static_assert(sizeof(Type) == 4, "Type is stored inline in every Expr; keep it small");

}  // namespace xdec::il
