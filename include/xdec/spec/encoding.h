// Encoding analysis: overlap detection and decoder decision trees.
//
// Two encodings that match the same instruction word are the single most common
// spec bug, and the resulting decoder silently picks whichever rule it happens
// to test first. Detecting the overlap at spec-compile time turns that into an
// error naming both rules.
//
// Genuine overlaps do exist -- an alias such as `mov` is `orr` with Rn == 31 --
// so an overlap is accepted when one rule is strictly more specific than the
// other, or when the spec breaks the tie with an explicit priority.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "xdec/support/diag.h"

namespace xdec::spec {

/// One decodable rule, reduced to what the decoder needs.
struct EncodingPattern {
  /// Index into the module's instruction list.
  uint32_t instruction = 0;
  std::string name;
  /// Bits the pattern constrains.
  uint64_t mask = 0;
  /// Required values in the constrained bits.
  uint64_t value = 0;
  /// Higher wins when two patterns overlap.
  int priority = 0;
  /// The rule has `require` clauses, so matching the bits is necessary but not
  /// sufficient and the decoder must evaluate them.
  bool hasGuards = false;
};

/// A node in the decoder decision tree.
///
/// An internal node reads a contiguous run of bits and dispatches on its value.
/// A pattern that does not constrain those bits appears in every child, which is
/// what keeps the tree correct in the presence of wildcards.
struct DecisionNode {
  /// Low bit of the field this node switches on. Meaningless for a leaf.
  unsigned shift = 0;
  /// Width of that field; zero marks a leaf.
  unsigned width = 0;
  /// `1 << width` children, indexed by the field value. Empty for a leaf.
  std::vector<std::unique_ptr<DecisionNode>> children;
  /// Candidates in match order: descending priority, then descending
  /// specificity. Only populated on leaves.
  std::vector<uint32_t> candidates;

  [[nodiscard]] bool isLeaf() const noexcept { return width == 0; }
};

struct DecisionTree {
  std::unique_ptr<DecisionNode> root;
  /// Instruction width in bits.
  unsigned insnWidth = 0;

  /// Patterns that could match `word`, in match order. The decoder still has to
  /// confirm mask and value plus any guards; the tree only narrows the search.
  [[nodiscard]] const std::vector<uint32_t>& lookup(uint64_t word) const;

  /// Deepest path length, for reporting how well the tree discriminates.
  [[nodiscard]] unsigned depth() const;
  /// Largest leaf candidate list, which bounds the decoder's worst case.
  [[nodiscard]] std::size_t worstLeaf() const;
  [[nodiscard]] std::size_t nodeCount() const;
};

struct OverlapReport {
  struct Conflict {
    uint32_t first = 0;
    uint32_t second = 0;
    /// An instruction word both patterns match, as evidence.
    uint64_t witness = 0;
  };
  std::vector<Conflict> conflicts;
};

/// Reports pairs of patterns that match a common word and that neither
/// specificity nor priority separates.
[[nodiscard]] OverlapReport findOverlaps(const std::vector<EncodingPattern>& patterns);

/// Builds a decision tree over the patterns. Never fails: a set of patterns
/// that cannot be discriminated simply produces a larger leaf.
[[nodiscard]] DecisionTree buildDecisionTree(const std::vector<EncodingPattern>& patterns,
                                             unsigned insnWidth);

}  // namespace xdec::spec
