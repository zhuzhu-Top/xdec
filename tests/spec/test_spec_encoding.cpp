// Overlap detection and the decoder tree.
//
// The tree is checked against brute force rather than against expectations
// about its shape: what matters is that it never loses a pattern that a linear
// scan would have found, whatever splitting heuristic it happens to pick.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <random>

#include "spec/spec_test_support.h"
#include "xdec/spec/check.h"
#include "xdec/spec/encoding.h"
#include "xdec/spec/parse.h"

using xdec::spec::DecisionTree;
using xdec::spec::EncodingPattern;
using xdec::spec::buildDecisionTree;
using xdec::spec::findOverlaps;

namespace {

/// The patterns a linear scan would consider, which is the answer the tree has
/// to reproduce.
[[nodiscard]] std::vector<uint32_t> bruteForce(const std::vector<EncodingPattern>& patterns,
                                               uint64_t word) {
  std::vector<uint32_t> matches;
  for (std::size_t index = 0; index < patterns.size(); ++index) {
    if ((word & patterns[index].mask) == patterns[index].value) {
      matches.push_back(static_cast<uint32_t>(index));
    }
  }
  return matches;
}

[[nodiscard]] EncodingPattern makePattern(uint32_t index, uint64_t mask, uint64_t value,
                                          int priority = 0, bool guards = false) {
  EncodingPattern pattern;
  pattern.instruction = index;
  pattern.name = std::to_string(index);
  pattern.mask = mask;
  pattern.value = value;
  pattern.priority = priority;
  pattern.hasGuards = guards;
  return pattern;
}

}  // namespace

TEST_CASE("overlapping encodings are reported", "[spec][encoding]") {
  SECTION("two rules that can match one word") {
    const std::vector<EncodingPattern> patterns = {
        makePattern(0, 0xf0000000, 0x10000000),
        makePattern(1, 0x0f000000, 0x00000000),
    };
    const auto report = findOverlaps(patterns);
    REQUIRE(report.conflicts.size() == 1);
    // The witness must actually be matched by both, so that the error message
    // shows a concrete instruction word rather than an assertion.
    const uint64_t witness = report.conflicts[0].witness;
    CHECK((witness & patterns[0].mask) == patterns[0].value);
    CHECK((witness & patterns[1].mask) == patterns[1].value);
  }

  SECTION("disjoint rules are not") {
    const std::vector<EncodingPattern> patterns = {
        makePattern(0, 0xf0000000, 0x10000000),
        makePattern(1, 0xf0000000, 0x20000000),
    };
    CHECK(findOverlaps(patterns).conflicts.empty());
  }

  SECTION("an alias refining a base encoding is accepted") {
    // This is the `mov` / `orr` relationship: same bits plus one more field.
    const std::vector<EncodingPattern> patterns = {
        makePattern(0, 0xf0000000, 0x10000000),
        makePattern(1, 0xf000001f, 0x1000001f),
    };
    CHECK(findOverlaps(patterns).conflicts.empty());
  }

  SECTION("an explicit priority settles a tie") {
    const std::vector<EncodingPattern> patterns = {
        makePattern(0, 0xf0000000, 0x10000000, 0),
        makePattern(1, 0x0f000000, 0x00000000, 5),
    };
    CHECK(findOverlaps(patterns).conflicts.empty());
  }

  SECTION("a guard is taken as an intentional separation") {
    const std::vector<EncodingPattern> patterns = {
        makePattern(0, 0xf0000000, 0x10000000, 0, /*guards=*/true),
        makePattern(1, 0x0f000000, 0x00000000),
    };
    CHECK(findOverlaps(patterns).conflicts.empty());
  }
}

TEST_CASE("the real spec has exactly the intended overlap", "[spec][encoding]") {
  const auto result = xdec::spec::check(xdec::spec::testing::arm64Module());
  INFO(result.report.format());
  REQUIRE(result.report.ok());

  // cmp is subs with Rd pinned to 31, and nothing else collides.
  CHECK(findOverlaps(result.module->patterns).conflicts.empty());
}

TEST_CASE("the decision tree agrees with a linear scan", "[spec][encoding]") {
  const auto result = xdec::spec::check(xdec::spec::testing::arm64Module());
  REQUIRE(result.report.ok());
  const std::vector<EncodingPattern>& patterns = result.module->patterns;
  const DecisionTree& tree = result.module->decoder;

  SECTION("on words drawn from the encodings themselves") {
    // Random words almost never decode to anything, so the interesting cases
    // have to be built from the patterns.
    std::mt19937_64 random{0x5eed};
    for (const EncodingPattern& pattern : patterns) {
      for (unsigned attempt = 0; attempt < 64; ++attempt) {
        const uint64_t word =
            (pattern.value & pattern.mask) | (random() & ~pattern.mask & 0xffffffffull);
        const std::vector<uint32_t> expected = bruteForce(patterns, word);
        const std::vector<uint32_t>& narrowed = tree.lookup(word);
        for (const uint32_t index : expected) {
          INFO("word 0x" << std::hex << word);
          CHECK(std::find(narrowed.begin(), narrowed.end(), index) != narrowed.end());
        }
      }
    }
  }

  SECTION("on random words") {
    std::mt19937_64 random{0xc0ffee};
    for (unsigned attempt = 0; attempt < 20000; ++attempt) {
      const uint64_t word = random() & 0xffffffffull;
      const std::vector<uint32_t> expected = bruteForce(patterns, word);
      const std::vector<uint32_t>& narrowed = tree.lookup(word);
      for (const uint32_t index : expected) {
        CHECK(std::find(narrowed.begin(), narrowed.end(), index) != narrowed.end());
      }
    }
  }

  SECTION("and it actually discriminates") {
    // A tree that returned every pattern at every leaf would pass the checks
    // above while being useless.
    CHECK(tree.worstLeaf() < patterns.size());
    CHECK(tree.nodeCount() > 1);
  }
}

TEST_CASE("candidates are offered in match order", "[spec][encoding]") {
  // A more specific pattern must be tried before the general one it refines,
  // otherwise every alias would be decoded as its base instruction.
  const std::vector<EncodingPattern> patterns = {
      makePattern(0, 0xf0000000, 0x10000000),
      makePattern(1, 0xf000001f, 0x1000001f),
  };
  const DecisionTree tree = buildDecisionTree(patterns, 32);
  const std::vector<uint32_t>& candidates = tree.lookup(0x1000001f);
  REQUIRE(candidates.size() == 2);
  CHECK(candidates[0] == 1);

  SECTION("an explicit priority outranks specificity") {
    std::vector<EncodingPattern> prioritised = patterns;
    prioritised[0].priority = 10;
    const DecisionTree ordered = buildDecisionTree(prioritised, 32);
    const std::vector<uint32_t>& order = ordered.lookup(0x1000001f);
    REQUIRE(order.size() == 2);
    CHECK(order[0] == 0);
  }
}

TEST_CASE("a tree over one pattern is a leaf", "[spec][encoding]") {
  const std::vector<EncodingPattern> patterns = {makePattern(0, 0xffffffff, 0xd65f03c0)};
  const DecisionTree tree = buildDecisionTree(patterns, 32);
  CHECK(tree.depth() == 1);
  CHECK(tree.lookup(0xd65f03c0).size() == 1);
  // The tree narrows; it does not confirm. A non-matching word may still reach
  // the leaf, and the decoder checks mask and value there.
  CHECK(tree.nodeCount() == 1);
}

TEST_CASE("a tree over no patterns is empty rather than broken", "[spec][encoding]") {
  const DecisionTree tree = buildDecisionTree({}, 32);
  CHECK(tree.lookup(0).empty());
  CHECK(tree.depth() == 1);
}
