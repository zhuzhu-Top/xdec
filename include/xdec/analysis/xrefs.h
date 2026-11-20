// Fast, format-independent string scanning and AArch64 cross-reference
// recovery over raw mapped code/data spans.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xdec/support/reader.h"

namespace xdec::analysis {

struct StringSectionSpan {
  uint64_t va = 0;
  std::string name;
  std::span<const std::byte> bytes;
};

struct ImageStringHit {
  uint64_t va = 0;
  std::string section;
  std::string text;
};

struct StringScanOptions {
  std::size_t minLength = 4;
  std::size_t maxLength = 512;
  /// Case-insensitive substring filter; empty matches all strings.
  std::string filter;
  std::size_t limit = 200;  // 0 = unlimited
};

/// Scans `sections` for NUL-terminated printable ASCII strings matching `options`.
[[nodiscard]] std::vector<ImageStringHit> scanCStringSections(
    std::span<const StringSectionSpan> sections, const StringScanOptions& options);

struct CodeSpan {
  uint64_t va = 0;
  std::span<const std::byte> bytes;
};

enum class XrefKind : uint8_t {
  Call,       // bl <target> (direct or via resolved branch-island / PLT stub)
  Branch,     // b <target>
  Address,    // adrp + add
  DataRead,   // adrp + ldr / ldrsw
  DataWrite,  // adrp + str
};

[[nodiscard]] std::string_view toString(XrefKind kind) noexcept;

struct XrefHit {
  uint64_t insnVa = 0;
  uint64_t targetVa = 0;
  uint64_t viaStubVa = 0;  // non-zero when reached through a PLT/branch-island/objc stub
  XrefKind kind = XrefKind::Call;
};

struct XrefScanOptions {
  /// When true, resolves `bl`/`b` targets through `pltGotSlot` + `objcStubSelectorSlot`
  /// (using `reader`) so calls through multi-hop branch islands or `__objc_stubs`
  /// match target GOT slots, selref slots, or import slots too.
  bool followStubs = true;
  unsigned adrpWindow = 16;
  std::size_t limit = 500;  // 0 = unlimited
  unsigned threads = 0;     // 0 = auto
};

/// Scans AArch64 instruction bytes in `code` for direct calls/branches and
/// ADRP+ADD / ADRP+LDR / ADRP+STR references to any address in `targets`.
[[nodiscard]] std::vector<XrefHit> scanAarch64Xrefs(
    std::span<const CodeSpan> code, std::span<const uint64_t> targets,
    const ByteReader& reader = {}, const XrefScanOptions& options = {});

}  // namespace xdec::analysis
