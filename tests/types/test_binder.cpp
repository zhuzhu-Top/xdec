// TypeBinder::consistent: the one place a header's claim is weighed against
// what the code proved, and TypeDatabase::findPointerTo, the const lookup
// that lets that weighing happen without a mutable database.
#include <catch2/catch_test_macros.hpp>

#include "xdec/types/binder.h"
#include "xdec/types/database.h"

using namespace xdec::types;

namespace {

[[nodiscard]] TypeBinder binder(const TypeDatabase& database) {
  return TypeBinder(database, [](uint64_t) { return BoundName{}; });
}

}  // namespace

TEST_CASE("no inferred opinion accepts whatever the header says", "[types][binder]") {
  TypeDatabase database;
  const TypeId intType = database.lookup("int");
  CHECK(binder(database).consistent(intType, /*inferredBits=*/0, /*inferredPointerDepth=*/0));
}

TEST_CASE("a pointer inference rejects a non-pointer header", "[types][binder]") {
  TypeDatabase database;
  const TypeId intType = database.lookup("int");
  // The code dereferenced it, whatever the header calls it.
  CHECK_FALSE(binder(database).consistent(intType, /*inferredBits=*/32, /*inferredPointerDepth=*/1));
}

TEST_CASE("a width mismatch rejects the header", "[types][binder]") {
  TypeDatabase database;
  const TypeId int32 = database.lookup("int");
  CHECK_FALSE(binder(database).consistent(int32, /*inferredBits=*/64, /*inferredPointerDepth=*/0));
  CHECK(binder(database).consistent(int32, /*inferredBits=*/32, /*inferredPointerDepth=*/0));
}

TEST_CASE("any pointer agrees with a 64-bit inference regardless of pointee",
          "[types][binder]") {
  TypeDatabase database;
  const TypeId pointer = database.pointerTo(database.lookup("uint32_t"));
  CHECK(binder(database).consistent(pointer, /*inferredBits=*/64, /*inferredPointerDepth=*/1));
  CHECK(binder(database).consistent(pointer, /*inferredBits=*/64, /*inferredPointerDepth=*/0));
}

TEST_CASE("an incomplete type contradicts nothing", "[types][binder]") {
  TypeDatabase database;
  const TypeId forward = database.declareTag(TypeKind::Struct, "Node");
  CHECK(binder(database).consistent(forward, /*inferredBits=*/64, /*inferredPointerDepth=*/0));
}

TEST_CASE("findPointerTo answers only what pointerTo already interned",
          "[types]") {
  TypeDatabase database;
  const TypeId intType = database.lookup("int");
  CHECK_FALSE(database.findPointerTo(intType).has_value());
  const TypeId pointer = database.pointerTo(intType);
  REQUIRE(database.findPointerTo(intType).has_value());
  CHECK(*database.findPointerTo(intType) == pointer);
}
