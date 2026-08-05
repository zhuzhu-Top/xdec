#include <catch2/catch_test_macros.hpp>

#include "xdec/types/parse.h"

using namespace xdec;
using namespace xdec::types;

namespace {

/// Parses into a fresh database and requires that nothing was skipped, so a
/// test that means to exercise the happy path cannot silently be exercising
/// the recovery path instead.
[[nodiscard]] TypeDatabase parseClean(std::string_view text) {
  TypeDatabase database;
  Result<ParseReport> report = parseHeader(text, database);
  REQUIRE(report.hasValue());
  INFO(report->format("<test>"));
  REQUIRE(report->skipped == 0);
  return database;
}

}  // namespace

TEST_CASE("typedefs, structs and enums import", "[types][parse]") {
  const TypeDatabase database = parseClean(R"(
    typedef struct { int32_t x, y, z; } EvalVec3;
    typedef enum { EVAL_ADD = 0, EVAL_SUB, EVAL_MUL = 4 } EvalKind;
    typedef uint64_t SizeTy;
    struct EvalNode {
      int32_t left;
      int32_t right;
      struct EvalNode *parent;
    };
  )");

  const TypeId vec = database.lookup("EvalVec3");
  REQUIRE(vec.valid());
  REQUIRE(database.sizeOf(vec) == 12);
  REQUIRE(database.format(vec) == "EvalVec3");

  const TypeId kind = database.lookup("EvalKind");
  REQUIRE(kind.valid());
  const TypeEntry* kindEntry = database.get(kind);
  REQUIRE(kindEntry->constants.size() == 3);
  REQUIRE(kindEntry->constants[1].value == 1);  // implicit increment
  REQUIRE(kindEntry->constants[2].value == 4);

  REQUIRE(database.resolveTypedef(database.lookup("SizeTy")) == database.lookup("uint64_t"));

  const TypeId node = database.lookup("EvalNode", NameSpace::Tag);
  REQUIRE(node.valid());
  REQUIRE(database.sizeOf(node) == 16);
  const TypeDatabase::FieldPath path = database.fieldAt(node, 8);
  REQUIRE(path.names == std::vector<std::string>{"parent"});
}

TEST_CASE("declarator binding follows C, not left-to-right reading", "[types][parse]") {
  const TypeDatabase database = parseClean(R"(
    typedef int32_t *IntPtrArray[4];
    typedef int32_t (*ArrayPtr)[4];
    typedef int32_t (*BinOp)(int32_t, int32_t);
    typedef int32_t (*OpTable[3])(int32_t, int32_t);
    typedef char *ArgV[];
    typedef int32_t Matrix[2][3];
  )");

  // Each name is a typedef, which prints as itself; the shape under it is
  // what these cases are about.
  auto spell = [&](const char* alias, const char* name) {
    return database.declare(database.resolveTypedef(database.lookup(alias)), name);
  };

  REQUIRE(spell("IntPtrArray", "v") == "int32_t* v[4]");
  REQUIRE(spell("ArrayPtr", "v") == "int32_t (*v)[4]");
  REQUIRE(spell("BinOp", "op") == "int32_t (*op)(int32_t, int32_t)");
  REQUIRE(spell("OpTable", "t") == "int32_t (*t[3])(int32_t, int32_t)");
  REQUIRE(spell("ArgV", "v") == "int8_t* v[]");
  // `int m[2][3]` is 2 rows of 3, so the outer dimension is the leftmost.
  REQUIRE(spell("Matrix", "m") == "int32_t m[2][3]");
  REQUIRE(database.sizeOf(database.lookup("Matrix")) == 24);
}

TEST_CASE("prototypes and extern globals become declarations", "[types][parse]") {
  const TypeDatabase database = parseClean(R"(
    typedef struct { uint64_t calls; uint64_t errors; } EvalStats;
    extern EvalStats g_stats;
    int32_t eval_helper(const EvalStats *stats, size_t n);
    int printf(const char *format, ...);
    void no_args(void);
    void *dlsym(void *handle, const char *symbol);
  )");

  const Declaration* helper = database.findDeclaration("eval_helper");
  REQUIRE(helper != nullptr);
  REQUIRE(helper->isFunction);
  // `size_t`, not the `uint64_t` it resolves to: a platform typedef keeps its
  // name, which is the whole reason a prototype is worth importing.
  REQUIRE(database.format(helper->type) == "int32_t (EvalStats* stats, size_t n)");

  const Declaration* stats = database.findDeclaration("g_stats");
  REQUIRE(stats != nullptr);
  REQUIRE_FALSE(stats->isFunction);
  REQUIRE(database.format(stats->type) == "EvalStats");

  const Declaration* variadic = database.findDeclaration("printf");
  REQUIRE(variadic != nullptr);
  REQUIRE(database.get(variadic->type)->variadic);
  REQUIRE(database.format(variadic->type) == "int32_t (int8_t* format, ...)");

  REQUIRE(database.format(database.findDeclaration("no_args")->type) == "void (void)");
  REQUIRE(database.format(database.findDeclaration("dlsym")->type) ==
          "void* (void* handle, int8_t* symbol)");
}

TEST_CASE("integer keyword combinations canonicalise", "[types][parse]") {
  const TypeDatabase database = parseClean(R"(
    typedef unsigned long int ULong;
    typedef long unsigned AlsoULong;
    typedef signed char SChar;
    typedef unsigned Unsigned;
    typedef long long LongLong;
  )");

  REQUIRE(database.resolveTypedef(database.lookup("ULong")) == database.lookup("uint64_t"));
  REQUIRE(database.resolveTypedef(database.lookup("AlsoULong")) == database.lookup("uint64_t"));
  REQUIRE(database.resolveTypedef(database.lookup("SChar")) == database.lookup("int8_t"));
  REQUIRE(database.resolveTypedef(database.lookup("Unsigned")) == database.lookup("uint32_t"));
  REQUIRE(database.resolveTypedef(database.lookup("LongLong")) == database.lookup("int64_t"));
}

TEST_CASE("comments, directives and attributes are skipped", "[types][parse]") {
  const TypeDatabase database = parseClean(R"(
    #include <stdint.h>
    #define EVAL_CAP 8
    // a line comment
    /* a block
       comment */
    __attribute__((visibility("default"))) int32_t exported(void);
    typedef struct __attribute__((packed)) { uint8_t bytes[EVAL_CAP]; } Buffer;
    extern "C" {
      void wrapped(void);
    }
  )");

  REQUIRE(database.findDeclaration("exported") != nullptr);
  REQUIRE(database.findDeclaration("wrapped") != nullptr);
  REQUIRE(database.sizeOf(database.lookup("Buffer")) == 8);  // #define fed the array length
}

TEST_CASE("flexible array members are modelled without a size", "[types][parse]") {
  const TypeDatabase database = parseClean(R"(
    struct Message { uint32_t len; uint8_t data[]; };
  )");

  const TypeId message = database.lookup("Message", NameSpace::Tag);
  REQUIRE(database.sizeOf(message) == 4);
  const TypeEntry* entry = database.get(message);
  REQUIRE(entry->fields.size() == 2);
  REQUIRE(entry->fields[1].flexible);
  REQUIRE(entry->fields[1].offset == 4);
  // Anything past the header lands in the flexible member, which is the only
  // useful answer for a trailing buffer.
  REQUIRE(database.fieldAt(message, 40).names == std::vector<std::string>{"data"});
}

TEST_CASE("one bad declaration costs one declaration", "[types][parse]") {
  TypeDatabase database;
  Result<ParseReport> report = parseHeader(R"(
    typedef int32_t Good1;
    this is not a declaration at all;
    typedef int32_t Good2;
  )",
                                           database);
  REQUIRE(report.hasValue());
  REQUIRE(report->skipped == 1);
  REQUIRE_FALSE(report->warnings.empty());
  REQUIRE(database.lookup("Good1").valid());
  REQUIRE(database.lookup("Good2").valid());
}

TEST_CASE("an unterminated comment fails the whole file", "[types][parse]") {
  TypeDatabase database;
  const Result<ParseReport> report = parseHeader("/* forever", database);
  REQUIRE_FALSE(report.hasValue());
}

TEST_CASE("the android-ndk preset imports cleanly", "[types][parse]") {
  const Result<std::string> path = resolveHeaderPath("android-ndk");
  REQUIRE(path.hasValue());

  TypeDatabase database;
  Result<ParseReport> report = parseHeaderFile(*path, database);
  REQUIRE(report.hasValue());
  INFO(report->format("android-ndk"));
  REQUIRE(report->skipped == 0);
  REQUIRE(report->accepted > 50);

  REQUIRE(database.format(database.findDeclaration("gettimeofday")->type) ==
          "int32_t (struct timeval* tv, struct timezone* tz)");
  REQUIRE(database.format(database.findDeclaration("memcpy")->type) ==
          "void* (void* dst, void* src, size_t n)");
  REQUIRE(database.sizeOf(database.lookup("timeval", NameSpace::Tag)) == 16);
  REQUIRE(database.format(database.lookup("jobject")) == "jobject");
  REQUIRE(database.format(database.findDeclaration("pthread_once")->type) ==
          "int32_t (pthread_once_t* control, void (*init)(void))");
}
