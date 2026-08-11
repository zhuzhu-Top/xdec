// Recovering a readable literal from a constant address the image can prove
// never changes -- `"ro.arch"` in place of `0x20f98` for a call argument that
// is, in fact, a pointer into `.rodata`.
//
// This is deliberately not an IL rewrite (compare passes/const_fold_memory.h,
// which folds an immutable *load* into the constant it always reads): nothing
// here dereferences a pointer the code passes along without ever reading
// itself, and doing that folding at the IL level would require inventing an
// IL notion of string literal only the emitter would ever consume. Recovery
// therefore happens where it is used -- once, on demand, from the emit
// layer's AddressRenderer (see emit/address_render.h) -- against the exact
// same immutability question const_fold_memory asks of a load address.
//
// Safety rules, all required, mirroring const_fold_memory's own:
//   1. Every byte read, `MemoryFacts::isImmutable` for that one byte: mapped,
//      never writable, not patched by the loader. A guess through a writable
//      or relocated byte is not a fact of the program, so recovery gives up
//      right there rather than reading past it speculatively.
//   2. Printable ASCII only (0x20..0x7e) up to the terminating NUL. A
//      non-ASCII byte says this is not a C string this decompiler should be
//      guessing the encoding of, so recovery fails rather than emitting
//      something unreadable or wrong.
//   3. Bounded length (kMaxLength). No terminator within the bound fails
//      closed rather than reading the rest of the section.
// Failing any of these returns nothing, and the caller's existing fallback
// (the bare address) is exactly as it was before this existed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "xdec/support/reader.h"

namespace xdec::analysis {

enum class ImageLiteralKind : uint8_t {
  CString,  // future: WString, Utf8Blob -- add a kind and a tryRecover, the
            // caller (AddressRenderer) does not need to change.
};

struct ImageLiteral {
  ImageLiteralKind kind = ImageLiteralKind::CString;
  /// Decoded characters, not yet C-escaped (see quoteCString).
  std::string text;
};

class ImageLiteralRecovery {
 public:
  ImageLiteralRecovery(ByteReader reader, MemoryFacts facts)
      : reader_(std::move(reader)), facts_(std::move(facts)) {}

  /// The literal at `va`, or nothing when `va` is not a provably-immutable,
  /// printable, NUL-terminated run of bytes -- including when this recovery
  /// was built with no reader at all (a pipeline with no image), which is
  /// the default and changes nothing for a caller that never had one.
  [[nodiscard]] std::optional<ImageLiteral> at(uint64_t va) const;

  static constexpr std::size_t kMaxLength = 4096;

 private:
  ByteReader reader_;
  MemoryFacts facts_;
};

/// `text` wrapped in double quotes with C escapes for the characters that
/// need them. `text` is already known printable ASCII (see `at`'s rule 2),
/// so this only ever escapes `"`, `\`, and never has to decide what to do
/// with a control byte.
[[nodiscard]] std::string quoteCString(std::string_view text);

}  // namespace xdec::analysis
