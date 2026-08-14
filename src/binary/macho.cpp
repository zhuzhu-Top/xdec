// Mach-O 64 loader.
//
// Scope, deliberately: little-endian ARM64/x86_64 MH_EXECUTE/MH_DYLIB images
// carrying LC_DYLD_INFO(_ONLY) rebase/bind opcodes (the pre-chained-fixups
// scheme). That covers iOS/macOS binaries built before Apple's newer
// LC_DYLD_CHAINED_FIXUPS format; the latter is a distinct on-disk encoding
// and is flagged, not silently ignored, when seen.
//
// Two design choices mirror elf.cpp on purpose:
//
//  * The memory map is built from LC_SEGMENT_64, never from section_64
//    records alone -- a section only exists inside a segment, and __LINKEDIT
//    (symbols, string table, dyld info bytes) has no sections at all despite
//    being mapped and readable.
//
//  * Rebase/bind slots are resolved to concrete values wherever that is
//    statically possible. A rebase slot's on-disk bytes already are the
//    correct virtual address at a load bias of zero -- the same convention
//    elf.cpp uses for R_*_RELATIVE and RELR -- so it is read back out of the
//    finalised memory map exactly like an ELF RELR entry. A bind slot names
//    an imported symbol whose value is only known to a real loader; it is
//    left unresolved on purpose, with the symbol name attached so callers can
//    still identify the target.
#include <algorithm>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xdec/binary/image.h"
#include "xdec/support/log.h"

#include "field_reader.h"
#include "macho_format.h"

namespace xdec::binary {

XDEC_DECLARE_LOG_CATEGORY(logBinary)

namespace {

using namespace macho;  // NOLINT(google-build-using-namespace) - constants only

inline constexpr unsigned kPointerSize = 8;

struct Segment {
  std::string name;
  uint64_t vmaddr = 0;
  uint64_t vmsize = 0;
  uint64_t fileoff = 0;
  uint64_t filesize = 0;
  uint32_t initprot = 0;
};

struct SectionRaw {
  std::string sectname;
  std::string segname;
  uint64_t addr = 0;
  uint64_t size = 0;
  uint64_t fileOffset = 0;
  uint32_t flags = 0;
  MemoryPermissions permissions = MemoryPermissions::None;
};

/// Segments dyld write-protects once it has applied their fixups.
///
/// `initprot` on these is read-write, and it describes the window during
/// loading, not the program: dyld rebases and binds the segment's pointers and
/// then `mprotect`s it read-only for good, which is the entire reason the
/// segment exists separately from `__DATA`. Reading initprot literally makes
/// every pointer in it look like a variable the program might assign to, and
/// so makes a jump table's anchor -- absd keeps its dispatch anchors in
/// `__DATA_CONST` -- an unknowable load rather than the constant it is.
[[nodiscard]] bool constAfterFixups(std::string_view segmentName) noexcept {
  return segmentName == "__DATA_CONST" || segmentName == "__AUTH_CONST";
}

MemoryPermissions permissionsFromProt(uint32_t initprot, std::string_view segmentName) {
  MemoryPermissions permissions = MemoryPermissions::None;
  if ((initprot & kVmProtRead) != 0) {
    permissions |= MemoryPermissions::Read;
  }
  if ((initprot & kVmProtWrite) != 0 && !constAfterFixups(segmentName)) {
    permissions |= MemoryPermissions::Write;
  }
  if ((initprot & kVmProtExecute) != 0) {
    permissions |= MemoryPermissions::Execute;
  }
  return permissions;
}

Arch archFromCpuType(uint32_t cpuType) {
  switch (cpuType) {
    case kCpuTypeArm64:
      return Arch::AArch64;
    case kCpuTypeX8664:
      return Arch::X86_64;
    default:
      return Arch::Unknown;
  }
}

/// Reads a ULEB128 value from `stream`, advancing `pos`. Stops at the end of
/// the buffer rather than reading out of bounds on a truncated opcode stream.
uint64_t readUleb(std::span<const std::byte> stream, uint64_t& pos) {
  uint64_t result = 0;
  unsigned shift = 0;
  while (pos < stream.size()) {
    const auto byte = std::to_integer<uint8_t>(stream[pos++]);
    result |= (static_cast<uint64_t>(byte & 0x7f) << shift);
    if ((byte & 0x80) == 0) {
      break;
    }
    shift += 7;
  }
  return result;
}

int64_t readSleb(std::span<const std::byte> stream, uint64_t& pos) {
  int64_t result = 0;
  unsigned shift = 0;
  uint8_t byte = 0;
  do {
    if (pos >= stream.size()) {
      break;
    }
    byte = std::to_integer<uint8_t>(stream[pos++]);
    result |= (static_cast<int64_t>(byte & 0x7f) << shift);
    shift += 7;
  } while ((byte & 0x80) != 0);
  if (shift < 64 && (byte & 0x40) != 0) {
    result |= -(static_cast<int64_t>(1) << shift);
  }
  return result;
}

class MachOLoader {
 public:
  MachOLoader(FileBuffer file, std::string path)
      : file_(std::move(file)), reader_(file_.bytes(), Endian::Little), path_(std::move(path)) {}

  Result<std::unique_ptr<BinaryImage>> load() {
    XDEC_TRY_VOID(parseHeader());
    XDEC_TRY_VOID(parseLoadCommands());
    XDEC_TRY_VOID(buildMemoryMap());
    parseSymbols();
    applyDyldInfo();
    resolveRebases();
    return finish();
  }

 private:
  // -- header -----------------------------------------------------------

  Result<void> parseHeader() {
    if (file_.size() < kHeaderSize64) {
      return err(DiagCode::BadFormat, "file is shorter than a Mach-O 64 header");
    }
    const auto magic = static_cast<uint32_t>(reader_.u32(0));
    if (magic != kMagic64) {
      return err(DiagCode::NotImplemented,
                 std::format("magic 0x{:x} is not a little-endian 64-bit Mach-O image; "
                             "32-bit and byte-swapped Mach-O are not implemented",
                             magic));
    }
    const auto cpuType = static_cast<uint32_t>(reader_.u32(4));
    const auto fileType = static_cast<uint32_t>(reader_.u32(12));
    commandCount_ = static_cast<uint32_t>(reader_.u32(16));
    if (reader_.failed()) {
      return err(DiagCode::BadFormat, "truncated Mach-O header");
    }

    arch_ = archFromCpuType(cpuType);
    if (arch_ == Arch::Unknown) {
      return err(DiagCode::UnsupportedArch, std::format("unsupported Mach-O cputype 0x{:x}", cpuType));
    }
    kind_ = fileType == kFileTypeExecute ? BinaryKind::Executable
           : (fileType == kFileTypeDylib || fileType == kFileTypeBundle) ? BinaryKind::SharedObject
                                                                          : BinaryKind::Unknown;
    return ok();
  }

  // -- load commands ------------------------------------------------------

  Result<void> parseLoadCommands() {
    uint64_t offset = kHeaderSize64;
    for (uint32_t index = 0; index < commandCount_; ++index) {
      if (offset + 8 > file_.size()) {
        return err(DiagCode::BadFormat, std::format("truncated load command {}", index));
      }
      const auto cmd = static_cast<uint32_t>(reader_.u32(offset));
      const auto cmdSize = static_cast<uint32_t>(reader_.u32(offset + 4));
      if (reader_.failed() || cmdSize < 8 || offset + cmdSize > file_.size()) {
        return err(DiagCode::BadFormat,
                   std::format("malformed load command {} at file offset 0x{:x}", index, offset));
      }
      switch (cmd) {
        case kLcSegment64:
          parseSegment(offset);
          break;
        case kLcSymtab:
          parseSymtabCommand(offset);
          break;
        case kLcDyldInfo:
        case kLcDyldInfoOnly:
          parseDyldInfoCommand(offset);
          break;
        case kLcMain:
          parseEntryPoint(offset);
          break;
        case kLcLoadDylib:
          parseLoadDylib(offset);
          break;
        case kLcDyldChainedFixups:
          XDEC_LOG_WARN(logBinary(),
                        "'{}' uses LC_DYLD_CHAINED_FIXUPS, which is not decoded yet; bound "
                        "pointer slots stay unrelocated",
                        path_);
          break;
        default:
          break;
      }
      offset += cmdSize;
    }
    if (!haveEntryPoint_) {
      XDEC_LOG_WARN(logBinary(), "'{}' has no LC_MAIN; entry point left unset", path_);
    }
    return ok();
  }

  std::string readFixedName(uint64_t offset, uint64_t length) {
    const std::span<const std::byte> bytes = file_.bytes();
    if (offset + length > bytes.size()) {
      return {};
    }
    const std::string_view view{reinterpret_cast<const char*>(bytes.data() + offset),
                                static_cast<std::size_t>(length)};
    const auto nul = view.find('\0');
    return std::string{nul == std::string_view::npos ? view : view.substr(0, nul)};
  }

  void parseSegment(uint64_t base) {
    Segment segment;
    segment.name = readFixedName(base + kSegmentNameOffset, kSegmentNameSize);
    segment.vmaddr = reader_.u64(base + kSegmentVmAddr);
    segment.vmsize = reader_.u64(base + kSegmentVmSize);
    segment.fileoff = reader_.u64(base + kSegmentFileOff);
    segment.filesize = reader_.u64(base + kSegmentFileSize);
    segment.initprot = static_cast<uint32_t>(reader_.u32(base + kSegmentInitProt));
    const auto sectionCount = static_cast<uint32_t>(reader_.u32(base + kSegmentNSects));
    if (reader_.failed()) {
      reader_.clearFailure();
      XDEC_LOG_WARN(logBinary(), "'{}': malformed LC_SEGMENT_64 at file offset 0x{:x}", path_, base);
      return;
    }

    const MemoryPermissions permissions =
        permissionsFromProt(segment.initprot, segment.name);
    segments_.push_back(segment);

    uint64_t sectionBase = base + kSegmentHeaderSize;
    for (uint32_t index = 0; index < sectionCount; ++index, sectionBase += kSectionRecordSize) {
      SectionRaw section;
      section.sectname = readFixedName(sectionBase + kSectionNameOffset, kSegmentNameSize);
      section.segname = readFixedName(sectionBase + kSectionSegNameOffset, kSegmentNameSize);
      section.addr = reader_.u64(sectionBase + kSectionAddr);
      section.size = reader_.u64(sectionBase + kSectionSize);
      section.fileOffset = reader_.u32(sectionBase + kSectionFileOffset);
      section.flags = static_cast<uint32_t>(reader_.u32(sectionBase + kSectionFlags));
      if (reader_.failed()) {
        reader_.clearFailure();
        XDEC_LOG_WARN(logBinary(), "'{}': truncated section_64 in segment '{}'", path_, segment.name);
        break;
      }
      section.permissions = permissions;
      sections_.push_back(std::move(section));
    }
  }

  void parseSymtabCommand(uint64_t base) {
    symtabOffset_ = static_cast<uint32_t>(reader_.u32(base + kSymtabSymOff));
    symtabCount_ = static_cast<uint32_t>(reader_.u32(base + kSymtabNSyms));
    strtabOffset_ = static_cast<uint32_t>(reader_.u32(base + kSymtabStrOff));
    strtabSize_ = static_cast<uint32_t>(reader_.u32(base + kSymtabStrSize));
    if (reader_.failed()) {
      reader_.clearFailure();
      return;
    }
    haveSymtab_ = true;
  }

  void parseDyldInfoCommand(uint64_t base) {
    rebaseOffset_ = static_cast<uint32_t>(reader_.u32(base + kDyldInfoRebaseOff));
    rebaseSize_ = static_cast<uint32_t>(reader_.u32(base + kDyldInfoRebaseSize));
    bindOffset_ = static_cast<uint32_t>(reader_.u32(base + kDyldInfoBindOff));
    bindSize_ = static_cast<uint32_t>(reader_.u32(base + kDyldInfoBindSize));
    lazyBindOffset_ = static_cast<uint32_t>(reader_.u32(base + kDyldInfoLazyBindOff));
    lazyBindSize_ = static_cast<uint32_t>(reader_.u32(base + kDyldInfoLazyBindSize));
    reader_.clearFailure();
  }

  void parseEntryPoint(uint64_t base) {
    entryFileOffset_ = reader_.u64(base + kEntryPointOffset);
    if (reader_.failed()) {
      reader_.clearFailure();
      return;
    }
    haveEntryPoint_ = true;
  }

  void parseLoadDylib(uint64_t base) {
    const auto nameOffset = static_cast<uint32_t>(reader_.u32(base + kDylibNameOffset));
    if (reader_.failed()) {
      reader_.clearFailure();
      return;
    }
    const std::string_view name = reader_.cstring(base + nameOffset);
    if (!reader_.failed()) {
      neededLibraries_.emplace_back(name);
    }
    reader_.clearFailure();
  }

  // -- memory map ---------------------------------------------------------

  Result<void> buildMemoryMap() {
    memory_.setBackingBytes(file_.bytes());
    for (const Segment& segment : segments_) {
      // __PAGEZERO is a huge unmapped guard region with no permissions and no
      // content; recording it would only bloat the region list.
      if (segment.name == "__PAGEZERO" || segment.vmsize == 0) {
        continue;
      }
      if (segment.filesize > segment.vmsize) {
        return err(DiagCode::BadFormat,
                   std::format("segment '{}' declares filesize 0x{:x} > vmsize 0x{:x}",
                               segment.name, segment.filesize, segment.vmsize));
      }
      MemoryRegion region;
      region.va = segment.vmaddr;
      region.size = segment.vmsize;
      region.fileOffset = segment.fileoff;
      region.fileSize = segment.filesize;
      region.permissions = permissionsFromProt(segment.initprot, segment.name);
      region.name = segment.name;
      memory_.addRegion(std::move(region));
    }
    XDEC_TRY_VOID(memory_.finalize());
    if (memory_.empty()) {
      return err(DiagCode::BadFormat, "image maps no memory");
    }
    return ok();
  }

  uint64_t fileOffsetToVa(uint64_t fileOffset) const {
    for (const Segment& segment : segments_) {
      if (fileOffset >= segment.fileoff && fileOffset - segment.fileoff < segment.filesize) {
        return segment.vmaddr + (fileOffset - segment.fileoff);
      }
    }
    return 0;
  }

  // -- symbols --------------------------------------------------------------

  void parseSymbols() {
    if (!haveSymtab_ || symtabCount_ == 0) {
      return;
    }
    const std::span<const std::byte> bytes = file_.bytes();
    const uint64_t tableBytes = uint64_t{symtabCount_} * kNlistRecordSize;
    if (symtabOffset_ + tableBytes > bytes.size()) {
      XDEC_LOG_WARN(logBinary(), "'{}': LC_SYMTAB table extends past end of file", path_);
      return;
    }
    FieldReader strings{reader_.slice(strtabOffset_, strtabSize_), Endian::Little};
    reader_.clearFailure();

    symbols_.reserve(symtabCount_);
    for (uint32_t index = 0; index < symtabCount_; ++index) {
      const uint64_t base = symtabOffset_ + uint64_t{index} * kNlistRecordSize;
      const auto strx = static_cast<uint32_t>(reader_.u32(base + kNlistStrx));
      const auto type = static_cast<uint8_t>(reader_.u8(base + kNlistType));
      const auto sect = static_cast<uint8_t>(reader_.u8(base + kNlistSect));
      const uint64_t value = reader_.u64(base + kNlistValue);
      if (reader_.failed()) {
        reader_.clearFailure();
        XDEC_LOG_WARN(logBinary(), "'{}': truncated symbol table entry {}", path_, index);
        break;
      }
      if ((type & kNStab) != 0) {
        continue;  // debugger-only stab symbol, not a real one.
      }

      const uint8_t typeField = type & kNTypeMask;
      Symbol symbol;
      symbol.rawIndex = index;
      symbol.va = value;
      symbol.defined = typeField == kNSect || typeField == kNAbs;
      symbol.binding = (type & kNExt) != 0 ? SymbolBinding::Global : SymbolBinding::Local;
      symbol.exported = symbol.defined && (type & kNExt) != 0;
      if (symbol.defined && typeField == kNSect && sect >= 1 && sect <= sections_.size()) {
        const SectionRaw& section = sections_[sect - 1];
        symbol.kind = hasPermission(section.permissions, MemoryPermissions::Execute)
                         ? SymbolKind::Function
                         : SymbolKind::Object;
      }
      if (strx != 0) {
        const std::string_view name = strings.cstring(strx);
        if (!strings.failed()) {
          symbol.name = std::string{name};
        }
        strings.clearFailure();
      }
      symbols_.push_back(std::move(symbol));
    }
  }

  // -- dyld rebase/bind opcodes ---------------------------------------------

  /// Both opcode streams begin with a 4-bit opcode and 4-bit immediate packed
  /// into one byte; `dispatch` receives that split and the raw byte stream so
  /// the two interpreters below can share the bounds-checked setup.
  template <class Dispatch>
  void runOpcodeStream(uint32_t fileOffset, uint32_t size, Dispatch dispatch) {
    if (size == 0) {
      return;
    }
    const std::span<const std::byte> bytes = file_.bytes();
    if (uint64_t{fileOffset} + size > bytes.size()) {
      XDEC_LOG_WARN(logBinary(), "'{}': dyld opcode stream at 0x{:x} extends past end of file",
                    path_, fileOffset);
      return;
    }
    const std::span<const std::byte> stream = bytes.subspan(fileOffset, size);
    uint64_t pos = 0;
    while (pos < stream.size()) {
      const auto byte = std::to_integer<uint8_t>(stream[pos++]);
      if (!dispatch(stream, pos, byte)) {
        return;
      }
    }
  }

  void applyDyldInfo() {
    runOpcodeStream(rebaseOffset_, rebaseSize_,
                    [this](std::span<const std::byte> stream, uint64_t& pos, uint8_t byte) {
                      return stepRebase(stream, pos, byte);
                    });
    runOpcodeStream(bindOffset_, bindSize_,
                    [this](std::span<const std::byte> stream, uint64_t& pos, uint8_t byte) {
                      return stepBind(stream, pos, byte, /*lazy=*/false);
                    });
    // Reset bind state between streams: a lazy-bind stream is a
    // concatenation of independent per-stub mini-programs, but the addend
    // and symbol name from a stray non-lazy entry must not leak into it.
    bindAddend_ = 0;
    bindSymbol_.clear();
    runOpcodeStream(lazyBindOffset_, lazyBindSize_,
                    [this](std::span<const std::byte> stream, uint64_t& pos, uint8_t byte) {
                      return stepBind(stream, pos, byte, /*lazy=*/true);
                    });
  }

  bool stepRebase(std::span<const std::byte> stream, uint64_t& pos, uint8_t byte) {
    const uint8_t opcode = byte & kRebaseOpcodeMask;
    const uint8_t imm = byte & kRebaseImmediateMask;
    switch (opcode) {
      case kRebaseOpDone:
        return false;
      case kRebaseOpSetTypeImm:
        return true;  // Only REBASE_TYPE_POINTER is expected; nothing to record.
      case kRebaseOpSetSegmentAndOffsetUleb: {
        rebaseSegment_ = imm;
        const uint64_t offset = readUleb(stream, pos);
        rebaseAddress_ = segmentVa(rebaseSegment_, offset);
        return true;
      }
      case kRebaseOpAddAddrUleb:
        rebaseAddress_ += readUleb(stream, pos);
        return true;
      case kRebaseOpAddAddrImmScaled:
        rebaseAddress_ += uint64_t{imm} * kPointerSize;
        return true;
      case kRebaseOpDoRebaseImmTimes:
        for (unsigned count = 0; count < imm; ++count) {
          emitRebase();
          rebaseAddress_ += kPointerSize;
        }
        return true;
      case kRebaseOpDoRebaseUlebTimes: {
        const uint64_t count = readUleb(stream, pos);
        for (uint64_t index = 0; index < count; ++index) {
          emitRebase();
          rebaseAddress_ += kPointerSize;
        }
        return true;
      }
      case kRebaseOpDoRebaseAddAddrUleb:
        emitRebase();
        rebaseAddress_ += kPointerSize + readUleb(stream, pos);
        return true;
      case kRebaseOpDoRebaseUlebTimesSkippingUleb: {
        const uint64_t count = readUleb(stream, pos);
        const uint64_t skip = readUleb(stream, pos);
        for (uint64_t index = 0; index < count; ++index) {
          emitRebase();
          rebaseAddress_ += kPointerSize + skip;
        }
        return true;
      }
      default:
        XDEC_LOG_WARN(logBinary(), "'{}': unknown rebase opcode 0x{:x}", path_, opcode);
        return false;
    }
  }

  bool stepBind(std::span<const std::byte> stream, uint64_t& pos, uint8_t byte, bool lazy) {
    const uint8_t opcode = byte & kBindOpcodeMask;
    const uint8_t imm = byte & kBindImmediateMask;
    switch (opcode) {
      case kBindOpDone:
        // Regular bind: end of stream. Lazy bind: end of one stub's
        // mini-program, with more following; either way just clear the
        // addend and keep scanning, since running off the end of `stream`
        // terminates the loop regardless.
        bindAddend_ = 0;
        return true;
      case kBindOpSetDylibOrdinalImm:
      case kBindOpSetDylibSpecialImm:
        return true;  // Ordinal is not needed to name the symbol.
      case kBindOpSetDylibOrdinalUleb:
        readUleb(stream, pos);
        return true;
      case kBindOpSetSymbolTrailingFlagsImm: {
        const uint64_t start = pos;
        while (pos < stream.size() && stream[pos] != std::byte{0}) {
          ++pos;
        }
        bindSymbol_.assign(reinterpret_cast<const char*>(stream.data() + start), pos - start);
        if (pos < stream.size()) {
          ++pos;  // Skip the NUL terminator.
        }
        return true;
      }
      case kBindOpSetTypeImm:
        return true;  // Only BIND_TYPE_POINTER is expected.
      case kBindOpSetAddendSleb:
        bindAddend_ = readSleb(stream, pos);
        return true;
      case kBindOpSetSegmentAndOffsetUleb: {
        bindSegment_ = imm;
        const uint64_t offset = readUleb(stream, pos);
        bindAddress_ = segmentVa(bindSegment_, offset);
        return true;
      }
      case kBindOpAddAddrUleb:
        bindAddress_ += readUleb(stream, pos);
        return true;
      case kBindOpDoBind:
        emitBind(lazy);
        bindAddress_ += kPointerSize;
        return true;
      case kBindOpDoBindAddAddrUleb:
        emitBind(lazy);
        bindAddress_ += kPointerSize + readUleb(stream, pos);
        return true;
      case kBindOpDoBindAddAddrImmScaled:
        emitBind(lazy);
        bindAddress_ += kPointerSize + uint64_t{imm} * kPointerSize;
        return true;
      case kBindOpDoBindUlebTimesSkippingUleb: {
        const uint64_t count = readUleb(stream, pos);
        const uint64_t skip = readUleb(stream, pos);
        for (uint64_t index = 0; index < count; ++index) {
          emitBind(lazy);
          bindAddress_ += kPointerSize + skip;
        }
        return true;
      }
      default:
        XDEC_LOG_WARN(logBinary(), "'{}': unknown bind opcode 0x{:x}", path_, opcode);
        return false;
    }
  }

  uint64_t segmentVa(uint32_t segmentIndex, uint64_t offset) const {
    if (segmentIndex >= segments_.size()) {
      return 0;
    }
    return segments_[segmentIndex].vmaddr + offset;
  }

  void emitRebase() {
    if (rebaseSegment_ >= segments_.size()) {
      return;
    }
    Relocation relocation;
    relocation.va = rebaseAddress_;
    relocation.kind = RelocKind::Relative;
    relocation.width = kPointerSize;
    relocation.isImplicitAddend = true;
    relocations_.push_back(relocation);
  }

  void emitBind(bool lazy) {
    if (bindSegment_ >= segments_.size() || bindSymbol_.empty()) {
      return;
    }
    Relocation relocation;
    relocation.va = bindAddress_;
    relocation.kind = lazy ? RelocKind::JumpSlot : RelocKind::GotSlot;
    relocation.width = kPointerSize;
    relocation.addend = bindAddend_;
    relocation.symbolIndex = importSymbolIndex(bindSymbol_);
    relocations_.push_back(relocation);
  }

  uint32_t importSymbolIndex(const std::string& name) {
    const auto [it, inserted] =
        importIndex_.try_emplace(name, static_cast<uint32_t>(symbols_.size()));
    if (inserted) {
      Symbol symbol;
      symbol.name = name;
      symbol.binding = SymbolBinding::Global;
      symbol.defined = false;
      symbol.fromDynamic = true;
      symbols_.push_back(std::move(symbol));
    }
    return it->second;
  }

  /// Rebase slots have no explicit addend field: the on-disk bytes already
  /// hold the link-time-relative pointer, exactly as ELF's RELR encoding
  /// works. At a load bias of zero that value is already the final one, so
  /// resolution just reads it back from the finalised memory map.
  void resolveRebases() {
    for (Relocation& relocation : relocations_) {
      if (!relocation.isImplicitAddend) {
        continue;
      }
      std::byte buffer[8] = {};
      if (memory_.read(relocation.va, std::span<std::byte>{buffer, kPointerSize})) {
        uint64_t value = 0;
        for (unsigned index = kPointerSize; index-- > 0;) {
          value = (value << 8) | static_cast<uint64_t>(std::to_integer<uint8_t>(buffer[index]));
        }
        relocation.value = value;
        relocation.hasValue = true;
      }
    }
  }

  // -- assembly -------------------------------------------------------------

  Result<std::unique_ptr<BinaryImage>> finish() {
    ImageContents contents;
    contents.format = BinaryFormat::MachO;
    contents.kind = kind_;
    contents.arch = arch_;
    contents.endian = Endian::Little;
    contents.pointerBits = kPointerSize * 8;
    contents.entryPoint = haveEntryPoint_ ? fileOffsetToVa(entryFileOffset_) : 0;
    contents.hasEntryPoint = haveEntryPoint_ && contents.entryPoint != 0;
    contents.path = path_;
    contents.neededLibraries = std::move(neededLibraries_);
    contents.symbols = std::move(symbols_);
    contents.relocations = std::move(relocations_);

    contents.sections.reserve(sections_.size());
    for (const SectionRaw& raw : sections_) {
      Section section;
      section.name = raw.sectname;
      section.va = raw.addr;
      section.size = raw.size;
      section.fileOffset = raw.fileOffset;
      const bool zeroFilled = (raw.flags & kSectionTypeMask) == kSZerofill;
      section.fileSize = zeroFilled ? 0 : raw.size;
      section.rawType = raw.flags;
      section.allocated = true;
      section.zeroFilled = zeroFilled;
      // Every section_64 record lives inside a __TEXT/__DATA*-style segment;
      // the tables about the program (symtab, strtab, dyld info bytes) live
      // in __LINKEDIT, which declares no sections at all.
      section.programData = true;
      section.permissions = raw.permissions;
      contents.sections.push_back(std::move(section));
    }

    contents.memory = std::move(memory_);
    contents.file = std::move(file_);
    return std::make_unique<BinaryImage>(std::move(contents));
  }

  FileBuffer file_;
  FieldReader reader_;
  std::string path_;

  Arch arch_ = Arch::Unknown;
  BinaryKind kind_ = BinaryKind::Unknown;
  uint32_t commandCount_ = 0;

  uint64_t entryFileOffset_ = 0;
  bool haveEntryPoint_ = false;

  uint32_t symtabOffset_ = 0;
  uint32_t symtabCount_ = 0;
  uint32_t strtabOffset_ = 0;
  uint32_t strtabSize_ = 0;
  bool haveSymtab_ = false;

  uint32_t rebaseOffset_ = 0;
  uint32_t rebaseSize_ = 0;
  uint32_t bindOffset_ = 0;
  uint32_t bindSize_ = 0;
  uint32_t lazyBindOffset_ = 0;
  uint32_t lazyBindSize_ = 0;

  // Rebase opcode interpreter state.
  uint32_t rebaseSegment_ = 0;
  uint64_t rebaseAddress_ = 0;

  // Bind opcode interpreter state (shared by the regular and lazy streams).
  uint32_t bindSegment_ = 0;
  uint64_t bindAddress_ = 0;
  int64_t bindAddend_ = 0;
  std::string bindSymbol_;

  std::vector<Segment> segments_;
  std::vector<SectionRaw> sections_;
  MemoryMap memory_;
  std::vector<Symbol> symbols_;
  std::vector<Relocation> relocations_;
  std::vector<std::string> neededLibraries_;
  std::unordered_map<std::string, uint32_t> importIndex_;
};

}  // namespace

Result<std::unique_ptr<BinaryImage>> loadMachO(FileBuffer file, std::string path) {
  MachOLoader loader{std::move(file), std::move(path)};
  return loader.load();
}

}  // namespace xdec::binary
