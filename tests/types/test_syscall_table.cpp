#include <catch2/catch_test_macros.hpp>

#include "xdec/types/database.h"
#include "xdec/types/parse.h"
#include "xdec/types/syscall_table.h"

using namespace xdec;
using namespace xdec::types;

TEST_CASE("the shipped aarch64 table loads and answers", "[types][syscall]") {
  const Result<std::string> path = SyscallTable::resolvePath(SyscallTable::defaultName());
  REQUIRE(path.hasValue());
  const Result<SyscallTable> table = SyscallTable::loadFile(*path);
  REQUIRE(table.hasValue());
  REQUIRE(table->arch() == "aarch64");
  REQUIRE(table->size() > 250);

  const SyscallInfo* write = table->find(64);
  REQUIRE(write != nullptr);
  REQUIRE(write->name == "write");
  REQUIRE(write->argCount == 3);
  REQUIRE(write->hasSignature());
  REQUIRE(write->argTypes[1] == "const void*");
  REQUIRE(write->returnType == "ssize_t");

  const SyscallInfo* gettimeofday = table->find(169);
  REQUIRE(gettimeofday != nullptr);
  REQUIRE(gettimeofday->name == "gettimeofday");
  REQUIRE(gettimeofday->argTypes[0] == "struct timeval*");

  // Arity without types: enough to print the right number of arguments.
  const SyscallInfo* futexWaitv = table->find(449);
  REQUIRE(futexWaitv != nullptr);
  REQUIRE(futexWaitv->argCount == 5);
  REQUIRE_FALSE(futexWaitv->hasSignature());

  const SyscallInfo* exitGroup = table->find(94);
  REQUIRE(exitGroup != nullptr);
  REQUIRE(exitGroup->noreturn);

  // A number the kernel does not define stays unknown rather than becoming a
  // neighbouring syscall.
  REQUIRE(table->find(9999) == nullptr);
  REQUIRE(table->find(447) == nullptr);
}

TEST_CASE("an empty table misses everything", "[types][syscall]") {
  const SyscallTable table;
  REQUIRE(table.empty());
  REQUIRE(table.find(64) == nullptr);
}

TEST_CASE("malformed tables are rejected with the offending entry named",
          "[types][syscall]") {
  auto load = [](const char* text) {
    const Result<json::Value> document = json::parse(text);
    REQUIRE(document.hasValue());
    return SyscallTable::fromJson(*document);
  };

  REQUIRE_FALSE(load(R"({"arch":"aarch64"})").hasValue());
  REQUIRE_FALSE(load(R"({"syscalls":{"write":{"name":"write","argc":3}}})").hasValue());
  REQUIRE_FALSE(load(R"({"syscalls":{"64":{"argc":3}}})").hasValue());
  REQUIRE_FALSE(load(R"({"syscalls":{"64":{"name":"write","argc":9}}})").hasValue());
  // args and argc disagreeing is the kind of edit that would silently drop an
  // argument at the emitter.
  REQUIRE_FALSE(
      load(R"({"syscalls":{"64":{"name":"write","args":["int","void*"],"argc":3}}})")
          .hasValue());
  REQUIRE(load(R"({"syscalls":{"64":{"name":"write","argc":3}}})").hasValue());
}

TEST_CASE("resolveTypes turns a syscall's argument spellings into TypeIds",
          "[types][syscall]") {
  const Result<std::string> path = SyscallTable::resolvePath(SyscallTable::defaultName());
  REQUIRE(path.hasValue());
  Result<SyscallTable> table = SyscallTable::loadFile(*path);
  REQUIRE(table.hasValue());

  TypeDatabase database;
  // The struct tags alone are not enough: nothing interns "struct timeval*"
  // (see TypeDatabase::findPointerTo) until some declaration actually takes
  // one, so a prototype naming each pointer is included purely to force that.
  const Result<ParseReport> report = parseHeader(
      "struct timeval { long tv_sec; long tv_usec; };\n"
      "struct timezone { int tz_minuteswest; int tz_dsttime; };\n"
      "void gettimeofday_proto(struct timeval *tv, struct timezone *tz);\n",
      database);
  REQUIRE(report.hasValue());

  const SyscallInfo* beforeGettimeofday = table->find(169);
  REQUIRE(beforeGettimeofday != nullptr);
  CHECK(beforeGettimeofday->argTypeIds.empty());  // not resolved yet

  table->resolveTypes(database);
  const SyscallInfo* gettimeofday = table->find(169);
  REQUIRE(gettimeofday != nullptr);
  REQUIRE(gettimeofday->argTypeIds.size() == 2);
  // "struct timeval*": one level of pointer over the tag this header declared.
  const TypeId timeval = database.lookup("timeval", NameSpace::Tag);
  REQUIRE(timeval.valid());
  CHECK(gettimeofday->argTypeIds[0] == database.pointerTo(timeval));
  // "struct timezone*" is declared here too, so it resolves the same way.
  CHECK(gettimeofday->argTypeIds[1].valid());

  // A number this header never declared a tag for still resolves cleanly to
  // an invalid TypeId rather than crashing the pass that reads it.
  const SyscallInfo* write = table->find(64);
  REQUIRE(write != nullptr);
  REQUIRE(write->argTypeIds.size() == write->argTypes.size());
  CHECK(write->returnTypeId.valid());  // "ssize_t" is a builtin, always present
}
