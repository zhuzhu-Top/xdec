// AddressRenderer: the one place a StackSlot or Global address (see
// analysis::StackFrame::classify) becomes C text, whatever context it is
// used in.
//
// Before this, "print this address" was three separate paths that happened
// to agree on StackSlot (CContext::stackSlotLvalue, consulted by both
// memoryLvalue and, through it, the folded-load substitution in
// ExprPrinter::value) and disagreed on everything else: a stack slot
// handed to a call as a pointer fell straight to ExprPrinter's raw
// arithmetic (`__entry_sp - 0x70`) because pointerOperand and printCall
// never asked StackFrame::classify at all, and a Global's own immutable
// contents were never consulted anywhere outside a Load/Store. This is the
// single dispatch every one of those contexts (see AddressRole) now goes
// through instead, so a fix here reaches all of them and a new one -- see
// docs/15-address-form.md -- has exactly one place to be added.
//
// Deliberately narrow: recognises only the two AddressKinds
// StackFrame::classify already names, and only when something is actually
// recovered there (a local, a name, a literal). An Other address, or a
// StackSlot/Global with nothing recovered, returns nothing -- the caller's
// own pre-existing fallback (a raw cast dereference or cast pointer)
// applies exactly as if this had not been consulted, which is what keeps
// every one of these call sites' behaviour unchanged wherever nothing new
// was recovered.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "xdec/il/function.h"

namespace xdec::emit {

class CContext;

enum class AddressRole : uint8_t {
  /// `*(T*)addr`: a Load/Store's own target, or the fallback dereference a
  /// folded load's substitution re-renders. `width` is the bits accessed.
  MemoryLvalue,
  /// `(T*)addr`, or nothing at all when addr already denotes a suitable
  /// pointer: a call argument, an ACLE atomic's address operand -- any use
  /// that takes the address itself rather than dereferencing it. `width` is
  /// the pointee's bits, for the cast a caller applies when this returns
  /// nothing.
  PointerValue,
  /// `&local`: an explicit request for one recovered local's own address,
  /// never a width-bit view through it (see CContext::addressOfLocal, the
  /// one caller of this role). `width` is unused.
  AddressOf,
};

/// An address rendered as C text, alongside the one fact `ExprPrinter::Text`
/// and its callers already track for every other expression: whether the
/// text already denotes a pointer (or, for AddressOf, an address) with
/// nothing left to cast.
struct AddressRenderResult {
  std::string text;
  bool pointer = false;
};

class AddressRenderer {
 public:
  explicit AddressRenderer(CContext& context) : ctx_(context) {}

  [[nodiscard]] std::optional<AddressRenderResult> render(il::ExprId expr, AddressRole role,
                                                          uint32_t width) const;

 private:
  [[nodiscard]] std::optional<AddressRenderResult> renderStackSlot(int64_t delta, AddressRole role,
                                                                   uint32_t width) const;
  /// Never attempts literal recovery: a Global's immutable contents are only
  /// worth guessing at where the caller already knows the position wants a
  /// pointer to begin with (see StmtPrinter::printCall's own, narrower use
  /// of analysis::ImageLiteralRecovery, gated on the callee's declared
  /// parameter type). Blindly literal-ifying every PointerValue Global here
  /// would also turn a plain integer constant that happens to double as a
  /// readable rodata address into a surprising string, with nothing in this
  /// call's own signature to say that was ever intended.
  [[nodiscard]] std::optional<AddressRenderResult> renderGlobal(uint64_t va, AddressRole role,
                                                                uint32_t width) const;

  CContext& ctx_;
};

}  // namespace xdec::emit
