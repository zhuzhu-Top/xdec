// apply-types: how many arguments a call has, once a header has said.
//
// Lifted from real ARM64 words rather than assembled by hand, because the thing
// under test is a claim about what SSA construction attached to a call and how
// much of it survives — building the operand list directly would test the test.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "xdec/il/verify.h"
#include "xdec/pass/manager.h"
#include "xdec/pass/registry.h"
#include "xdec/passes/builtin.h"
#include "xdec/spec/lift.h"
#include "xdec/types/parse.h"

#include "../spec/spec_test_support.h"

namespace il = xdec::il;
using xdec::Result;
using xdec::il::Function;
using xdec::il::Maturity;
using xdec::il::OpId;
using xdec::types::TypeDatabase;

namespace {

const xdec::spec::SpecEngine& engine() {
  static const std::unique_ptr<xdec::spec::SpecEngine> kEngine = [] {
    auto loaded = xdec::spec::loadSpecFile(xdec::spec::testing::arm64SpecPath());
    if (!loaded) {
      FAIL(loaded.error().format());
    }
    return std::move(loaded).value();
  }();
  return *kEngine;
}

class WordMemory {
 public:
  void put(uint64_t va, uint32_t word) { words_[va] = word; }

  [[nodiscard]] xdec::spec::ByteReader reader() const {
    return [this](uint64_t va, std::span<std::byte> out) -> Result<void> {
      const auto found = words_.find(va);
      if (found == words_.end() || out.size() != 4) {
        return xdec::err(xdec::DiagCode::BadFormat, "unmapped");
      }
      for (unsigned i = 0; i < 4; ++i) {
        out[i] = static_cast<std::byte>((found->second >> (i * 8)) & 0xff);
      }
      return xdec::ok();
    };
  }

 private:
  std::map<uint64_t, uint32_t> words_;
};

/// `bl <pc-relative>`, the ordinary direct call.
[[nodiscard]] constexpr uint32_t bl(int64_t displacement) {
  return 0x94000000U |
         (static_cast<uint32_t>((displacement >> 2) & 0x03ffffff));
}
/// `blr x<rn>`: a call through whatever the register holds.
[[nodiscard]] constexpr uint32_t blr(unsigned rn) { return 0xd63f0000U | (rn << 5U); }
constexpr uint32_t kRet = 0xd65f03c0U;

/// The function at 0x1000 is `entry`; 0x2000 is `callee`. Anything else is
/// nameless, which is what most of a stripped binary is.
[[nodiscard]] xdec::pass::NameAt names() {
  return [](uint64_t va) {
    if (va == 0x1000) {
      return xdec::pass::SymbolName{"entry", true};
    }
    if (va == 0x2000) {
      return xdec::pass::SymbolName{"callee", true};
    }
    return xdec::pass::SymbolName{};
  };
}

[[nodiscard]] TypeDatabase parse(std::string_view header) {
  TypeDatabase database;
  Result<xdec::types::ParseReport> report = xdec::types::parseHeader(header, database);
  // Built first: `<<` binds tighter than `?:`, so a conditional inside INFO
  // would be parsed as a condition on the message builder.
  const std::string note = report ? report->format("<test>") : report.error().format();
  INFO(note);
  REQUIRE(report);
  REQUIRE(report->skipped == 0);
  return database;
}

/// Lifts at 0x1000, runs the stock pipeline with the given header imported (or
/// none), and returns how many arguments the one call op came out with.
[[nodiscard]] std::size_t argCount(const WordMemory& memory,
                                   const TypeDatabase* database) {
  auto lifted = xdec::spec::liftFunction(engine(), memory.reader(), 0x1000);
  const std::string liftError = lifted ? std::string{} : lifted.error().format();
  INFO(liftError);
  REQUIRE(lifted);

  auto function = std::move(lifted->function);
  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);
  xdec::pass::Manager manager;
  manager.setImage(memory.reader());
  manager.setTypeDatabase(database);
  manager.setNames(names());
  auto ran = manager.runTo(*function, registry, Maturity::Vars);
  const std::string runError = ran ? std::string{} : ran.error().format();
  INFO(runError);
  REQUIRE(ran);

  const il::VerifyReport report = il::verify(*function, Maturity::Vars);
  for (const xdec::Diag& diag : report.errors) {
    INFO(diag.format());
  }
  REQUIRE(report.ok());

  OpId call;
  for (const il::BlockId blockId : function->blockHandles()) {
    for (const OpId opId : function->block(blockId).ops) {
      if (function->op(opId).code == il::OpCode::Call) {
        call = opId;
      }
    }
  }
  REQUIRE(call.valid());
  // Operand zero is the target; the rest are the arguments.
  return function->operands(function->op(call)).size() - 1;
}

}  // namespace

// The baseline every other case is measured against: with nothing imported,
// every argument register is a possible argument and the call keeps all eight.
TEST_CASE("without a header a call keeps the whole convention",
          "[passes][apply-types]") {
  WordMemory memory;
  memory.put(0x1000, bl(0x1000));
  memory.put(0x1004, kRet);
  CHECK(argCount(memory, nullptr) == 8);
}

TEST_CASE("a direct call is trimmed to the imported prototype's arity",
          "[passes][apply-types]") {
  WordMemory memory;
  memory.put(0x1000, bl(0x1000));
  memory.put(0x1004, kRet);

  const TypeDatabase database = parse("int callee(void *dst, unsigned long n);");
  CHECK(argCount(memory, &database) == 2);
}

// A prototype that declares more than the convention attached describes a call
// this code does not make. The extra parameters are not invented.
TEST_CASE("a call is never extended past what the convention attached",
          "[passes][apply-types]") {
  WordMemory memory;
  memory.put(0x1000, bl(0x1000));
  memory.put(0x1004, kRet);

  const TypeDatabase database = parse(
      "int callee(long a, long b, long c, long d, long e, long f, long g, long h, "
      "long i, long j);");
  CHECK(argCount(memory, &database) == 8);
}

// `printf` really does read as many registers as its caller set.
TEST_CASE("a variadic prototype trims nothing", "[passes][apply-types]") {
  WordMemory memory;
  memory.put(0x1000, bl(0x1000));
  memory.put(0x1004, kRet);

  const TypeDatabase database = parse("int callee(const char *format, ...);");
  CHECK(argCount(memory, &database) == 8);
}

// The indirect case: nothing is known about the address in x0, but the header
// says x0 is this function's first parameter and that the parameter is a
// pointer to a function of two arguments.
TEST_CASE("a call through a typed parameter is trimmed to the pointee's arity",
          "[passes][apply-types]") {
  WordMemory memory;
  memory.put(0x1000, blr(0));
  memory.put(0x1004, kRet);

  const TypeDatabase database =
      parse("int entry(int (*op)(int, int), int a, int b);");
  CHECK(argCount(memory, &database) == 2);
}

// The same shape with no prototype for the *enclosing* function: x0 is then a
// register with an unknown value, and nothing may be concluded from it.
TEST_CASE("an untyped indirect call keeps every argument", "[passes][apply-types]") {
  WordMemory memory;
  memory.put(0x1000, blr(0));
  memory.put(0x1004, kRet);

  const TypeDatabase database = parse("int callee(void *dst, unsigned long n);");
  CHECK(argCount(memory, &database) == 8);
}
