#include "xdec/spec/encoding.h"

#include <algorithm>

#include "xdec/support/bits.h"

namespace xdec::spec {
namespace {

/// Two patterns can match the same word exactly when they agree on every bit
/// they both constrain.
[[nodiscard]] bool patternsOverlap(const EncodingPattern& a, const EncodingPattern& b) noexcept {
  const uint64_t common = a.mask & b.mask;
  return ((a.value ^ b.value) & common) == 0;
}

/// A witness word that both match: take each pattern's required bits, and leave
/// the unconstrained bits zero.
[[nodiscard]] uint64_t overlapWitness(const EncodingPattern& a,
                                      const EncodingPattern& b) noexcept {
  return (a.value & a.mask) | (b.value & b.mask);
}

/// True when `a` constrains a strict superset of `b`'s bits. Such a pattern is
/// unambiguously the more specific of the two, which is exactly the shape of an
/// alias like `mov` sitting on top of `orr`.
[[nodiscard]] bool strictlyMoreSpecific(const EncodingPattern& a,
                                        const EncodingPattern& b) noexcept {
  return (a.mask & b.mask) == b.mask && a.mask != b.mask;
}

/// Ordering the decoder tries candidates in: highest priority first, then the
/// most constrained pattern, then declaration order for stability.
[[nodiscard]] bool matchesBefore(const EncodingPattern& a, const EncodingPattern& b) noexcept {
  if (a.priority != b.priority) {
    return a.priority > b.priority;
  }
  const unsigned aBits = countOnes(a.mask);
  const unsigned bBits = countOnes(b.mask);
  if (aBits != bBits) {
    return aBits > bBits;
  }
  return a.instruction < b.instruction;
}

/// How many bits a run must discriminate before it is worth a tree level. A
/// wider field means more children, so the split has to pay for itself. Four
/// bits is the point where the fan-out stops paying: any pattern that leaves
/// the field unconstrained is copied into every one of the 2^w children, so a
/// spec with catch-all rules buys a wide split at exponential cost in nodes.
constexpr unsigned kMaxFieldWidth = 4;
constexpr std::size_t kLeafThreshold = 4;
constexpr unsigned kMaxDepth = 12;

struct Candidate {
  unsigned shift = 0;
  unsigned width = 0;
  /// Largest child bucket; lower is a better split.
  std::size_t worstChild = 0;
  /// Patterns that do not constrain the field and therefore land in every
  /// bucket. They are pure cost.
  std::size_t wildcards = 0;
};

/// Scores a candidate field by the size of the largest bucket it produces.
[[nodiscard]] Candidate scoreField(const std::vector<EncodingPattern>& patterns,
                                   const std::vector<uint32_t>& active, unsigned shift,
                                   unsigned width) {
  Candidate candidate;
  candidate.shift = shift;
  candidate.width = width;

  const uint64_t fieldMask = lowMask(width) << shift;
  std::vector<std::size_t> buckets(std::size_t{1} << width, 0);

  for (const uint32_t index : active) {
    const EncodingPattern& pattern = patterns[index];
    if ((pattern.mask & fieldMask) != fieldMask) {
      // Partially or wholly unconstrained: it has to go everywhere.
      ++candidate.wildcards;
      continue;
    }
    const auto bucket = static_cast<std::size_t>((pattern.value & fieldMask) >> shift);
    ++buckets[bucket];
  }

  const std::size_t largest = *std::max_element(buckets.begin(), buckets.end());
  candidate.worstChild = largest + candidate.wildcards;
  return candidate;
}

std::unique_ptr<DecisionNode> buildNode(const std::vector<EncodingPattern>& patterns,
                                        std::vector<uint32_t> active, unsigned insnWidth,
                                        unsigned depth) {
  auto node = std::make_unique<DecisionNode>();

  if (active.size() <= kLeafThreshold || depth >= kMaxDepth) {
    node->candidates = std::move(active);
    return node;
  }

  // Pick the field that splits the active set best. Scanning every start and
  // width is affordable: instruction words are at most 64 bits and this runs
  // once per spec compile.
  //
  // Selection is by worst child, but only down to the leaf threshold. Below it
  // a narrower field is strictly better, because recursion stops there anyway
  // and the extra bits buy nothing but children.
  //
  // Without that floor the widest field almost always wins on worst-child alone,
  // and a spec with a catch-all rule pays for it twice over: the catch-all is a
  // wildcard everywhere, so it lands in all 256 buckets of an eight-bit split,
  // none of the buckets is empty, and every one of them recurses. Two levels of
  // that is seventy thousand nodes to distinguish a hundred and fifty patterns.
  Candidate best;
  bool haveBest = false;
  const auto effective = [](const Candidate& candidate) {
    return std::max(candidate.worstChild, kLeafThreshold);
  };
  for (unsigned width = kMaxFieldWidth; width >= 1; --width) {
    for (unsigned shift = 0; shift + width <= insnWidth; ++shift) {
      const Candidate candidate = scoreField(patterns, active, shift, width);
      // A field every pattern ignores splits nothing.
      if (candidate.wildcards == active.size()) {
        continue;
      }
      // Ranked by how much linear scanning is left, then by how cleanly the
      // field partitions, then by how few children it costs. A field that every
      // active pattern constrains is preferred at equal quality because its
      // children sum to the parent, while one that some pattern ignores copies
      // that pattern into all 2^w children.
      const auto rank = [&effective](const Candidate& c) {
        return std::tuple{effective(c), c.wildcards != 0, c.width};
      };
      if (!haveBest || rank(candidate) < rank(best)) {
        best = candidate;
        haveBest = true;
      }
    }
  }

  // No field improves on testing every candidate, so stop rather than build a
  // level that only adds indirection.
  if (!haveBest || best.worstChild >= active.size()) {
    std::sort(active.begin(), active.end(), [&patterns](uint32_t lhs, uint32_t rhs) {
      return matchesBefore(patterns[lhs], patterns[rhs]);
    });
    node->candidates = std::move(active);
    return node;
  }

  node->shift = best.shift;
  node->width = best.width;
  const uint64_t fieldMask = lowMask(best.width) << best.shift;
  const std::size_t childCount = std::size_t{1} << best.width;

  std::vector<std::vector<uint32_t>> buckets(childCount);
  for (const uint32_t index : active) {
    const EncodingPattern& pattern = patterns[index];
    if ((pattern.mask & fieldMask) != fieldMask) {
      for (std::vector<uint32_t>& bucket : buckets) {
        bucket.push_back(index);
      }
      continue;
    }
    const auto slot = static_cast<std::size_t>((pattern.value & fieldMask) >> best.shift);
    buckets[slot].push_back(index);
  }

  // An empty bucket is a null child rather than an empty leaf. A wide field over
  // a sparse encoding space leaves most buckets empty, and allocating a node for
  // each of them is how a hundred and fifty patterns turn into a tree with tens
  // of thousands of nodes.
  node->children.reserve(childCount);
  for (std::vector<uint32_t>& bucket : buckets) {
    node->children.push_back(bucket.empty()
                                 ? nullptr
                                 : buildNode(patterns, std::move(bucket), insnWidth, depth + 1));
  }
  return node;
}

unsigned nodeDepth(const DecisionNode* node) {
  if (node == nullptr || node->isLeaf()) {
    return 1;
  }
  unsigned deepest = 0;
  for (const std::unique_ptr<DecisionNode>& child : node->children) {
    deepest = std::max(deepest, nodeDepth(child.get()));
  }
  return deepest + 1;
}

std::size_t nodeWorstLeaf(const DecisionNode* node) {
  if (node == nullptr) {
    return 0;
  }
  if (node->isLeaf()) {
    return node->candidates.size();
  }
  std::size_t worst = 0;
  for (const std::unique_ptr<DecisionNode>& child : node->children) {
    worst = std::max(worst, nodeWorstLeaf(child.get()));
  }
  return worst;
}

std::size_t countNodes(const DecisionNode* node) {
  if (node == nullptr) {
    return 0;
  }
  std::size_t total = 1;
  for (const std::unique_ptr<DecisionNode>& child : node->children) {
    total += countNodes(child.get());
  }
  return total;
}

}  // namespace

OverlapReport findOverlaps(const std::vector<EncodingPattern>& patterns) {
  OverlapReport report;
  for (std::size_t i = 0; i < patterns.size(); ++i) {
    for (std::size_t j = i + 1; j < patterns.size(); ++j) {
      const EncodingPattern& a = patterns[i];
      const EncodingPattern& b = patterns[j];
      if (!patternsOverlap(a, b)) {
        continue;
      }
      // An explicit priority is the spec author saying which wins.
      if (a.priority != b.priority) {
        continue;
      }
      // One rule refining another is the normal alias relationship.
      if (strictlyMoreSpecific(a, b) || strictlyMoreSpecific(b, a)) {
        continue;
      }
      // Guards can separate rules in ways the bit pattern cannot express, and
      // proving that would need a solver. Trusting them silently would hide
      // real conflicts, so this is reported as a conflict only when neither
      // rule has guards.
      if (a.hasGuards || b.hasGuards) {
        continue;
      }
      report.conflicts.push_back(
          OverlapReport::Conflict{a.instruction, b.instruction, overlapWitness(a, b)});
    }
  }
  return report;
}

DecisionTree buildDecisionTree(const std::vector<EncodingPattern>& patterns,
                               unsigned insnWidth) {
  DecisionTree tree;
  tree.insnWidth = insnWidth;

  std::vector<uint32_t> active;
  active.reserve(patterns.size());
  for (std::size_t index = 0; index < patterns.size(); ++index) {
    active.push_back(static_cast<uint32_t>(index));
  }
  std::sort(active.begin(), active.end(), [&patterns](uint32_t lhs, uint32_t rhs) {
    return matchesBefore(patterns[lhs], patterns[rhs]);
  });

  tree.root = buildNode(patterns, std::move(active), insnWidth, 0);
  return tree;
}

const std::vector<uint32_t>& DecisionTree::lookup(uint64_t word) const {
  static const std::vector<uint32_t> kEmpty;
  const DecisionNode* node = root.get();
  if (node == nullptr) {
    return kEmpty;
  }
  while (!node->isLeaf()) {
    const auto slot =
        static_cast<std::size_t>((word >> node->shift) & lowMask(node->width));
    if (slot >= node->children.size() || node->children[slot] == nullptr) {
      return kEmpty;
    }
    node = node->children[slot].get();
  }
  return node->candidates;
}

unsigned DecisionTree::depth() const { return nodeDepth(root.get()); }

std::size_t DecisionTree::worstLeaf() const { return nodeWorstLeaf(root.get()); }

std::size_t DecisionTree::nodeCount() const { return countNodes(root.get()); }

}  // namespace xdec::spec
