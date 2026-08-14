// Format-independent view of a loaded binary.
//
// BinaryImage is concrete and immutable after construction: format-specific
// loaders fill in an ImageContents, the image derives its lookup indices once,
// and from then on every reader can share it across threads without locking.
// That immutability is what makes function-level analysis parallelisable.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xdec/binary/backing_store.h"
#include "xdec/binary/format_metadata.h"
#include "xdec/binary/memory_map.h"
#include "xdec/support/result.h"
#include "xdec/support/target.h"

namespace xdec::binary {

enum class BinaryFormat : uint8_t { Unknown, Elf, Pe, MachO, DyldCache };
[[nodiscard]] std::string_view toString(BinaryFormat format) noexcept;

enum class BinaryKind : uint8_t { Unknown, Executable, SharedObject, Relocatable, Core };
[[nodiscard]] std::string_view toString(BinaryKind kind) noexcept;

struct Section {
  std::string name;
  uint64_t va = 0;
  uint64_t size = 0;
  uint64_t fileOffset = 0;
  uint64_t fileSize = 0;
  MemoryPermissions permissions = MemoryPermissions::None;
  /// Raw format-specific type (ELF sh_type, PE characteristics).
  uint32_t rawType = 0;
  bool allocated = false;
  /// True for SHT_NOBITS style sections: occupies memory, has no file bytes.
  bool zeroFilled = false;
  /// Whether the bytes here are the program's own code or data, as opposed to
  /// metadata the loader and linker read about it — symbol and string tables,
  /// relocations, hash tables, notes. Both kinds are mapped, and in a shared
  /// object both are `allocated`, so neither answers this; the difference matters
  /// because an address in the second kind is not something the program refers to
  /// as a variable, and presenting one as though it were invents a variable that
  /// does not exist.
  bool programData = false;
  unsigned entrySize = 0;

  [[nodiscard]] uint64_t endVa() const noexcept { return va + size; }
  [[nodiscard]] bool contains(uint64_t address) const noexcept {
    return address >= va && address < va + size;
  }
};

enum class SymbolKind : uint8_t {
  Unknown,
  Function,
  Object,
  SectionLabel,
  File,
  Tls,
  IndirectFunction,
};
[[nodiscard]] std::string_view toString(SymbolKind kind) noexcept;

enum class SymbolBinding : uint8_t { Unknown, Local, Global, Weak };
[[nodiscard]] std::string_view toString(SymbolBinding binding) noexcept;

inline constexpr uint32_t kNoSymbol = ~uint32_t{0};

struct Symbol {
  std::string name;
  uint64_t va = 0;
  uint64_t size = 0;
  SymbolKind kind = SymbolKind::Unknown;
  SymbolBinding binding = SymbolBinding::Unknown;
  /// False for imports: the name is referenced but defined elsewhere.
  bool defined = false;
  /// Visible to other objects (global or weak, non-hidden).
  bool exported = false;
  /// Index in the originating symbol table, used to resolve relocations.
  uint32_t rawIndex = kNoSymbol;
  bool fromDynamic = false;
};

/// Normalised relocation classes. Raw per-architecture types are kept in
/// `rawType`; this enum captures only what analysis actually branches on.
enum class RelocKind : uint8_t {
  Unknown,
  None,
  /// Slot value is symbol address plus addend.
  Absolute,
  /// Slot value is the load bias plus addend: an internal pointer.
  Relative,
  /// Global offset table entry for a symbol.
  GotSlot,
  /// Procedure linkage table entry for a symbol.
  JumpSlot,
  /// Slot is filled at load time by calling a resolver function.
  IndirectFunction,
  TlsModule,
  TlsOffset,
  TlsDescriptor,
};
[[nodiscard]] std::string_view toString(RelocKind kind) noexcept;

struct Relocation {
  /// The address being patched.
  uint64_t va = 0;
  uint32_t rawType = 0;
  RelocKind kind = RelocKind::Unknown;
  /// The value the loader writes, when it can be determined statically.
  uint64_t value = 0;
  bool hasValue = false;
  int64_t addend = 0;
  uint32_t symbolIndex = kNoSymbol;
  /// Number of bytes patched at `va`.
  unsigned width = 8;
  /// RELR-style: the addend is the value already stored in the slot rather than
  /// an explicit field, so resolution must read the slot.
  bool isImplicitAddend = false;
};

/// Everything a loader produces. Split from BinaryImage so that loaders build a
/// plain aggregate and the image stays immutable.
struct ImageContents {
  BinaryFormat format = BinaryFormat::Unknown;
  BinaryKind kind = BinaryKind::Unknown;
  Arch arch = Arch::Unknown;
  Endian endian = Endian::Little;
  unsigned pointerBits = 0;
  uint64_t entryPoint = 0;
  bool hasEntryPoint = false;
  std::string path;
  /// Owning file bytes. A single file for ELF/Mach-O; several parts for a
  /// split dyld shared cache. See backing_store.h.
  BackingStore store;
  MemoryMap memory;
  std::vector<Section> sections;
  std::vector<Symbol> symbols;
  std::vector<Relocation> relocations;
  std::vector<std::string> neededLibraries;
  std::string soname;
  /// Format-specific data; null for formats that need none. See
  /// format_metadata.h.
  std::unique_ptr<FormatMetadata> formatMetadata;
};

class BinaryImage {
 public:
  explicit BinaryImage(ImageContents contents);

  BinaryImage(const BinaryImage&) = delete;
  BinaryImage& operator=(const BinaryImage&) = delete;

  [[nodiscard]] BinaryFormat format() const noexcept { return contents_.format; }
  [[nodiscard]] BinaryKind kind() const noexcept { return contents_.kind; }
  [[nodiscard]] Arch arch() const noexcept { return contents_.arch; }
  [[nodiscard]] Endian endian() const noexcept { return contents_.endian; }
  [[nodiscard]] unsigned pointerBits() const noexcept { return contents_.pointerBits; }
  [[nodiscard]] unsigned pointerBytes() const noexcept { return contents_.pointerBits / 8; }
  [[nodiscard]] std::string_view path() const noexcept { return contents_.path; }
  [[nodiscard]] bool hasEntryPoint() const noexcept { return contents_.hasEntryPoint; }
  [[nodiscard]] uint64_t entryPoint() const noexcept { return contents_.entryPoint; }
  [[nodiscard]] std::string_view soname() const noexcept { return contents_.soname; }

  [[nodiscard]] const MemoryMap& memory() const noexcept { return contents_.memory; }
  [[nodiscard]] std::span<const Section> sections() const noexcept { return contents_.sections; }
  [[nodiscard]] std::span<const Symbol> symbols() const noexcept { return contents_.symbols; }
  [[nodiscard]] std::span<const Relocation> relocations() const noexcept {
    return contents_.relocations;
  }
  [[nodiscard]] std::span<const std::string> neededLibraries() const noexcept {
    return contents_.neededLibraries;
  }
  [[nodiscard]] std::size_t fileSize() const noexcept { return contents_.store.totalSize(); }
  [[nodiscard]] const FormatMetadata* formatMetadata() const noexcept {
    return contents_.formatMetadata.get();
  }

  // -- address queries ------------------------------------------------------

  [[nodiscard]] const Section* sectionAt(uint64_t va) const noexcept;
  [[nodiscard]] const Section* sectionNamed(std::string_view name) const noexcept;

  /// Symbol whose start address is exactly `va`. Prefers a defined, sized,
  /// global symbol when several coincide.
  [[nodiscard]] const Symbol* symbolAt(uint64_t va) const noexcept;
  /// Innermost sized symbol whose range covers `va`.
  [[nodiscard]] const Symbol* symbolContaining(uint64_t va) const noexcept;
  [[nodiscard]] const Symbol* symbolNamed(std::string_view name) const noexcept;

  /// Relocation patching exactly `va`.
  [[nodiscard]] const Relocation* relocationAt(uint64_t va) const noexcept;
  /// Any relocation whose patched bytes intersect `[va, va + size)`.
  [[nodiscard]] const Relocation* relocationOverlapping(uint64_t va,
                                                        uint64_t size) const noexcept;

  /// Name of the imported symbol a GOT or PLT slot at `va` binds to, if any.
  /// This is the "relocation truth" that makes an indirect call target
  /// identifiable without guessing.
  [[nodiscard]] std::optional<std::string_view> importNameAt(uint64_t va) const noexcept;

  [[nodiscard]] bool isMapped(uint64_t va) const noexcept { return contents_.memory.isMapped(va); }
  [[nodiscard]] bool isExecutable(uint64_t va) const noexcept;
  [[nodiscard]] bool isWritable(uint64_t va) const noexcept;

  /// Whether `[va, va + size)` reads the same value for the whole life of the
  /// program: every byte mapped, no byte writable, and no relocation patching
  /// any of them. The relocation clause is not redundant with the permission
  /// one — `.data.rel.ro` is read-only *after* the loader has written to it,
  /// and what it writes there depends on which module wins a symbol, which is
  /// not knowable from one file. This is the question a pass must ask before
  /// treating a load as a constant of the program rather than as an
  /// observation of memory.
  [[nodiscard]] bool isImmutable(uint64_t va, uint64_t size) const noexcept;

  // -- reads ---------------------------------------------------------------

  /// Runtime view: file bytes, zero-filled BSS, relocated pointers applied.
  Result<void> read(uint64_t va, std::span<std::byte> out) const;

  /// Little/big-endian unsigned integer of `bytes` width (1, 2, 4 or 8).
  Result<uint64_t> readUnsigned(uint64_t va, unsigned bytes) const;
  Result<uint64_t> readPointer(uint64_t va) const;
  Result<std::string> readCString(uint64_t va, std::size_t maxLength = 4096) const;

  /// Raw bytes for instruction decoding. Fails when the range is not fully
  /// file-backed or crosses a region boundary, which for a code range means the
  /// caller has walked off the end of a section.
  Result<std::span<const std::byte>> codeView(uint64_t va, uint64_t size) const;

  /// Executable regions, in address order. Entry point discovery walks these.
  [[nodiscard]] std::vector<const MemoryRegion*> executableRegions() const;

 private:
  void buildIndices();

  ImageContents contents_;
  /// Indices into `contents_.symbols`, sorted by address then by descending
  /// size, covering only defined symbols.
  std::vector<uint32_t> symbolsByAddress_;
  /// Widest symbol in the image; bounds the backward walk in symbolContaining.
  uint64_t maxSymbolSize_ = 0;
  /// Indices into `contents_.relocations`, sorted by address.
  std::vector<uint32_t> relocationsByAddress_;
  uint64_t lowestRelocationVa_ = 0;
  uint64_t highestRelocationEndVa_ = 0;
  /// Widest relocation; bounds the backward walk when overlaying relocations.
  uint64_t maxRelocationWidth_ = 0;
};

/// Sniffs the format from the file's magic bytes and dispatches to a loader.
Result<std::unique_ptr<BinaryImage>> openBinary(const std::filesystem::path& path);

/// Loads an ELF image from an already-read buffer. Exposed for tests that
/// synthesise images in memory.
Result<std::unique_ptr<BinaryImage>> loadElf(FileBuffer file, std::string path);

/// Loads a little-endian 64-bit Mach-O image from an already-read buffer.
/// Exposed for tests that synthesise images in memory.
Result<std::unique_ptr<BinaryImage>> loadMachO(FileBuffer file, std::string path);

/// Loads a dyld shared cache rooted at `mainPath` (an `arm64`/`arm64e`-suffixed
/// main file such as `dyld_shared_cache_arm64`), discovering and opening its
/// subcache and symbols siblings on disk. Unlike loadElf/loadMachO this reads
/// more than the one buffer already passed to openBinary, because a cache's
/// mapped address space routinely spans several physical files.
Result<std::unique_ptr<BinaryImage>> loadDyldCache(FileBuffer mainFile,
                                                   std::filesystem::path mainPath);

}  // namespace xdec::binary
