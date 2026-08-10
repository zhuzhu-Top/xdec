// calleeThroughImportSlot / resolveCallee: the prototype behind a GOT/import
// slot call, and the TargetProfile alias that lets a loader's spelling of a
// symbol (Bionic's `__errno`) bind against a header's different spelling for
// the same function (`__errno_location`).
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

#include "il/il_test_support.h"
#include "xdec/analysis/import_callee.h"
#include "xdec/binary/target_profile.h"
#include "xdec/il/function.h"
#include "xdec/types/binder.h"
#include "xdec/types/database.h"
#include "xdec/types/parse.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::LoaderValue;
using xdec::MemoryFacts;
using xdec::analysis::calleeThroughImportSlot;
using xdec::analysis::resolveCallee;
using xdec::binary::TargetProfile;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::Function;
using xdec::il::Type;
using xdec::types::BoundName;
using xdec::types::TypeBinder;
using xdec::types::TypeDatabase;

namespace {

[[nodiscard]] TypeDatabase parse(std::string_view header) {
  TypeDatabase database;
  const auto report = xdec::types::parseHeader(header, database);
  const std::string note = report ? report->format("<test>") : report.error().format();
  INFO(note);
  REQUIRE(report);
  return database;
}

[[nodiscard]] TypeBinder binder(const TypeDatabase& database) {
  return TypeBinder(database, [](uint64_t) { return BoundName{}; });
}

/// A GOT slot read through one `Load` -- the shape a lifted `ldr xN, [got]`
/// leaves behind, whether or not this test's `facts` binds the slot to an
/// import.
struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {
    entry = function.createBlock(0x1000);
    function.setEntryBlock(entry);
  }

  ExprId gotLoad(uint64_t slot) {
    return function.valueRef(function.appendLoad(
        entry, 0x1000, Type::integer(64), function.constant(Type::integer(64), slot)));
  }

  [[nodiscard]] static MemoryFacts boundTo(uint64_t slot, std::string importName) {
    MemoryFacts facts;
    facts.loader = [slot, name = std::move(importName)](uint64_t va) {
      LoaderValue value;
      if (va == slot) {
        value.importName = name;
      }
      return value;
    };
    return facts;
  }

  Function function;
  BlockId entry;
};

}  // namespace

TEST_CASE("a GOT slot's import name binds against the header declaring it",
          "[analysis][import-callee]") {
  Fixture f;
  const TypeDatabase database = parse("int callee(void *dst, unsigned long n);");
  const TypeBinder b = binder(database);
  const MemoryFacts facts = Fixture::boundTo(0x3000, "callee");
  const auto* prototype = calleeThroughImportSlot(f.function, b, facts, f.gotLoad(0x3000));
  REQUIRE(prototype != nullptr);
  CHECK(prototype->params.size() == 2);
}

TEST_CASE("a GOT slot with no loader facts resolves to nothing",
          "[analysis][import-callee]") {
  Fixture f;
  const TypeDatabase database = parse("int callee(void *dst, unsigned long n);");
  const TypeBinder b = binder(database);
  CHECK(calleeThroughImportSlot(f.function, b, MemoryFacts{}, f.gotLoad(0x3000)) == nullptr);
}

TEST_CASE("without an alias a loader name that differs from the header's finds nothing",
          "[analysis][import-callee]") {
  Fixture f;
  const TypeDatabase database = parse("int *__errno_location(void);");
  const TypeBinder b = binder(database);
  const MemoryFacts facts = Fixture::boundTo(0x3000, "__errno");
  CHECK(calleeThroughImportSlot(f.function, b, facts, f.gotLoad(0x3000)) == nullptr);
}

// The exact sub_199214 shape: Bionic's dynamic symbol table names the
// syscall-error accessor `__errno`; the NDK header declares it as
// `__errno_location`. TargetProfile::symbolAliases is what lets the same
// call bind to that declaration (see docs/10-import-resolution.md).
TEST_CASE("a TargetProfile alias lets a loader name bind to the header's own spelling",
          "[analysis][import-callee]") {
  Fixture f;
  const TypeDatabase database = parse("int *__errno_location(void);");
  const TypeBinder b = binder(database);
  const MemoryFacts facts = Fixture::boundTo(0x3000, "__errno");
  TargetProfile profile;
  profile.symbolAliases.emplace("__errno", "__errno_location");

  const auto* prototype = calleeThroughImportSlot(f.function, b, facts, f.gotLoad(0x3000), &profile);
  REQUIRE(prototype != nullptr);
  CHECK(prototype->params.empty());
}

TEST_CASE("resolveCallee falls back to a function-pointer parameter's declared type",
          "[analysis][import-callee]") {
  Fixture f;
  const TypeDatabase database = parse("int entry(int (*op)(int, int), int a, int b);");
  const TypeBinder b = binder(database);
  const auto* self = b.prototypeFor(b.forName("entry"));
  REQUIRE(self != nullptr);

  const ExprId target = f.function.entryReg(f.function.registers().find("x0"));
  const auto* prototype = resolveCallee(f.function, b, self, MemoryFacts{}, target);
  REQUIRE(prototype != nullptr);
  CHECK(prototype->params.size() == 2);
}

TEST_CASE("resolveCallee prefers the symbol at a constant address over anything else",
          "[analysis][import-callee]") {
  Fixture f;
  const TypeDatabase database = parse("int callee(void *dst, unsigned long n);");
  TypeBinder b(database, [](uint64_t va) {
    return va == 0x2000 ? BoundName{"callee", true} : BoundName{};
  });
  const ExprId target = f.function.constant(Type::integer(64), 0x2000);
  const auto* prototype = resolveCallee(f.function, b, /*self=*/nullptr, MemoryFacts{}, target);
  REQUIRE(prototype != nullptr);
  CHECK(prototype->params.size() == 2);
}
