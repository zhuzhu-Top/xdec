#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "xdec/support/arena.h"

using namespace xdec;

namespace {

struct Node {
  uint64_t address;
  uint32_t left;
  uint32_t right;
};

bool isAlignedTo(const void* pointer, std::size_t alignment) {
  return reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0;
}

}  // namespace

TEST_CASE("arena honours requested alignment", "[arena]") {
  Arena arena{1024};
  for (int iteration = 0; iteration < 200; ++iteration) {
    // Interleave a one-byte allocation so the bump pointer is left misaligned.
    void* single = arena.allocate(1, 1);
    CHECK(single != nullptr);
    for (std::size_t alignment : {2u, 4u, 8u, 16u, 32u, 64u}) {
      void* block = arena.allocate(alignment * 3, alignment);
      CHECK(block != nullptr);
      CHECK(isAlignedTo(block, alignment));
    }
  }
}

TEST_CASE("arena returns distinct non-overlapping blocks", "[arena]") {
  Arena arena{256};
  std::vector<Node*> nodes;
  for (uint32_t index = 0; index < 500; ++index) {
    nodes.push_back(arena.create<Node>(Node{index, index + 1, index + 2}));
  }
  // Values survive the chunk growth that 500 nodes in 256-byte chunks forces.
  for (uint32_t index = 0; index < 500; ++index) {
    CHECK(nodes[index]->address == index);
    CHECK(nodes[index]->left == index + 1);
    CHECK(nodes[index]->right == index + 2);
  }
  const std::set<Node*> unique{nodes.begin(), nodes.end()};
  CHECK(unique.size() == nodes.size());
  CHECK(arena.chunkCount() > 1);
}

TEST_CASE("arena allocates and copies arrays", "[arena]") {
  Arena arena;
  const std::span<uint32_t> block = arena.allocateArray<uint32_t>(64);
  REQUIRE(block.size() == 64);
  CHECK(isAlignedTo(block.data(), alignof(uint32_t)));
  for (std::size_t index = 0; index < block.size(); ++index) {
    block[index] = static_cast<uint32_t>(index * 7);
  }
  CHECK(block[63] == 63 * 7);

  const std::vector<uint16_t> source{1, 2, 3, 4, 5};
  const std::span<uint16_t> copy = arena.copyArray<uint16_t>(source);
  REQUIRE(copy.size() == source.size());
  CHECK(std::equal(copy.begin(), copy.end(), source.begin()));

  // A zero-length array is legal and yields an empty span.
  CHECK(arena.allocateArray<uint64_t>(0).empty());
}

TEST_CASE("saveString NUL-terminates", "[arena]") {
  Arena arena;
  const std::string_view saved = arena.saveString("mnemonic");
  CHECK(saved == "mnemonic");
  // The terminator lets the same storage be handed to C APIs.
  CHECK(saved.data()[saved.size()] == '\0');

  const std::string_view empty = arena.saveString("");
  CHECK(empty.empty());
  CHECK(empty.data()[0] == '\0');
}

TEST_CASE("internString deduplicates", "[arena]") {
  Arena arena;
  const std::string_view first = arena.internString("ldr");
  const std::string_view second = arena.internString("ldr");
  const std::string_view other = arena.internString("str");

  CHECK(first == "ldr");
  // Equal inputs must return the identical storage, not just equal contents:
  // callers rely on pointer identity to compare names cheaply.
  CHECK(first.data() == second.data());
  CHECK(first.data() != other.data());

  // Interning a string built at runtime still finds the existing entry.
  std::string dynamic;
  dynamic += 'l';
  dynamic += 'd';
  dynamic += 'r';
  CHECK(arena.internString(dynamic).data() == first.data());
}

TEST_CASE("arena tracks and resets usage", "[arena]") {
  Arena arena{1024};
  CHECK(arena.bytesUsed() == 0);

  CHECK(arena.allocate(100, 8) != nullptr);
  CHECK(arena.bytesUsed() >= 100);
  const std::size_t afterFirst = arena.bytesUsed();
  CHECK(arena.allocate(100, 8) != nullptr);
  CHECK(arena.bytesUsed() > afterFirst);

  CHECK(arena.internString("kept-until-reset") == "kept-until-reset");
  arena.reset();
  CHECK(arena.bytesUsed() == 0);
  // One chunk is retained so a reset-and-reuse cycle does not thrash malloc.
  CHECK(arena.chunkCount() == 1);

  // The interner must have been cleared: its previous entries point into
  // storage that has now been handed out again.
  const std::string_view again = arena.internString("kept-until-reset");
  CHECK(again == "kept-until-reset");
}

TEST_CASE("arena serves allocations larger than its chunk size", "[arena]") {
  Arena arena{64};
  const std::span<uint8_t> big = arena.allocateArray<uint8_t>(10000);
  REQUIRE(big.size() == 10000);
  big[0] = 1;
  big[9999] = 2;
  CHECK(big[0] == 1);
  CHECK(big[9999] == 2);
}

TEST_CASE("arena is movable", "[arena]") {
  Arena source{1024};
  const std::string_view text = source.saveString("moved");
  Node* node = source.create<Node>(Node{42, 0, 0});

  Arena destination{std::move(source)};
  // Moving must not relocate the storage: existing pointers stay valid.
  CHECK(text == "moved");
  CHECK(node->address == 42);
  CHECK(destination.bytesUsed() > 0);
}
