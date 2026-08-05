// Symbolic compile-time integers.
//
// Every integer in a spec is either a literal or something derived from decoded
// instruction fields, so at spec-compile time it is symbolic and at lift time it
// is concrete. This class represents the symbolic half.
//
// It exists so that width polymorphism can be checked rather than assumed. One
// rule covers both the 32- and 64-bit forms of an instruction by declaring its
// operand width as `32 << sf`; the checker can then prove that two operands of
// an `add` have the same width because both widths intern to the same node,
// without knowing what `sf` is.
//
// Nodes are hash-consed and folded on construction, so equality is a handle
// comparison and `32 << 1` is indistinguishable from `64`.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xdec/support/handle.h"

namespace xdec::spec {

struct SymTag;
using SymId = Handle<SymTag>;

enum class SymOp : uint8_t {
  Const,
  /// A named unknown: a decoded field or a function parameter.
  Symbol,
  /// A value the checker cannot model. Comparisons against it are never proven
  /// equal, which is what makes unknowns fail loudly rather than pass silently.
  Unknown,
  Add,
  Sub,
  Mul,
  DivU,
  RemU,
  And,
  Or,
  Xor,
  Shl,
  ShrU,
  ShrS,
  Not,
  Negate,
  /// Comparisons produce 0 or 1.
  Equal,
  NotEqual,
  LessU,
  LessS,
  /// `Select(c, a, b)`.
  Select,
};

struct SymNode {
  SymOp op = SymOp::Const;
  uint64_t value = 0;
  /// Symbol name, interned by the pool.
  uint32_t symbol = 0;
  SymId operands[3];
  uint8_t operandCount = 0;

  friend bool operator==(const SymNode& lhs, const SymNode& rhs) noexcept;
};

}  // namespace xdec::spec

// Declared before SymPool, which stores nodes in a hash map.
template <>
struct std::hash<xdec::spec::SymNode> {
  [[nodiscard]] std::size_t operator()(const xdec::spec::SymNode& node) const noexcept {
    std::size_t digest = 0xcbf29ce484222325ull;
    const auto mix = [&digest](uint64_t value) {
      for (unsigned byte = 0; byte < 8; ++byte) {
        digest ^= static_cast<std::size_t>((value >> (byte * 8)) & 0xFF);
        digest *= 0x100000001b3ull;
      }
    };
    mix(static_cast<uint64_t>(node.op));
    mix(node.value);
    mix(node.symbol);
    for (unsigned index = 0; index < node.operandCount; ++index) {
      mix(node.operands[index].index());
    }
    return digest;
  }
};

namespace xdec::spec {

class SymPool {
 public:
  SymPool();

  [[nodiscard]] SymId constant(uint64_t value);
  [[nodiscard]] SymId symbol(std::string_view name);
  [[nodiscard]] SymId unknown();

  [[nodiscard]] SymId unary(SymOp op, SymId operand);
  [[nodiscard]] SymId binary(SymOp op, SymId lhs, SymId rhs);
  [[nodiscard]] SymId select(SymId condition, SymId ifTrue, SymId ifFalse);

  [[nodiscard]] const SymNode& node(SymId id) const { return nodes_[id]; }
  [[nodiscard]] bool isConstant(SymId id) const;
  /// The literal value, when the node is one.
  [[nodiscard]] bool asConstant(SymId id, uint64_t& out) const;
  [[nodiscard]] bool isUnknown(SymId id) const;
  /// True when the two denote the same value for every assignment of symbols.
  /// Conservative: unequal handles may still be equal in fact.
  [[nodiscard]] bool provablyEqual(SymId lhs, SymId rhs) const;

  /// Replaces every occurrence of `name` with `value`, folding as it goes. Used
  /// to refine an environment inside a compile-time `if`.
  [[nodiscard]] SymId substitute(SymId id, std::string_view name, uint64_t value);

  [[nodiscard]] std::string_view symbolName(uint32_t id) const;
  [[nodiscard]] std::string toString(SymId id) const;

 private:
  [[nodiscard]] SymId intern(const SymNode& node);
  [[nodiscard]] uint32_t internSymbol(std::string_view name);

  HandleVector<SymId, SymNode> nodes_;
  std::unordered_map<SymNode, SymId> index_;
  std::vector<std::string> symbolNames_;
  std::unordered_map<std::string, uint32_t> symbolIndex_;
  SymId unknown_;
};

}  // namespace xdec::spec
