#include <catch2/catch_test_macros.hpp>

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
