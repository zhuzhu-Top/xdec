#include <catch2/catch_test_macros.hpp>

#include "xdec/types/database.h"

using namespace xdec;
using namespace xdec::types;

TEST_CASE("scalar types are preloaded and interned", "[types]") {
  TypeDatabase database;

  REQUIRE(database.lookup("int").valid());
  REQUIRE(database.lookup("uint32_t") == database.lookup("unsigned int"));
  REQUIRE(database.lookup("int64_t") == database.lookup("long"));
  REQUIRE_FALSE(database.lookup("EvalVec3").valid());

  // `size_t` is a typedef and not a spelling of `unsigned long`: the two are
  // the same 64 bits here and not the same thing to a reader, so the entry
  // keeps the name and resolves to the width only when asked to.
  REQUIRE(database.lookup("size_t") != database.lookup("unsigned long"));
  REQUIRE(database.resolveTypedef(database.lookup("size_t")) ==
          database.lookup("unsigned long"));
  REQUIRE(database.format(database.lookup("size_t")) == "size_t");
  REQUIRE(database.sizeOf(database.lookup("size_t")) == 8);

  REQUIRE(database.format(database.lookup("int")) == "int32_t");
  REQUIRE(database.format(database.voidType()) == "void");
  REQUIRE(database.sizeOf(database.lookup("uint16_t")) == 2);
  REQUIRE_FALSE(database.sizeOf(database.voidType()).has_value());
}

TEST_CASE("derived types are hash-consed", "[types]") {
  TypeDatabase database;
  const TypeId intType = database.lookup("int");

  REQUIRE(database.pointerTo(intType) == database.pointerTo(intType));
  REQUIRE(database.pointerTo(intType) != database.pointerTo(database.lookup("unsigned int")));
  REQUIRE(database.arrayOf(intType, 4) == database.arrayOf(intType, 4));
  REQUIRE(database.arrayOf(intType, 4) != database.arrayOf(intType, 5));

  REQUIRE(database.sizeOf(database.pointerTo(intType)) == 8);
  REQUIRE(database.sizeOf(database.arrayOf(intType, 4)) == 16);
}

TEST_CASE("struct layout follows natural C alignment", "[types]") {
  TypeDatabase database;
  const TypeId id = database.declareTag(TypeKind::Struct, "Mixed");
  REQUIRE_FALSE(database.sizeOf(id).has_value());  // declared, not defined

  std::vector<StructField> fields;
  fields.push_back(StructField{"flag", database.lookup("uint8_t"), 0, false});
  fields.push_back(StructField{"count", database.lookup("uint32_t"), 0, false});
  fields.push_back(StructField{"next", database.pointerTo(id), 0, false});
  REQUIRE(database.defineAggregate(id, std::move(fields)).hasValue());

  const TypeEntry* entry = database.get(id);
  REQUIRE(entry != nullptr);
  REQUIRE(entry->fields[0].offset == 0);
  REQUIRE(entry->fields[1].offset == 4);
  REQUIRE(entry->fields[2].offset == 8);
  REQUIRE(database.sizeOf(id) == 16);
  REQUIRE(database.alignOf(id) == 8);
}

TEST_CASE("field lookup by byte offset descends into nested aggregates", "[types]") {
  TypeDatabase database;
  const TypeId inner = database.declareTag(TypeKind::Struct, "Header");
  std::vector<StructField> innerFields;
  innerFields.push_back(StructField{"magic", database.lookup("uint32_t"), 0, false});
  innerFields.push_back(StructField{"len", database.lookup("uint32_t"), 0, false});
  REQUIRE(database.defineAggregate(inner, std::move(innerFields)).hasValue());

  const TypeId outer = database.declareTag(TypeKind::Struct, "Packet");
  std::vector<StructField> outerFields;
  outerFields.push_back(StructField{"header", inner, 0, false});
  outerFields.push_back(StructField{"payload", database.pointerTo(database.voidType()), 0,
                                    false});
  REQUIRE(database.defineAggregate(outer, std::move(outerFields)).hasValue());

  const TypeDatabase::FieldPath atFour = database.fieldAt(outer, 4);
  REQUIRE(atFour.found());
  REQUIRE(atFour.names == std::vector<std::string>{"header", "len"});
  REQUIRE(atFour.remainder == 0);

  const TypeDatabase::FieldPath atEight = database.fieldAt(outer, 8);
  REQUIRE(atEight.names == std::vector<std::string>{"payload"});

  const TypeDatabase::FieldPath past = database.fieldAt(outer, 64);
  REQUIRE_FALSE(past.found());
}

TEST_CASE("C declarator spelling round-trips through the spiral rule", "[types]") {
  TypeDatabase database;
  const TypeId intType = database.lookup("int");
  const TypeId charPtr = database.pointerTo(database.lookup("char"));

  REQUIRE(database.declare(database.pointerTo(intType), "p") == "int32_t* p");
  REQUIRE(database.declare(database.arrayOf(intType, 4), "a") == "int32_t a[4]");

  // array of 4 pointers versus pointer to array of 4
  REQUIRE(database.declare(database.arrayOf(database.pointerTo(intType), 4), "a") ==
          "int32_t* a[4]");
  REQUIRE(database.declare(database.pointerTo(database.arrayOf(intType, 4)), "a") ==
          "int32_t (*a)[4]");

  std::vector<FunctionParam> params;
  params.push_back(FunctionParam{"", intType});
  params.push_back(FunctionParam{"", charPtr});
  const TypeId fn = database.functionType(intType, params, false);
  REQUIRE(database.declare(database.pointerTo(fn), "handler") ==
          "int32_t (*handler)(int32_t, int8_t*)");
  REQUIRE(database.format(database.pointerTo(fn)) == "int32_t (*)(int32_t, int8_t*)");
}

TEST_CASE("typedef of an anonymous aggregate borrows the name", "[types]") {
  TypeDatabase database;
  const TypeId body = database.createAnonymousAggregate(TypeKind::Struct);
  std::vector<StructField> fields;
  fields.push_back(StructField{"x", database.lookup("int32_t"), 0, false});
  REQUIRE(database.defineAggregate(body, std::move(fields)).hasValue());

  const Result<TypeId> alias = database.addTypedef("Point", body);
  REQUIRE(alias.hasValue());
  REQUIRE(*alias == body);  // no separate typedef entry; the body wears the name
  REQUIRE(database.lookup("Point") == body);
  REQUIRE(database.format(body) == "Point");
}

TEST_CASE("conflicting typedefs are reported, agreeing ones are not", "[types]") {
  TypeDatabase database;
  REQUIRE(database.addTypedef("Handle", database.lookup("uint32_t")).hasValue());
  REQUIRE(database.addTypedef("Handle", database.lookup("uint32_t")).hasValue());
  REQUIRE_FALSE(database.addTypedef("Handle", database.lookup("uint64_t")).hasValue());
}

TEST_CASE("a database survives a JSON round trip", "[types]") {
  TypeDatabase database;
  const TypeId node = database.declareTag(TypeKind::Struct, "Node");
  std::vector<StructField> fields;
  fields.push_back(StructField{"value", database.lookup("int32_t"), 0, false});
  fields.push_back(StructField{"next", database.pointerTo(node), 0, false});
  REQUIRE(database.defineAggregate(node, std::move(fields)).hasValue());

  const TypeId kind = database.declareTag(TypeKind::Enum, "Kind");
  REQUIRE(database
              .defineEnum(kind, {EnumConstant{"ADD", 0}, EnumConstant{"SUB", 1}},
                          TypeId::invalid())
              .hasValue());

  std::vector<FunctionParam> params;
  params.push_back(FunctionParam{"n", database.pointerTo(node)});
  const TypeId fn = database.functionType(database.lookup("int32_t"), params, false);
  REQUIRE(database.declareFunction("walk", fn).hasValue());
  REQUIRE(database.declareGlobal("g_root", database.pointerTo(node)).hasValue());

  const std::string text = database.toJson().dump();
  const Result<json::Value> reparsed = json::parse(text);
  REQUIRE(reparsed.hasValue());
  Result<TypeDatabase> restored = TypeDatabase::fromJson(*reparsed);
  REQUIRE(restored.hasValue());

  REQUIRE(restored->lookup("Node", NameSpace::Tag).valid());
  REQUIRE(restored->sizeOf(restored->lookup("Node", NameSpace::Tag)) == 16);
  const Declaration* walk = restored->findDeclaration("walk");
  REQUIRE(walk != nullptr);
  REQUIRE(walk->isFunction);
  REQUIRE(restored->format(walk->type) == "int32_t (struct Node* n)");
  const Declaration* root = restored->findDeclaration("g_root");
  REQUIRE(root != nullptr);
  REQUIRE(restored->format(root->type) == "struct Node*");
}

TEST_CASE("merge imports another database's types and declarations", "[types]") {
  TypeDatabase source;
  const TypeId vec = source.createAnonymousAggregate(TypeKind::Struct);
  std::vector<StructField> fields;
  fields.push_back(StructField{"x", source.lookup("int32_t"), 0, false});
  fields.push_back(StructField{"y", source.lookup("int32_t"), 0, false});
  REQUIRE(source.defineAggregate(vec, std::move(fields)).hasValue());
  REQUIRE(source.addTypedef("Vec2", vec).hasValue());
  std::vector<FunctionParam> params;
  params.push_back(FunctionParam{"v", source.pointerTo(vec)});
  REQUIRE(source.declareFunction("length", source.functionType(source.lookup("int32_t"),
                                                               params, false))
              .hasValue());

  TypeDatabase target;
  REQUIRE(target.merge(source).hasValue());
  REQUIRE(target.lookup("Vec2").valid());
  REQUIRE(target.sizeOf(target.lookup("Vec2")) == 8);
  const Declaration* length = target.findDeclaration("length");
  REQUIRE(length != nullptr);
  REQUIRE(target.format(length->type) == "int32_t (Vec2* v)");
}
