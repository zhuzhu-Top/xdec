// Mach-O format-specific metadata: compact unwind function bounds, ObjC
// runtime tables (methods, selrefs, classrefs, ivars), and CFString literals.
//
// Populated lazily from mapped image bytes on demand so opening a large Mach-O
// pays no startup cost when only summary info or a single function is queried.
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "xdec/binary/format_metadata.h"
#include "xdec/binary/image.h"

namespace xdec::binary {

struct MachORange {
  uint64_t va = 0;
  uint64_t size = 0;

  [[nodiscard]] bool empty() const noexcept { return size == 0; }
  [[nodiscard]] bool contains(uint64_t address) const noexcept {
    return size != 0 && address >= va && address - va < size;
  }
};

struct ObjcMethodInfo {
  /// Human-readable Objective-C signature name, e.g. `-[Foo bar:]`.
  std::string displayName;
  /// C-identifier-safe function name, e.g. `objc_Foo_bar_`.
  std::string cName;
  /// Raw Objective-C `@encode` type string, e.g. `v24@0:8@16`.
  std::string typeEncoding;
};

struct ObjcIvarSummary {
  std::string name;
  std::string typeEncoding;
  uint64_t offsetSlotVa = 0;
  int32_t offset = 0;
};

struct ObjcMethodSummary {
  char kind = '-';
  std::string selector;
  std::string typeEncoding;
  uint64_t impVa = 0;
};

struct ObjcClassSummary {
  std::string name;
  std::string superName;
  uint64_t classVa = 0;
  uint32_t instanceSize = 0;
  std::vector<ObjcIvarSummary> ivars;
  std::vector<ObjcMethodSummary> methods;
};

class MachOMetadata final : public FormatMetadata {
 public:
  struct Sections {
    uint64_t imageBase = 0;
    MachORange text;
    MachORange unwindInfo;
    MachORange cfstring;
    MachORange cstring;
    MachORange ustring;
    MachORange objcMethname;
    MachORange objcClassname;
    MachORange objcMethtype;
    MachORange objcSelrefs;
    MachORange objcClassrefs;
    MachORange objcSuperrefs;
    MachORange objcProtorefs;
    MachORange objcIvar;
    MachORange objcClasslist;
    MachORange objcCatlist;
  };

  explicit MachOMetadata(Sections sections) : sections_(sections) {}

  [[nodiscard]] const Sections& sections() const noexcept { return sections_; }

  /// Binary-searches `__TEXT,__unwind_info` for the function interval
  /// `[startVa, endVa)` covering `va` (or starting at `va`).
  [[nodiscard]] std::optional<std::pair<uint64_t, uint64_t>> functionBoundsAt(
      const BinaryImage& image, uint64_t va) const;

  /// Binary-searches `__TEXT,__unwind_info` for the smallest recorded function
  /// start strictly above `va`.
  [[nodiscard]] std::optional<uint64_t> nextUnwindFunctionAfter(const BinaryImage& image,
                                                                uint64_t va) const;

  /// Resolves an ObjC method entry (`__objc_classlist`/`__objc_catlist`) at `va`.
  [[nodiscard]] std::optional<ObjcMethodInfo> objcMethodAt(const BinaryImage& image,
                                                           uint64_t va) const;

  /// Nearest ObjC method whose IMP starts at or before `va` (within `__text`).
  [[nodiscard]] std::optional<std::pair<uint64_t, ObjcMethodInfo>> nearestObjcMethodBefore(
      const BinaryImage& image, uint64_t va) const;

  /// Resolves a data slot (`__objc_selrefs`, `__objc_classrefs`,
  /// `__objc_superrefs`, `__objc_ivar`, `__cfstring`) to a sanitized C symbol
  /// name when `nlist` has no name for `va`.
  [[nodiscard]] std::optional<std::string> resolveDataSymbolAt(const BinaryImage& image,
                                                               uint64_t va) const;

  /// Decodes a `__cfstring` entry at `va` to its string content (when ASCII-printable).
  [[nodiscard]] std::optional<std::string> cfStringAt(const BinaryImage& image,
                                                      uint64_t va) const;

  /// All Objective-C classes (with ivars and instance/class methods) from
  /// `__objc_classlist`. Indexed lazily on first call.
  [[nodiscard]] const std::vector<ObjcClassSummary>& objcClasses(const BinaryImage& image) const;

  /// All indexed Objective-C methods (`impVa -> ObjcMethodInfo`) from
  /// `__objc_classlist` and `__objc_catlist`. Indexed lazily on first call.
  [[nodiscard]] const std::unordered_map<uint64_t, ObjcMethodInfo>& allObjcMethods(
      const BinaryImage& image) const;

 private:
  void ensureObjcIndexed(const BinaryImage& image) const;
  void ensureObjcClassesIndexed(const BinaryImage& image) const;

  Sections sections_;
  mutable std::once_flag objcOnce_;
  mutable std::once_flag objcClassesOnce_;
  mutable std::unordered_map<uint64_t, ObjcMethodInfo> objcMethods_;
  mutable std::vector<uint64_t> sortedObjcMethodVas_;
  mutable std::unordered_map<uint64_t, std::string> objcIvars_;
  mutable std::vector<ObjcClassSummary> objcClasses_;
};

}  // namespace xdec::binary
