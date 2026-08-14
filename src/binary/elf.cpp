// ELF loader.
//
// Two decisions here are load-bearing for correctness downstream:
//
//  * The memory map is built from PT_LOAD segments, never from section headers.
//    A NOBITS section's sh_offset is meaningless -- in both reference samples
//    `.relro_padding` and `.data` report the same sh_offset -- so a
//    section-driven map produces either garbage or nothing for `.bss`. Segments
//    give p_filesz/p_memsz, which makes the zero-initialised tail explicit.
//
//  * Relocations are resolved to concrete values wherever that is statically
//    possible, and left explicitly unresolved otherwise. A pointer slot that
//    reads as its unrelocated placeholder is indistinguishable from a real
//    zero, and every jump table reached through such a slot would look empty.
#include <algorithm>
#include <format>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xdec/binary/image.h"
#include "xdec/support/bits.h"
#include "xdec/support/log.h"

#include "elf_format.h"
#include "field_reader.h"

namespace xdec::binary {

XDEC_DECLARE_LOG_CATEGORY(logBinary)

namespace {

using namespace elf;  // NOLINT(google-build-using-namespace) - constants only

struct ProgramHeader {
  uint32_t type = 0;
  uint32_t flags = 0;
  uint64_t offset = 0;
  uint64_t vaddr = 0;
  uint64_t fileSize = 0;
  uint64_t memorySize = 0;
  uint64_t align = 0;
};

struct SectionHeader {
  std::string name;
  uint32_t nameOffset = 0;
  uint32_t type = 0;
  uint64_t flags = 0;
  uint64_t addr = 0;
  uint64_t offset = 0;
  uint64_t size = 0;
  uint32_t link = 0;
  uint32_t info = 0;
  uint64_t entrySize = 0;
};

struct DynamicEntry {
  int64_t tag = 0;
  uint64_t value = 0;
};

/// How a relocation type behaves, independent of architecture.
struct RelocSemantics {
  RelocKind kind = RelocKind::Unknown;
  /// The slot value derives from a symbol address.
  bool usesSymbol = false;
  /// The slot value is the load bias plus the addend, i.e. an internal pointer.
  bool biasRelative = false;
  /// Bytes patched; 0 means "pointer sized".
  unsigned width = 0;
};

RelocSemantics classifyAArch64(uint32_t type) {
  switch (type) {
    case kAArch64None:
      return {RelocKind::None, false, false, 0};
    case kAArch64Abs64:
      return {RelocKind::Absolute, true, false, 8};
    case kAArch64Abs32:
      return {RelocKind::Absolute, true, false, 4};
    case kAArch64Abs16:
      return {RelocKind::Absolute, true, false, 2};
    case kAArch64Prel64:
      return {RelocKind::Absolute, true, false, 8};
    case kAArch64Prel32:
      return {RelocKind::Absolute, true, false, 4};
    case kAArch64GlobDat:
      return {RelocKind::GotSlot, true, false, 8};
    case kAArch64JumpSlot:
      return {RelocKind::JumpSlot, true, false, 8};
    case kAArch64Relative:
      return {RelocKind::Relative, false, true, 8};
    case kAArch64IRelative:
      return {RelocKind::IndirectFunction, false, true, 8};
    case kAArch64TlsDtpMod:
      return {RelocKind::TlsModule, true, false, 8};
    case kAArch64TlsDtpRel:
    case kAArch64TlsTpRel:
      return {RelocKind::TlsOffset, true, false, 8};
    case kAArch64TlsDesc:
      return {RelocKind::TlsDescriptor, true, false, 16};
    default:
      return {RelocKind::Unknown, false, false, 0};
  }
}

RelocSemantics classifyX8664(uint32_t type) {
  switch (type) {
    case kX8664None:
      return {RelocKind::None, false, false, 0};
    case kX866464:
      return {RelocKind::Absolute, true, false, 8};
    case kX866432:
      return {RelocKind::Absolute, true, false, 4};
    case kX8664GlobDat:
      return {RelocKind::GotSlot, true, false, 8};
    case kX8664JumpSlot:
      return {RelocKind::JumpSlot, true, false, 8};
    case kX8664Relative:
      return {RelocKind::Relative, false, true, 8};
    case kX8664IRelative:
      return {RelocKind::IndirectFunction, false, true, 8};
    case kX8664DtpMod64:
      return {RelocKind::TlsModule, true, false, 8};
    case kX8664DtpOff64:
    case kX8664TpOff64:
      return {RelocKind::TlsOffset, true, false, 8};
    default:
      return {RelocKind::Unknown, false, false, 0};
  }
}

RelocSemantics classifyArm32(uint32_t type) {
  switch (type) {
    case kArmNone:
      return {RelocKind::None, false, false, 0};
    case kArmAbs32:
      return {RelocKind::Absolute, true, false, 4};
    case kArmGlobDat:
      return {RelocKind::GotSlot, true, false, 4};
    case kArmJumpSlot:
      return {RelocKind::JumpSlot, true, false, 4};
    case kArmRelative:
      return {RelocKind::Relative, false, true, 4};
    case kArmIRelative:
      return {RelocKind::IndirectFunction, false, true, 4};
    case kArmTlsDtpMod32:
      return {RelocKind::TlsModule, true, false, 4};
    case kArmTlsDtpOff32:
    case kArmTlsTpOff32:
      return {RelocKind::TlsOffset, true, false, 4};
    default:
      return {RelocKind::Unknown, false, false, 0};
  }
}

RelocSemantics classifyRelocation(Arch arch, uint32_t type) {
  switch (arch) {
    case Arch::AArch64:
      return classifyAArch64(type);
    case Arch::X86_64:
      return classifyX8664(type);
    case Arch::Arm32:
    case Arch::Thumb:
      return classifyArm32(type);
    default:
      return {RelocKind::Unknown, false, false, 0};
  }
}

MemoryPermissions permissionsFromSegmentFlags(uint32_t flags) {
  MemoryPermissions permissions = MemoryPermissions::None;
  if ((flags & kPfRead) != 0) {
    permissions |= MemoryPermissions::Read;
  }
  if ((flags & kPfWrite) != 0) {
    permissions |= MemoryPermissions::Write;
  }
  if ((flags & kPfExecute) != 0) {
    permissions |= MemoryPermissions::Execute;
  }
  return permissions;
}

MemoryPermissions permissionsFromSectionFlags(uint64_t flags) {
  MemoryPermissions permissions = MemoryPermissions::Read;
  if ((flags & kShfWrite) != 0) {
    permissions |= MemoryPermissions::Write;
  }
  if ((flags & kShfExecInstr) != 0) {
    permissions |= MemoryPermissions::Execute;
  }
  return permissions;
}

SymbolKind symbolKindFromType(uint8_t type) {
  switch (type) {
    case kSttFunc:
      return SymbolKind::Function;
    case kSttObject:
      return SymbolKind::Object;
    case kSttSection:
      return SymbolKind::SectionLabel;
    case kSttFile:
      return SymbolKind::File;
    case kSttTls:
      return SymbolKind::Tls;
    case kSttGnuIfunc:
      return SymbolKind::IndirectFunction;
    default:
      return SymbolKind::Unknown;
  }
}

SymbolBinding symbolBindingFromBind(uint8_t bind) {
  switch (bind) {
    case kStbLocal:
      return SymbolBinding::Local;
    case kStbGlobal:
      return SymbolBinding::Global;
    case kStbWeak:
      return SymbolBinding::Weak;
    default:
      return SymbolBinding::Unknown;
  }
}

class ElfLoader {
 public:
  ElfLoader(FileBuffer file, std::string path)
      : file_(std::move(file)),
        reader_(file_.bytes(), Endian::Little),
        path_(std::move(path)) {}

  Result<std::unique_ptr<BinaryImage>> load() {
    XDEC_TRY_VOID(parseIdentAndHeader());
    XDEC_TRY_VOID(parseProgramHeaders());
    XDEC_TRY_VOID(parseSectionHeaders());
    XDEC_TRY_VOID(buildMemoryMap());
    parseDynamic();
    XDEC_TRY_VOID(parseSymbols());
    parseRelocations();
    resolveRelocations();
    return finish();
  }

 private:
  // -- header ---------------------------------------------------------------

  Result<void> parseIdentAndHeader() {
    if (file_.size() < kIdentSize) {
      return err(DiagCode::BadFormat, "file is shorter than an ELF identifier");
    }
    FieldReader ident{file_.bytes(), Endian::Little};
    if (ident.u8(0) != 0x7F || ident.u8(1) != 'E' || ident.u8(2) != 'L' || ident.u8(3) != 'F') {
      return err(DiagCode::BadFormat, "missing ELF magic");
    }
    const auto classByte = static_cast<uint8_t>(ident.u8(kIdentClass));
    const auto dataByte = static_cast<uint8_t>(ident.u8(kIdentData));

    if (classByte == kClass64) {
      is64Bit_ = true;
    } else if (classByte == kClass32) {
      is64Bit_ = false;
    } else {
      return err(DiagCode::BadFormat, std::format("unknown ELF class {}", classByte));
    }

    if (dataByte == kData2Lsb) {
      endian_ = Endian::Little;
    } else if (dataByte == kData2Msb) {
      endian_ = Endian::Big;
    } else {
      return err(DiagCode::BadFormat, std::format("unknown ELF data encoding {}", dataByte));
    }

    reader_ = FieldReader{file_.bytes(), endian_};
    layout_ = is64Bit_ ? &kHeader64 : &kHeader32;
    addressSize_ = layout_->addressSize;

    const auto type = static_cast<uint16_t>(reader_.u16(layout_->type));
    const auto machine = static_cast<uint16_t>(reader_.u16(layout_->machine));
    entryPoint_ = reader_.read(layout_->entry, addressSize_);
    programHeaderOffset_ = reader_.read(layout_->phoff, addressSize_);
    sectionHeaderOffset_ = reader_.read(layout_->shoff, addressSize_);
    programHeaderEntrySize_ = static_cast<unsigned>(reader_.u16(layout_->phentsize));
    programHeaderCount_ = static_cast<unsigned>(reader_.u16(layout_->phnum));
    sectionHeaderEntrySize_ = static_cast<unsigned>(reader_.u16(layout_->shentsize));
    sectionHeaderCount_ = static_cast<unsigned>(reader_.u16(layout_->shnum));
    sectionNameTableIndex_ = static_cast<unsigned>(reader_.u16(layout_->shstrndx));

    if (reader_.failed()) {
      return err(DiagCode::BadFormat, "truncated ELF header");
    }

    switch (type) {
      case kEtExec:
        kind_ = BinaryKind::Executable;
        break;
      case kEtDyn:
        kind_ = BinaryKind::SharedObject;
        break;
      case kEtRel:
        kind_ = BinaryKind::Relocatable;
        break;
      case kEtCore:
        kind_ = BinaryKind::Core;
        break;
      default:
        kind_ = BinaryKind::Unknown;
        break;
    }

    arch_ = archFromMachine(machine);
    if (arch_ == Arch::Unknown) {
      return err(DiagCode::UnsupportedArch, std::format("unsupported e_machine {}", machine));
    }
    return ok();
  }

  [[nodiscard]] Arch archFromMachine(uint16_t machine) const {
    switch (machine) {
      case kEmAArch64:
        return Arch::AArch64;
      case kEmArm:
        return Arch::Arm32;
      case kEm386:
        return Arch::X86;
      case kEmX8664:
        return Arch::X86_64;
      case kEmRiscV:
        return is64Bit_ ? Arch::RiscV64 : Arch::RiscV32;
      case kEmMips:
        return is64Bit_ ? Arch::Mips64 : Arch::Mips32;
      case kEmPpc64:
        return Arch::PowerPc64;
      case kEmLoongArch:
        return Arch::LoongArch64;
      default:
        return Arch::Unknown;
    }
  }

  // -- program headers ------------------------------------------------------

  Result<void> parseProgramHeaders() {
    if (programHeaderCount_ == 0) {
      return ok();
    }
    const auto& layout = is64Bit_ ? kProgram64 : kProgram32;
    if (programHeaderEntrySize_ < layout.size) {
      return err(DiagCode::BadFormat,
                 std::format("program header entry size {} is smaller than the {} byte layout",
                             programHeaderEntrySize_, layout.size));
    }

    programHeaders_.reserve(programHeaderCount_);
    for (unsigned index = 0; index < programHeaderCount_; ++index) {
      const uint64_t base = programHeaderOffset_ + uint64_t{index} * programHeaderEntrySize_;
      ProgramHeader header;
      header.type = static_cast<uint32_t>(reader_.u32(base + layout.type));
      header.flags = static_cast<uint32_t>(reader_.u32(base + layout.flags));
      header.offset = reader_.read(base + layout.offset, addressSize_);
      header.vaddr = reader_.read(base + layout.vaddr, addressSize_);
      header.fileSize = reader_.read(base + layout.filesz, addressSize_);
      header.memorySize = reader_.read(base + layout.memsz, addressSize_);
      header.align = reader_.read(base + layout.align, addressSize_);
      if (reader_.failed()) {
        return err(DiagCode::BadFormat, std::format("truncated program header {}", index));
      }
      programHeaders_.push_back(header);
    }
    return ok();
  }

  // -- section headers ------------------------------------------------------

  Result<void> parseSectionHeaders() {
    if (sectionHeaderCount_ == 0 || sectionHeaderOffset_ == 0) {
      XDEC_LOG_INFO(logBinary(), "'{}' has no section headers; using dynamic segment only",
                    path_);
      return ok();
    }
    const auto& layout = is64Bit_ ? kSection64 : kSection32;
    if (sectionHeaderEntrySize_ < layout.size_) {
      return err(DiagCode::BadFormat,
                 std::format("section header entry size {} is smaller than the {} byte layout",
                             sectionHeaderEntrySize_, layout.size_));
    }

    sectionHeaders_.reserve(sectionHeaderCount_);
    for (unsigned index = 0; index < sectionHeaderCount_; ++index) {
      const uint64_t base = sectionHeaderOffset_ + uint64_t{index} * sectionHeaderEntrySize_;
      SectionHeader header;
      header.nameOffset = static_cast<uint32_t>(reader_.u32(base + layout.name));
      header.type = static_cast<uint32_t>(reader_.u32(base + layout.type));
      header.flags = reader_.read(base + layout.flags, layout.flagsSize);
      header.addr = reader_.read(base + layout.addr, addressSize_);
      header.offset = reader_.read(base + layout.offset, addressSize_);
      header.size = reader_.read(base + layout.size, addressSize_);
      header.link = static_cast<uint32_t>(reader_.u32(base + layout.link));
      header.info = static_cast<uint32_t>(reader_.u32(base + layout.info));
      header.entrySize = reader_.read(base + layout.entsize, addressSize_);
      if (reader_.failed()) {
        return err(DiagCode::BadFormat, std::format("truncated section header {}", index));
      }
      sectionHeaders_.push_back(std::move(header));
    }

    // Resolve names from the section name string table.
    if (sectionNameTableIndex_ < sectionHeaders_.size()) {
      const SectionHeader& nameTable = sectionHeaders_[sectionNameTableIndex_];
      FieldReader names{reader_.slice(nameTable.offset, nameTable.size), endian_};
      if (!reader_.failed()) {
        for (SectionHeader& header : sectionHeaders_) {
          const std::string_view name = names.cstring(header.nameOffset);
          if (names.failed()) {
            names.clearFailure();
            continue;
          }
          header.name = std::string{name};
        }
      }
      reader_.clearFailure();
    }
    return ok();
  }

  // -- memory map -----------------------------------------------------------

  Result<void> buildMemoryMap() {
    memory_.setBackingBytes(file_.bytes());

    bool haveLoadSegments = false;
    for (const ProgramHeader& header : programHeaders_) {
      if (header.type != kPtLoad || header.memorySize == 0) {
        continue;
      }
      haveLoadSegments = true;
      if (header.fileSize > header.memorySize) {
        // The file's own two views of the segment disagree. Clamping one to the
        // other would pick a winner silently, so reject instead.
        return err(DiagCode::BadFormat,
                   std::format("PT_LOAD at 0x{:x} declares p_filesz 0x{:x} > p_memsz 0x{:x}",
                               header.vaddr, header.fileSize, header.memorySize));
      }
      MemoryRegion region;
      region.va = header.vaddr;
      region.size = header.memorySize;
      region.fileOffset = header.offset;
      region.fileSize = header.fileSize;
      region.permissions = permissionsFromSegmentFlags(header.flags);
      region.name = std::format("LOAD@0x{:x}", header.vaddr);
      memory_.addRegion(std::move(region));
    }

    if (!haveLoadSegments) {
      // Relocatable objects have no segments. Fall back to allocated sections,
      // which is only meaningful when they carry distinct addresses.
      XDEC_TRY_VOID(addRegionsFromSections());
    } else {
      // A well-formed image covers every allocated section with a segment. If
      // one is left out, expose it anyway rather than silently failing reads,
      // but say so: it means the file's two views disagree.
      warnAboutUncoveredSections();
    }

    XDEC_TRY_VOID(memory_.finalize());
    if (memory_.empty()) {
      return err(DiagCode::BadFormat, "image maps no memory");
    }
    return ok();
  }

  Result<void> addRegionsFromSections() {
    unsigned added = 0;
    for (const SectionHeader& header : sectionHeaders_) {
      if ((header.flags & kShfAlloc) == 0 || header.size == 0 || header.addr == 0) {
        continue;
      }
      MemoryRegion region;
      region.va = header.addr;
      region.size = header.size;
      region.fileOffset = header.offset;
      region.fileSize = header.type == kShtNoBits ? 0 : header.size;
      region.permissions = permissionsFromSectionFlags(header.flags);
      region.name = header.name;
      memory_.addRegion(std::move(region));
      ++added;
    }
    if (added == 0) {
      return err(DiagCode::NotImplemented,
                 "image has neither PT_LOAD segments nor addressed allocated sections");
    }
    return ok();
  }

  void warnAboutUncoveredSections() {
    for (const SectionHeader& header : sectionHeaders_) {
      if ((header.flags & kShfAlloc) == 0 || header.size == 0) {
        continue;
      }
      bool covered = false;
      for (const ProgramHeader& segment : programHeaders_) {
        if (segment.type != kPtLoad) {
          continue;
        }
        if (header.addr >= segment.vaddr &&
            header.addr + header.size <= segment.vaddr + segment.memorySize) {
          covered = true;
          break;
        }
      }
      if (!covered) {
        XDEC_LOG_WARN(logBinary(),
                      "section '{}' at 0x{:x} size 0x{:x} is not covered by any PT_LOAD segment",
                      header.name, header.addr, header.size);
      }
    }
  }

  /// Translates a virtual address to a file offset using PT_LOAD segments.
  /// Needed for DT_* pointers, which are addresses rather than offsets.
  [[nodiscard]] bool virtualToFileOffset(uint64_t va, uint64_t& outOffset) const {
    for (const ProgramHeader& header : programHeaders_) {
      if (header.type != kPtLoad) {
        continue;
      }
      if (va >= header.vaddr && va - header.vaddr < header.fileSize) {
        outOffset = header.offset + (va - header.vaddr);
        return true;
      }
    }
    // Without segments, fall back to sections.
    for (const SectionHeader& header : sectionHeaders_) {
      if ((header.flags & kShfAlloc) == 0 || header.type == kShtNoBits || header.size == 0) {
        continue;
      }
      if (va >= header.addr && va - header.addr < header.size) {
        outOffset = header.offset + (va - header.addr);
        return true;
      }
    }
    return false;
  }

  // -- dynamic section ------------------------------------------------------

  void parseDynamic() {
    uint64_t dynamicOffset = 0;
    uint64_t dynamicSize = 0;

    for (const ProgramHeader& header : programHeaders_) {
      if (header.type == kPtDynamic) {
        dynamicOffset = header.offset;
        dynamicSize = header.fileSize;
        break;
      }
    }
    if (dynamicSize == 0) {
      for (const SectionHeader& header : sectionHeaders_) {
        if (header.type == kShtDynamic) {
          dynamicOffset = header.offset;
          dynamicSize = header.size;
          break;
        }
      }
    }
    if (dynamicSize == 0) {
      return;
    }

    const unsigned entrySize = addressSize_ * 2;
    for (uint64_t offset = 0; offset + entrySize <= dynamicSize; offset += entrySize) {
      DynamicEntry entry;
      entry.tag = reader_.signedRead(dynamicOffset + offset, addressSize_);
      entry.value = reader_.read(dynamicOffset + offset + addressSize_, addressSize_);
      if (reader_.failed()) {
        reader_.clearFailure();
        XDEC_LOG_WARN(logBinary(), "truncated dynamic section in '{}'", path_);
        break;
      }
      if (entry.tag == kDtNull) {
        break;
      }
      dynamic_.push_back(entry);
    }

    // Resolve the dynamic string table so that DT_NEEDED/DT_SONAME can be read.
    uint64_t stringTableVa = 0;
    uint64_t stringTableSize = 0;
    for (const DynamicEntry& entry : dynamic_) {
      if (entry.tag == kDtStrtab) {
        stringTableVa = entry.value;
      } else if (entry.tag == kDtStrSz) {
        stringTableSize = entry.value;
      }
    }
    uint64_t stringTableOffset = 0;
    if (stringTableVa != 0 && virtualToFileOffset(stringTableVa, stringTableOffset)) {
      dynamicStringTable_ = FieldReader{reader_.slice(stringTableOffset, stringTableSize), endian_};
      reader_.clearFailure();
      haveDynamicStringTable_ = true;
    }

    if (haveDynamicStringTable_) {
      for (const DynamicEntry& entry : dynamic_) {
        if (entry.tag != kDtNeeded && entry.tag != kDtSoname) {
          continue;
        }
        const std::string_view name = dynamicStringTable_.cstring(entry.value);
        if (dynamicStringTable_.failed()) {
          dynamicStringTable_.clearFailure();
          continue;
        }
        if (entry.tag == kDtNeeded) {
          neededLibraries_.emplace_back(name);
        } else {
          soname_ = std::string{name};
        }
      }
    }
  }

  [[nodiscard]] uint64_t dynamicValue(int64_t tag, uint64_t fallback = 0) const {
    for (const DynamicEntry& entry : dynamic_) {
      if (entry.tag == tag) {
        return entry.value;
      }
    }
    return fallback;
  }

  [[nodiscard]] bool hasDynamicTag(int64_t tag) const {
    return std::any_of(dynamic_.begin(), dynamic_.end(),
                       [tag](const DynamicEntry& entry) { return entry.tag == tag; });
  }

  // -- symbols --------------------------------------------------------------

  Result<void> parseSymbols() {
    // Section headers give both the symbol table and its exact size, so prefer
    // them; the dynamic path has to infer the count from a hash table.
    for (uint32_t index = 0; index < sectionHeaders_.size(); ++index) {
      const SectionHeader& header = sectionHeaders_[index];
      if (header.type != kShtSymtab && header.type != kShtDynsym) {
        continue;
      }
      if (header.link >= sectionHeaders_.size()) {
        XDEC_LOG_WARN(logBinary(), "symbol table '{}' has invalid sh_link {}", header.name,
                      header.link);
        continue;
      }
      const SectionHeader& strings = sectionHeaders_[header.link];
      FieldReader stringReader{reader_.slice(strings.offset, strings.size), endian_};
      reader_.clearFailure();

      const unsigned entrySize =
          header.entrySize != 0 ? static_cast<unsigned>(header.entrySize)
                                : (is64Bit_ ? kSymbol64.entrySize : kSymbol32.entrySize);
      const uint64_t count = entrySize != 0 ? header.size / entrySize : 0;
      const uint32_t base = static_cast<uint32_t>(symbols_.size());
      symbolTableBase_[index] = base;
      if (header.type == kShtDynsym) {
        dynamicSymbolBase_ = base;
        haveDynamicSymbols_ = true;
      }
      parseSymbolRange(header.offset, count, entrySize, stringReader,
                       header.type == kShtDynsym);
    }

    if (!symbols_.empty()) {
      return ok();
    }

    // Fully stripped section headers: recover the dynamic symbol table.
    const uint64_t symbolTableVa = dynamicValue(kDtSymtab);
    if (symbolTableVa == 0) {
      return ok();
    }
    uint64_t symbolTableOffset = 0;
    if (!virtualToFileOffset(symbolTableVa, symbolTableOffset)) {
      XDEC_LOG_WARN(logBinary(), "DT_SYMTAB 0x{:x} is not in any mapped file range",
                    symbolTableVa);
      return ok();
    }
    const unsigned entrySize = static_cast<unsigned>(
        dynamicValue(kDtSymEnt, is64Bit_ ? kSymbol64.entrySize : kSymbol32.entrySize));
    const uint64_t count = dynamicSymbolCount();
    if (count == 0) {
      XDEC_LOG_WARN(logBinary(), "cannot determine dynamic symbol count for '{}'", path_);
      return ok();
    }
    dynamicSymbolBase_ = 0;
    haveDynamicSymbols_ = true;
    parseSymbolRange(symbolTableOffset, count, entrySize, dynamicStringTable_, true);
    return ok();
  }

  void parseSymbolRange(uint64_t tableOffset, uint64_t count, unsigned entrySize,
                        FieldReader& strings, bool fromDynamic) {
    const auto& layout = is64Bit_ ? kSymbol64 : kSymbol32;
    symbols_.reserve(symbols_.size() + static_cast<std::size_t>(count));

    for (uint64_t index = 0; index < count; ++index) {
      const uint64_t base = tableOffset + index * entrySize;
      const auto nameOffset = static_cast<uint32_t>(reader_.u32(base + layout.name));
      const auto info = static_cast<uint8_t>(reader_.u8(base + layout.info));
      const auto other = static_cast<uint8_t>(reader_.u8(base + layout.other));
      const auto sectionIndex = static_cast<uint16_t>(reader_.u16(base + layout.shndx));
      const uint64_t value = reader_.read(base + layout.value, layout.addressSize);
      const uint64_t size = reader_.read(base + layout.size, layout.addressSize);
      if (reader_.failed()) {
        reader_.clearFailure();
        XDEC_LOG_WARN(logBinary(), "truncated symbol table entry {}", index);
        break;
      }

      Symbol symbol;
      symbol.rawIndex = static_cast<uint32_t>(index);
      symbol.fromDynamic = fromDynamic;
      symbol.kind = symbolKindFromType(static_cast<uint8_t>(info & 0x0F));
      symbol.binding = symbolBindingFromBind(static_cast<uint8_t>(info >> 4));
      symbol.va = value;
      symbol.size = size;
      symbol.defined = sectionIndex != kShnUndef;

      const uint8_t visibility = other & 0x03;
      symbol.exported = symbol.defined &&
                        (symbol.binding == SymbolBinding::Global ||
                         symbol.binding == SymbolBinding::Weak) &&
                        (visibility == kStvDefault || visibility == kStvProtected);

      if (nameOffset != 0) {
        const std::string_view name = strings.cstring(nameOffset);
        if (strings.failed()) {
          strings.clearFailure();
        } else {
          symbol.name = std::string{name};
        }
      }
      symbols_.push_back(std::move(symbol));
    }
  }

  /// Derives the dynamic symbol count from DT_GNU_HASH or DT_HASH. Only needed
  /// when section headers are absent, since neither DT tag records it directly.
  [[nodiscard]] uint64_t dynamicSymbolCount() {
    if (const uint64_t hashVa = dynamicValue(kDtHash); hashVa != 0) {
      uint64_t offset = 0;
      if (virtualToFileOffset(hashVa, offset)) {
        // DT_HASH's second word is nchain, which equals the symbol count.
        const uint64_t chainCount = reader_.u32(offset + 4);
        if (!reader_.failed() && chainCount != 0) {
          return chainCount;
        }
        reader_.clearFailure();
      }
    }

    const uint64_t gnuHashVa = dynamicValue(kDtGnuHash);
    if (gnuHashVa == 0) {
      return 0;
    }
    uint64_t offset = 0;
    if (!virtualToFileOffset(gnuHashVa, offset)) {
      return 0;
    }
    const uint64_t bucketCount = reader_.u32(offset + 0);
    const uint64_t symbolOffset = reader_.u32(offset + 4);
    const uint64_t bloomSize = reader_.u32(offset + 8);
    if (reader_.failed() || bucketCount == 0) {
      reader_.clearFailure();
      return 0;
    }

    const uint64_t bloomBytes = bloomSize * (is64Bit_ ? 8u : 4u);
    const uint64_t bucketsOffset = offset + 16 + bloomBytes;
    uint64_t highestSymbol = 0;
    for (uint64_t bucket = 0; bucket < bucketCount; ++bucket) {
      const uint64_t value = reader_.u32(bucketsOffset + bucket * 4);
      if (reader_.failed()) {
        reader_.clearFailure();
        return 0;
      }
      highestSymbol = std::max(highestSymbol, value);
    }
    if (highestSymbol < symbolOffset) {
      return symbolOffset;
    }

    // Walk the chain of the bucket containing the highest symbol until the
    // terminator bit is set; that entry is the last dynamic symbol.
    const uint64_t chainsOffset = bucketsOffset + bucketCount * 4;
    uint64_t chainIndex = highestSymbol - symbolOffset;
    while (true) {
      const uint64_t value = reader_.u32(chainsOffset + chainIndex * 4);
      if (reader_.failed()) {
        reader_.clearFailure();
        return 0;
      }
      ++chainIndex;
      if ((value & 1) != 0) {
        break;
      }
    }
    return symbolOffset + chainIndex;
  }

  // -- relocations ----------------------------------------------------------

  void parseRelocations() {
    bool parsedFromSections = false;
    for (const SectionHeader& header : sectionHeaders_) {
      switch (header.type) {
        case kShtRela:
          parseRelaTable(header.offset, header.size, symbolBaseForSection(header.link), true);
          parsedFromSections = true;
          break;
        case kShtRel:
          parseRelaTable(header.offset, header.size, symbolBaseForSection(header.link), false);
          parsedFromSections = true;
          break;
        case kShtRelr:
        case kShtAndroidRelr:
          parseRelrTable(header.offset, header.size);
          parsedFromSections = true;
          break;
        case kShtAndroidRela:
          XDEC_LOG_WARN(logBinary(),
                        "section '{}' uses Android packed relocations, which are not decoded yet; "
                        "pointer slots it covers will read as unrelocated",
                        header.name);
          parsedFromSections = true;
          break;
        default:
          break;
      }
    }
    if (parsedFromSections) {
      return;
    }

    // No relocation sections: drive it from the dynamic segment instead.
    const uint32_t base = haveDynamicSymbols_ ? dynamicSymbolBase_ : 0;
    if (const uint64_t va = dynamicValue(kDtRela); va != 0) {
      parseRelaTableAtVa(va, dynamicValue(kDtRelaSz), base, true);
    }
    if (const uint64_t va = dynamicValue(kDtRel); va != 0) {
      parseRelaTableAtVa(va, dynamicValue(kDtRelSz), base, false);
    }
    if (const uint64_t va = dynamicValue(kDtJmpRel); va != 0) {
      // DT_PLTREL says whether the PLT table uses REL or RELA records.
      const bool isRela = dynamicValue(kDtPltRel, static_cast<uint64_t>(kDtRela)) ==
                          static_cast<uint64_t>(kDtRela);
      parseRelaTableAtVa(va, dynamicValue(kDtPltRelSz), base, isRela);
    }
    if (const uint64_t va = dynamicValue(kDtRelr); va != 0) {
      parseRelrTableAtVa(va, dynamicValue(kDtRelrSz));
    }
    if (hasDynamicTag(kDtAndroidRela) || hasDynamicTag(kDtAndroidRel)) {
      XDEC_LOG_WARN(logBinary(),
                    "'{}' uses Android packed relocations, which are not decoded yet", path_);
    }
  }

  [[nodiscard]] uint32_t symbolBaseForSection(uint32_t sectionIndex) const {
    const auto it = symbolTableBase_.find(sectionIndex);
    return it != symbolTableBase_.end() ? it->second : 0;
  }

  void parseRelaTableAtVa(uint64_t va, uint64_t size, uint32_t symbolBase, bool hasAddend) {
    uint64_t offset = 0;
    if (!virtualToFileOffset(va, offset)) {
      XDEC_LOG_WARN(logBinary(), "relocation table at 0x{:x} is not in any mapped file range", va);
      return;
    }
    parseRelaTable(offset, size, symbolBase, hasAddend);
  }

  void parseRelaTable(uint64_t tableOffset, uint64_t size, uint32_t symbolBase, bool hasAddend) {
    const unsigned entrySize = is64Bit_ ? (hasAddend ? kRela64Size : kRel64Size)
                                        : (hasAddend ? kRela32Size : kRel32Size);
    const unsigned infoSize = addressSize_;

    for (uint64_t cursor = 0; cursor + entrySize <= size; cursor += entrySize) {
      const uint64_t base = tableOffset + cursor;
      const uint64_t offsetField = reader_.read(base, addressSize_);
      const uint64_t info = reader_.read(base + addressSize_, infoSize);
      const int64_t addend =
          hasAddend ? reader_.signedRead(base + addressSize_ * 2, addressSize_) : 0;
      if (reader_.failed()) {
        reader_.clearFailure();
        XDEC_LOG_WARN(logBinary(), "truncated relocation table at file offset 0x{:x}",
                      tableOffset);
        return;
      }

      // r_info packs the symbol index and type differently per ELF class.
      const uint32_t symbolIndex =
          is64Bit_ ? static_cast<uint32_t>(info >> 32) : static_cast<uint32_t>(info >> 8);
      const uint32_t type = is64Bit_ ? static_cast<uint32_t>(info & 0xFFFFFFFFu)
                                     : static_cast<uint32_t>(info & 0xFFu);

      Relocation relocation;
      relocation.va = offsetField;
      relocation.rawType = type;
      relocation.addend = addend;
      relocation.symbolIndex =
          symbolIndex == 0 ? kNoSymbol : static_cast<uint32_t>(symbolBase + symbolIndex);
      relocations_.push_back(relocation);
    }
  }

  void parseRelrTableAtVa(uint64_t va, uint64_t size) {
    uint64_t offset = 0;
    if (!virtualToFileOffset(va, offset)) {
      XDEC_LOG_WARN(logBinary(), "RELR table at 0x{:x} is not in any mapped file range", va);
      return;
    }
    parseRelrTable(offset, size);
  }

  /// RELR is a bitmap encoding of relative relocations: a word with the low bit
  /// clear sets the cursor, a word with it set is a bitmap for the following
  /// entries.
  void parseRelrTable(uint64_t tableOffset, uint64_t size) {
    const unsigned wordSize = addressSize_;
    const unsigned bitsPerWord = wordSize * 8;
    uint64_t cursor = 0;

    for (uint64_t position = 0; position + wordSize <= size; position += wordSize) {
      const uint64_t word = reader_.read(tableOffset + position, wordSize);
      if (reader_.failed()) {
        reader_.clearFailure();
        XDEC_LOG_WARN(logBinary(), "truncated RELR table at file offset 0x{:x}", tableOffset);
        return;
      }

      if ((word & 1) == 0) {
        Relocation relocation;
        relocation.va = word;
        relocation.rawType = 0;
        relocation.kind = RelocKind::Relative;
        relocation.width = wordSize;
        relocation.isImplicitAddend = true;
        relocations_.push_back(relocation);
        cursor = word + wordSize;
        continue;
      }

      uint64_t bitmap = word >> 1;
      uint64_t address = cursor;
      for (unsigned bit = 0; bit < bitsPerWord - 1; ++bit, address += wordSize) {
        if ((bitmap & (uint64_t{1} << bit)) == 0) {
          continue;
        }
        Relocation relocation;
        relocation.va = address;
        relocation.rawType = 0;
        relocation.kind = RelocKind::Relative;
        relocation.width = wordSize;
        relocation.isImplicitAddend = true;
        relocations_.push_back(relocation);
      }
      cursor += uint64_t{bitsPerWord - 1} * wordSize;
    }
  }

  /// Turns raw relocation records into concrete slot values where possible.
  /// Runs after the memory map is finalised because RELR entries take their
  /// addend from the current contents of the slot.
  void resolveRelocations() {
    const unsigned pointerWidth = addressSize_;
    unsigned resolved = 0;
    unsigned imports = 0;
    unsigned unknown = 0;

    for (Relocation& relocation : relocations_) {
      if (relocation.isImplicitAddend) {
        // RELR: the slot already holds the link-time address, and the loader
        // adds the load bias. At a bias of zero the existing value is final.
        std::byte buffer[8] = {};
        if (memory_.read(relocation.va, std::span<std::byte>{buffer, pointerWidth})) {
          uint64_t value = 0;
          for (unsigned index = pointerWidth; index-- > 0;) {
            const unsigned byteIndex = endian_ == Endian::Little ? index : pointerWidth - 1 - index;
            value = (value << 8) | static_cast<uint64_t>(std::to_integer<uint8_t>(buffer[byteIndex]));
          }
          relocation.value = value;
          relocation.hasValue = true;
          ++resolved;
        }
        continue;
      }

      const RelocSemantics semantics = classifyRelocation(arch_, relocation.rawType);
      relocation.kind = semantics.kind;
      relocation.width = semantics.width != 0 ? semantics.width : pointerWidth;

      if (semantics.kind == RelocKind::Unknown) {
        ++unknown;
        continue;
      }
      if (semantics.biasRelative) {
        // Load bias is zero in the analysis address space.
        relocation.value = static_cast<uint64_t>(relocation.addend);
        relocation.hasValue = true;
        ++resolved;
        continue;
      }
      if (!semantics.usesSymbol) {
        continue;
      }

      if (relocation.symbolIndex == kNoSymbol || relocation.symbolIndex >= symbols_.size()) {
        ++unknown;
        continue;
      }
      const Symbol& symbol = symbols_[relocation.symbolIndex];
      if (!symbol.defined) {
        // An import: the value is only known once a real loader binds it. Left
        // unresolved on purpose, with the symbol name still attached so callers
        // can identify the target by name.
        ++imports;
        continue;
      }
      if (semantics.kind == RelocKind::TlsModule || semantics.kind == RelocKind::TlsOffset ||
          semantics.kind == RelocKind::TlsDescriptor) {
        // Thread-local slots depend on runtime module layout.
        continue;
      }
      relocation.value = symbol.va + static_cast<uint64_t>(relocation.addend);
      relocation.hasValue = true;
      ++resolved;
    }

    XDEC_LOG_DEBUG(logBinary(),
                   "relocations: {} total, {} resolved, {} unresolved imports, {} unclassified",
                   relocations_.size(), resolved, imports, unknown);
  }

  // -- assembly -------------------------------------------------------------

  Result<std::unique_ptr<BinaryImage>> finish() {
    ImageContents contents;
    contents.format = BinaryFormat::Elf;
    contents.kind = kind_;
    contents.arch = arch_;
    contents.endian = endian_;
    contents.pointerBits = addressSize_ * 8;
    contents.entryPoint = entryPoint_;
    contents.hasEntryPoint = entryPoint_ != 0;
    contents.path = path_;
    contents.soname = std::move(soname_);
    contents.neededLibraries = std::move(neededLibraries_);
    contents.symbols = std::move(symbols_);
    contents.relocations = std::move(relocations_);

    contents.sections.reserve(sectionHeaders_.size());
    for (const SectionHeader& header : sectionHeaders_) {
      Section section;
      section.name = header.name;
      section.va = header.addr;
      section.size = header.size;
      section.fileOffset = header.offset;
      section.fileSize = header.type == kShtNoBits ? 0 : header.size;
      section.rawType = header.type;
      section.allocated = (header.flags & kShfAlloc) != 0;
      section.zeroFilled = header.type == kShtNoBits;
      // PROGBITS and NOBITS are the two ways an ELF section holds the program's
      // own bytes; every other type is a table about the program.
      section.programData = header.type == kShtProgBits || header.type == kShtNoBits;
      section.entrySize = static_cast<unsigned>(header.entrySize);
      section.permissions = section.allocated ? permissionsFromSectionFlags(header.flags)
                                              : MemoryPermissions::None;
      contents.sections.push_back(std::move(section));
    }

    // The file buffer moves last: the memory map holds spans into it, and
    // FileBuffer keeps its data address stable across moves.
    contents.memory = std::move(memory_);
    contents.store.addPart(path_, std::move(file_));
    return std::make_unique<BinaryImage>(std::move(contents));
  }

  FileBuffer file_;
  FieldReader reader_;
  std::string path_;

  bool is64Bit_ = true;
  Endian endian_ = Endian::Little;
  const HeaderLayout* layout_ = &kHeader64;
  unsigned addressSize_ = 8;

  Arch arch_ = Arch::Unknown;
  BinaryKind kind_ = BinaryKind::Unknown;
  uint64_t entryPoint_ = 0;
  uint64_t programHeaderOffset_ = 0;
  uint64_t sectionHeaderOffset_ = 0;
  unsigned programHeaderEntrySize_ = 0;
  unsigned programHeaderCount_ = 0;
  unsigned sectionHeaderEntrySize_ = 0;
  unsigned sectionHeaderCount_ = 0;
  unsigned sectionNameTableIndex_ = 0;

  std::vector<ProgramHeader> programHeaders_;
  std::vector<SectionHeader> sectionHeaders_;
  std::vector<DynamicEntry> dynamic_;
  FieldReader dynamicStringTable_{{}, Endian::Little};
  bool haveDynamicStringTable_ = false;

  MemoryMap memory_;
  std::vector<Symbol> symbols_;
  std::vector<Relocation> relocations_;
  std::vector<std::string> neededLibraries_;
  std::string soname_;

  std::unordered_map<uint32_t, uint32_t> symbolTableBase_;
  uint32_t dynamicSymbolBase_ = 0;
  bool haveDynamicSymbols_ = false;
};

}  // namespace

Result<std::unique_ptr<BinaryImage>> loadElf(FileBuffer file, std::string path) {
  ElfLoader loader{std::move(file), std::move(path)};
  return loader.load();
}

}  // namespace xdec::binary
