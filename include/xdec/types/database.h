// TypeDatabase: the store of imported types and the declarations that name
// them, plus the C spelling rules that turn an entry back into text.
//
// Three things this class owns that callers should not reimplement:
//
//   * Interning. Derived types (`int*`, `char[8]`, `void(*)(int)`) are
//     hash-consed, so pointer-to-the-same-thing is one entry and TypeId
//     equality is type equality for them. Named types are not interned; two
//     structs with the same tag are the same entry by name.
//   * Layout. `size`/`align`/field offsets are computed with the natural C
//     rules for the target when a struct is completed. Getting this wrong
//     means field matching silently points at the wrong member, so it happens
//     in one place with one test.
//   * Spelling. `format` and `declare` implement the C declarator spiral
//     (`int (*f)(void)`, not `int* f()`). Every emitter that prints a type
//     goes through here.
//
// Forward declaration is supported because C headers need it: `struct node`
// can be pointed at before its body is known. `declareStruct` creates an
// incomplete entry, `defineStruct` fills it in, and the TypeId is stable
// across the two.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xdec/support/json.h"
#include "xdec/support/result.h"
#include "xdec/types/type.h"

namespace xdec::types {

/// Which C namespace a name lives in. C keeps struct/union/enum tags apart
/// from ordinary identifiers, and a header that says both `struct stat` and
/// `typedef ... stat` is legal, so the database keeps them apart too.
enum class NameSpace : uint8_t { Ordinary, Tag };

class TypeDatabase {
 public:
  /// Constructs a database preloaded with the C scalar types, so `lookup("int")`
  /// answers before any header is read.
  TypeDatabase();

  // --- Scalars -----------------------------------------------------------

  [[nodiscard]] TypeId voidType() const noexcept { return void_; }
  [[nodiscard]] TypeId boolType() const noexcept { return bool_; }
  [[nodiscard]] TypeId intType(uint32_t bitWidth, bool isSigned);
  [[nodiscard]] TypeId floatType(uint32_t bitWidth);

  // --- Derived types (interned) -----------------------------------------

  [[nodiscard]] TypeId pointerTo(TypeId pointee);

  /// The pointer-to-`pointee` type, if some earlier import already interned
  /// it (a header declaring a parameter or field of that pointer type is
  /// enough). Read-only: never creates an entry, so a caller holding only a
  /// `const TypeDatabase&` — evidence, not authorship, see types/binder.h —
  /// can still ask "does this pointer type already exist" without needing
  /// the mutable access `pointerTo` requires. Unset when nothing ever
  /// declared exactly this pointer.
  [[nodiscard]] std::optional<TypeId> findPointerTo(TypeId pointee) const;
  [[nodiscard]] TypeId arrayOf(TypeId element, uint64_t length);
  [[nodiscard]] TypeId functionType(TypeId returnType, std::vector<FunctionParam> params,
                                    bool variadic);

  // --- Named types -------------------------------------------------------

  /// Returns the existing tag entry or creates an incomplete one. Calling this
  /// twice with the same tag yields the same TypeId, which is what makes
  /// `struct node { struct node *next; };` expressible.
  [[nodiscard]] TypeId declareTag(TypeKind kind, std::string_view tag);

  /// Fills in an aggregate's body and computes its layout. Field offsets left
  /// at zero are assigned by natural alignment; a caller that already knows
  /// the offsets sets `explicitOffsets` and they are kept verbatim.
  [[nodiscard]] Result<void> defineAggregate(TypeId id, std::vector<StructField> fields,
                                             bool explicitOffsets = false);

  [[nodiscard]] Result<void> defineEnum(TypeId id, std::vector<EnumConstant> constants,
                                        TypeId base);

  /// Creates an anonymous aggregate, for `typedef struct { ... } Name;`. The
  /// typedef supplies the name a reader will see; the tag namespace stays
  /// empty, as it is in the source.
  [[nodiscard]] TypeId createAnonymousAggregate(TypeKind kind);

  /// Registers `name` as an alias for `underlying`. Redefining a typedef to
  /// the same type is accepted silently (headers do it); to a different one is
  /// an error, because that is a real conflict a reader must resolve.
  [[nodiscard]] Result<TypeId> addTypedef(std::string_view name, TypeId underlying);

  // --- Lookup ------------------------------------------------------------

  [[nodiscard]] TypeId lookup(std::string_view name,
                              NameSpace space = NameSpace::Ordinary) const;

  [[nodiscard]] const TypeEntry* get(TypeId id) const noexcept { return entries_.tryGet(id); }
  [[nodiscard]] std::size_t typeCount() const noexcept { return entries_.size(); }

  /// Strips typedefs (and nothing else) down to the underlying entry. Enums
  /// stay enums: `EvalKind` and `int` print differently and that difference is
  /// the reason for importing.
  [[nodiscard]] TypeId resolveTypedef(TypeId id) const;

  /// Size in bytes, or unset for `void`, an incomplete aggregate, a function
  /// type, or an array of unknown length.
  [[nodiscard]] std::optional<uint64_t> sizeOf(TypeId id) const;
  [[nodiscard]] std::optional<uint32_t> alignOf(TypeId id) const;

  /// The field of an aggregate covering byte `offset`, and the offset within
  /// that field. Descends into nested aggregates, so a hit reports the full
  /// access path (`{"header", "len"}`). Empty path when nothing matches.
  struct FieldPath {
    std::vector<std::string> names;
    TypeId type;
    uint64_t remainder = 0;
    [[nodiscard]] bool found() const noexcept { return !names.empty(); }
  };
  [[nodiscard]] FieldPath fieldAt(TypeId aggregate, uint64_t offset) const;

  // --- Declarations ------------------------------------------------------

  [[nodiscard]] Result<void> declareFunction(std::string_view name, TypeId functionType);
  [[nodiscard]] Result<void> declareGlobal(std::string_view name, TypeId type);
  [[nodiscard]] const Declaration* findDeclaration(std::string_view name) const;
  [[nodiscard]] const std::map<std::string, Declaration>& declarations() const noexcept {
    return declarations_;
  }

  // --- Spelling ----------------------------------------------------------

  /// The C spelling of the type on its own (`struct node *`, `int (*)(int)`).
  [[nodiscard]] std::string format(TypeId id) const;

  /// The C spelling of a declaration of `name` with this type
  /// (`int (*handler)(int)`, `char buf[8]`). This is the one to use when
  /// emitting anything with an identifier, because C's declarator syntax does
  /// not let the type be written to the left of every name.
  [[nodiscard]] std::string declare(TypeId id, std::string_view name) const;

  /// Full definitions of every named aggregate and enum, in dependency order,
  /// as C text. Emitted into a decompiled file's preamble so the output
  /// compiles against nothing but itself.
  [[nodiscard]] std::string formatDefinitions() const;

  /// The same, restricted to what `roots` reach. This is the form an emitter
  /// wants: a preset header declares thousands of types and one decompiled
  /// function mentions four, so defining all of them would bury the code under
  /// its own preamble. Reachability follows pointers as well as by-value
  /// members, so a struct only ever pointed at still gets its forward
  /// declaration and the output still compiles.
  [[nodiscard]] std::string formatDefinitions(std::span<const TypeId> roots) const;

  // --- Serialisation -----------------------------------------------------

  [[nodiscard]] json::Value toJson() const;
  [[nodiscard]] static Result<TypeDatabase> fromJson(const json::Value& value);

  /// Absorbs every type and declaration of `other`. Used to stack a preset
  /// under a user header. Conflicting typedefs are reported, not merged.
  [[nodiscard]] Result<void> merge(const TypeDatabase& other);

 private:
  /// Tag for the empty-database constructor. Deserialisation must not preload
  /// the builtins, or every TypeId in the document would be off by the number
  /// of them.
  struct EmptyTag {};
  explicit TypeDatabase(EmptyTag) noexcept {}

  [[nodiscard]] TypeId intern(TypeEntry entry);
  [[nodiscard]] static std::string keyOf(const TypeEntry& entry);
  /// Every entry `roots` refers to, transitively and through pointers.
  [[nodiscard]] std::vector<TypeId> reachableFrom(std::span<const TypeId> roots) const;
  void computeLayout(TypeEntry& entry, bool explicitOffsets) const;
  void formatInto(TypeId id, std::string& prefix, std::string& suffix) const;
  void collectDefinitionOrder(TypeId id, std::vector<TypeId>& order,
                              std::vector<uint8_t>& state) const;
  [[nodiscard]] std::string spellTagOrName(const TypeEntry& entry) const;

  HandleVector<TypeId, TypeEntry> entries_;
  /// Structural key -> entry, for interned derived types and scalars.
  std::unordered_map<std::string, TypeId> interned_;
  std::map<std::string, TypeId> ordinary_;
  std::map<std::string, TypeId> tags_;
  std::map<std::string, Declaration> declarations_;
  TypeId void_;
  TypeId bool_;
};

}  // namespace xdec::types
