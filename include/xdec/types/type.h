// The external type IR: what a C declaration means, independent of any binary.
//
// This is deliberately a separate vocabulary from analysis::CType. CType is
// what the decompiler *inferred* — a width, a pointer depth, a signedness bit,
// all of it recovered from how bytes were used. A TypeId is what a header
// *said*, and headers say things inference cannot: that two 4-byte fields are
// `left` and `right` of an `EvalNode`, that a `size_t` is not merely 64 bits
// wide, that a function takes three arguments and not eight.
//
// Keeping the two vocabularies apart is the point. Merging them would force
// every inference site to reason about struct layout, and would let a wrong
// header silently overwrite a fact the binary proved. Instead the external
// type rides alongside the inferred one (see types::Binding) and the emitter
// decides which to print, with a documented precedence.
//
// Entries live in a TypeDatabase and are referred to by TypeId. Handles rather
// than pointers, for the same reason the IL uses them: the storage can grow
// while a caller holds a reference, and a handle printed in a diagnostic is a
// stable name.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xdec/support/handle.h"

namespace xdec::types {

struct TypeIdTag;
/// An entry in a TypeDatabase. Only meaningful together with the database it
/// came from; there is no global type space.
using TypeId = Handle<TypeIdTag>;

enum class TypeKind : uint8_t {
  /// `void`. Distinct from "unknown": a function returning void is a fact.
  Void,
  Bool,
  /// Integer of `bitWidth` bits, `isSigned` deciding the spelling.
  Int,
  /// IEEE binary32/binary64 by `bitWidth`.
  Float,
  Pointer,
  Array,
  Struct,
  Union,
  Enum,
  /// A name for another type. Kept as its own entry rather than resolved at
  /// parse time, because the whole value of importing a header is that the
  /// output says `size_t` where the header said `size_t`.
  Typedef,
  Function,
};

[[nodiscard]] std::string_view toString(TypeKind kind) noexcept;

/// A member of a struct or union.
///
/// `offset` is in bytes and is always filled in by the database when the type
/// is completed: either from the natural C layout it computes, or from what a
/// caller supplied. Field-offset matching (turning `*(uint32_t*)(p + 0x10)`
/// into `p->color`) is the entire reason struct import is worth doing, and it
/// needs a number, not a declaration order.
struct StructField {
  std::string name;
  TypeId type;
  uint64_t offset = 0;
  /// `uint8_t data[]` — a trailing member with no size. Its offset is the
  /// struct's size; it contributes nothing to that size.
  bool flexible = false;
};

struct EnumConstant {
  std::string name;
  int64_t value = 0;
};

/// A function parameter. The name is optional in C and frequently absent in
/// real headers, in which case the emitter falls back to positional names.
struct FunctionParam {
  std::string name;
  TypeId type;
};

/// Sentinel for an array declared without a length (`int x[]` outside a
/// struct, or a pointer-decayed parameter).
inline constexpr uint64_t kUnknownArrayLength = ~uint64_t{0};

/// One type. A tagged union spelled as a struct with unused members, because
/// the alternative — a variant of eleven alternatives, several of which carry
/// vectors — costs more code at every visitation site than the few unused
/// words cost here, and this table has hundreds of entries, not millions.
struct TypeEntry {
  TypeKind kind = TypeKind::Void;

  /// Struct/union/enum tag, typedef name. Empty for anonymous aggregates and
  /// for every derived type (pointers, arrays, function types), which are
  /// spelled structurally.
  ///
  /// An anonymous aggregate that a typedef names borrows that name here, so
  /// `typedef struct { int x; } Point;` prints as `Point` and not as a
  /// re-expanded body at every use. `hasTag` is what tells the two apart.
  std::string name;

  /// Whether `name` is a tag, and so needs the `struct`/`union`/`enum` keyword
  /// in front of it. False for the borrowed typedef name above.
  bool hasTag = false;

  /// Int/Float/Bool: the width. Zero elsewhere.
  uint32_t bitWidth = 0;
  bool isSigned = false;

  /// Pointer: pointee. Array: element. Typedef: underlying. Enum: the integer
  /// type the constants are stored in.
  TypeId element;

  /// Array only.
  uint64_t arrayLength = kUnknownArrayLength;

  /// Struct/union members, in declaration order.
  std::vector<StructField> fields;
  /// Enum constants, in declaration order.
  std::vector<EnumConstant> constants;

  /// Function only.
  TypeId returnType;
  std::vector<FunctionParam> params;
  bool variadic = false;

  /// Aggregate size and alignment in bytes, computed when the type is
  /// completed. Zero size on a struct that has been declared but not yet
  /// defined — a forward reference, which is legal to point at and illegal to
  /// dereference.
  uint64_t size = 0;
  uint32_t align = 1;
  /// Whether a struct/union/enum has a body yet.
  bool complete = true;

  /// A typedef the platform provides rather than one an imported header
  /// declared: `size_t`, `pid_t`, `off_t`. Kept as entries, because printing
  /// `size_t` where the source said `size_t` is most of what type import is
  /// worth -- but never re-emitted as a definition, since the system header
  /// that owns the name is the one that must define it.
  bool builtin = false;
};

/// A named declaration a header made about the program: a function prototype
/// or an extern variable. This is what makes a type database bindable to a
/// binary at all — types alone name nothing in the image.
struct Declaration {
  std::string name;
  /// Function declarations point at a Function entry; variables at any type.
  TypeId type;
  bool isFunction = false;
};

}  // namespace xdec::types
