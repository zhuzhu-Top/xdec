// TypeBinder: which imported declaration, if any, describes a given address.
//
// A type database on its own names nothing in a binary. `struct timeval` is a
// shape; `int gettimeofday(struct timeval*, struct timezone*)` is a claim about
// a name; and only the image's symbol table connects a name to an address. The
// binder is that connection, and it is deliberately the only place it is made,
// because every rule about when a header may override what the code shows is a
// rule about trust and belongs in one auditable place.
//
// The rules, strongest evidence first:
//
//   1. **A defined symbol that starts exactly at the address.** Its name is the
//      program's own, so a declaration under that name describes this code.
//   2. **An import a relocation resolves.** A GOT slot holding `dlsym` is not
//      `dlsym`, but the thing called through it is, so a prototype found this
//      way applies to the call and not to the slot.
//   3. **Nothing.** An address no symbol names gets no type, ever. The
//      alternative -- matching on layout, or on a plausible-looking name -- is
//      how a decompiler starts telling readers things that are not so.
//
// A symbol that merely *covers* an address is not evidence about that address:
// in a stripped library most code is some offset into one large export, and
// borrowing the enclosing symbol's prototype for an interior function would
// attach a signature to a function that never had one.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

#include "xdec/types/database.h"
#include "xdec/types/type.h"

namespace xdec::types {

/// The name a binary gives an address, as the binder needs it: only exact
/// matches, and only the fact of being code or data.
struct BoundName {
  std::string name;
  bool isFunction = false;

  [[nodiscard]] bool empty() const noexcept { return name.empty(); }
};

/// How the binder asks what lives at an address. A resolver that is not set
/// resolves nothing, which is what binding against an image-less pipeline gets.
using NameAt = std::function<BoundName(uint64_t va)>;

/// What a header turned out to say about an address.
struct Binding {
  /// The name the declaration was found under.
  std::string name;
  TypeId type;
  bool isFunction = false;

  [[nodiscard]] bool valid() const noexcept { return type.valid(); }
};

class TypeBinder {
 public:
  /// Both the database and whatever the resolver captures must outlive the
  /// binder; it stores references, as every analysis here does.
  TypeBinder(const TypeDatabase& database, NameAt names)
      : database_(database), names_(std::move(names)) {}

  /// The binding for an address, or an invalid one when nothing names it or
  /// nothing was declared under that name. Cached, because the emitter asks
  /// about the same callee once per call site.
  [[nodiscard]] const Binding& at(uint64_t va) const;

  /// The binding for a name the caller already has -- an import a relocation
  /// named, say, where there is no address of the function to ask about.
  [[nodiscard]] const Binding& forName(std::string_view name) const;

  /// The Function entry behind a binding at this address, or null when the
  /// address is unbound or bound to data. Typedefs are resolved, so a
  /// `typedef int (Handler)(void)` still answers.
  [[nodiscard]] const TypeEntry* prototypeAt(uint64_t va) const;
  [[nodiscard]] const TypeEntry* prototypeFor(const Binding& binding) const;

  /// The function type a `T (*)(...)` points at, through any typedefs on the
  /// way, or null for every other type. This is what makes a call through a
  /// variable describable: the value is a pointer the code computed, but the
  /// declaration it came from still says what is on the other end of it.
  [[nodiscard]] const TypeEntry* pointeeFunction(TypeId id) const;

  /// The same, for the struct or union a pointer points at, and only when it
  /// has a body: an offset into an incomplete type is an offset into something
  /// nothing has described.
  [[nodiscard]] const TypeEntry* pointeeRecord(TypeId id) const;

  /// What a pointer points at, through typedefs on both sides. Null when the
  /// type is not a pointer at all.
  [[nodiscard]] const TypeEntry* pointeeOf(TypeId id) const;

  /// Whether an imported type can stand in for what inference found, so that a
  /// caller can decide between them without knowing how either is spelled.
  ///
  /// The question is not "are these the same type" -- they never are, that is
  /// the point of importing -- but "would printing the imported one contradict
  /// something the code proved". A pointer where the code proved a pointer is
  /// fine however it is spelled; a 32-bit field where the code loaded eight
  /// bytes is not, and there the binary wins, because the header may simply be
  /// the wrong version.
  ///
  /// `inferredBits` of zero means inference had no opinion, which is the common
  /// case for an argument register nothing narrowed: then the header is the
  /// only evidence there is, and it is accepted.
  [[nodiscard]] bool consistent(TypeId external, uint32_t inferredBits,
                                unsigned inferredPointerDepth) const;

  /// Whether a value of this type lives in registers, so that an emitter may
  /// declare a parameter or a return with it and leave the body alone.
  ///
  /// A struct passed or returned by value does not: the ABI splits it across
  /// registers or hands over a hidden pointer, and the recovered body works in
  /// exactly those terms. Declaring `EvalVec3 f(void)` over a body that returns
  /// a 64-bit register would not be a better-typed decompilation, it would be
  /// one that does not compile. The header's claim is still worth showing --
  /// as a comment, which is what the emitter does with a false answer here.
  [[nodiscard]] bool registerShaped(TypeId id) const;

  [[nodiscard]] const TypeDatabase& database() const noexcept { return database_; }

 private:
  const TypeDatabase& database_;
  NameAt names_;
  mutable std::map<uint64_t, Binding> byAddress_;
  mutable std::map<std::string, Binding, std::less<>> byName_;
};

}  // namespace xdec::types
