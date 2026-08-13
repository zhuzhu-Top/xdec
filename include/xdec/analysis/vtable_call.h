// Recognising an indirect call through a vtable slot: `call load(obj +
// slotOffset)`, confirmed once the same `obj` reads more than one distinct
// slot somewhere in the function -- the one signal available without a type
// system that a pointer is being used as an object rather than as a bare
// function pointer.
//
// Deliberately stops at the object and the offset. Naming the offset a
// "vtable slot" and the object's type a struct is exactly the next step
// (see the plan's own note: "输出改进签名，不依赖特定类名" -- an improved
// signature, not a class name), left to whatever eventually owns struct
// layout synthesis; this is the general, class-name-free precondition that
// step would need.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "xdec/il/function.h"

namespace xdec::analysis {

struct VtableCallSite {
  il::OpId call;
  /// The pointer the slot was read through, exactly as written -- since the
  /// expression pool is hash-consed, two call sites reading through "the
  /// same" object compare equal by this ExprId alone.
  il::ExprId object{};
  uint64_t slotOffset = 0;
};

/// Matches `call`'s own target against `load(obj + slotOffset)` (through any
/// widening/narrowing around either the load or the address, and `obj` alone
/// when the address has no offset). Nullopt for a direct call, a call
/// through a value that is not a load, or a load whose address is not of
/// this base-plus-constant shape.
[[nodiscard]] std::optional<VtableCallSite> matchVtableCallTarget(const il::Function& function,
                                                                   il::OpId call);

/// Every `matchVtableCallTarget` hit in `function` whose `object` is also
/// seen, at a different offset, at another call site -- the bar the header
/// comment describes for calling this a vtable rather than one ordinary
/// function-pointer read. A function with only single-slot objects returns
/// nothing: seeing one slot on a pointer says nothing about whether it is a
/// vtable at all.
[[nodiscard]] std::vector<VtableCallSite> findConfirmedVtableCalls(const il::Function& function);

}  // namespace xdec::analysis
