// Mach-O on-disk constants and structure offsets.
//
// Mirrors elf_format.h: named field offsets rather than packed structs, so
// the parser works regardless of host alignment/byte order assumptions. Only
// the 64-bit, non-byte-swapped shape is covered -- see macho.cpp for why.
#pragma once

#include <cstdint>

namespace xdec::binary::macho {

// mach_header_64 magic. MH_MAGIC (0xfeedface) is the 32-bit counterpart;
// MH_CIGAM/MH_CIGAM_64 are the byte-swapped forms seen when the file's
// endianness differs from the host reading it. iOS/macOS ship little-endian
// only, so a byte-swapped magic would mean a foreign-endian host, not a
// foreign-endian file; not supported.
inline constexpr uint32_t kMagic64 = 0xfeedfacfu;
inline constexpr uint32_t kMagic32 = 0xfeedfaceu;
inline constexpr uint32_t kMagicFat = 0xcafebabeu;

// cputype (CPU_ARCH_ABI64 = 0x01000000 ORed with the 32-bit base type).
inline constexpr uint32_t kCpuTypeArm64 = 0x0100000cu;
inline constexpr uint32_t kCpuTypeX8664 = 0x01000007u;

// filetype
inline constexpr uint32_t kFileTypeExecute = 2;
inline constexpr uint32_t kFileTypeDylib = 6;
inline constexpr uint32_t kFileTypeBundle = 8;

// Load command sizes/offsets. mach_header_64 is 32 bytes; load commands
// follow immediately, each starting with {cmd, cmdsize}.
inline constexpr uint64_t kHeaderSize64 = 32;

inline constexpr uint32_t kLcSegment64 = 0x19;
inline constexpr uint32_t kLcSymtab = 0x2;
inline constexpr uint32_t kLcLoadDylib = 0xc;
inline constexpr uint32_t kLcLoadDylinker = 0xe;
inline constexpr uint32_t kLcDyldInfo = 0x22;
inline constexpr uint32_t kLcDyldInfoOnly = 0x80000022u;  // LC_DYLD_INFO | LC_REQ_DYLD
inline constexpr uint32_t kLcMain = 0x80000028u;          // LC_MAIN | LC_REQ_DYLD
inline constexpr uint32_t kLcDyldChainedFixups = 0x80000034u;

// segment_command_64 fields, relative to the load command's own base.
inline constexpr uint64_t kSegmentNameOffset = 8;    // char[16]
inline constexpr uint64_t kSegmentNameSize = 16;
inline constexpr uint64_t kSegmentVmAddr = 24;
inline constexpr uint64_t kSegmentVmSize = 32;
inline constexpr uint64_t kSegmentFileOff = 40;
inline constexpr uint64_t kSegmentFileSize = 48;
inline constexpr uint64_t kSegmentInitProt = 60;
inline constexpr uint64_t kSegmentNSects = 64;
inline constexpr uint64_t kSegmentHeaderSize = 72;

// section_64 fields, relative to the section's own base (80 bytes each).
inline constexpr uint64_t kSectionNameOffset = 0;   // char[16]
inline constexpr uint64_t kSectionSegNameOffset = 16;  // char[16]
inline constexpr uint64_t kSectionAddr = 32;
inline constexpr uint64_t kSectionSize = 40;
inline constexpr uint64_t kSectionFileOffset = 48;
inline constexpr uint64_t kSectionFlags = 64;
inline constexpr uint64_t kSectionRecordSize = 80;

// section_64 flags: low byte is the section type.
inline constexpr uint32_t kSectionTypeMask = 0x000000ffu;
inline constexpr uint32_t kSZerofill = 1;

// VM protection bits (segment_command_64.initprot).
inline constexpr uint32_t kVmProtRead = 1;
inline constexpr uint32_t kVmProtWrite = 2;
inline constexpr uint32_t kVmProtExecute = 4;

// symtab_command fields, relative to the load command's own base.
inline constexpr uint64_t kSymtabSymOff = 8;
inline constexpr uint64_t kSymtabNSyms = 12;
inline constexpr uint64_t kSymtabStrOff = 16;
inline constexpr uint64_t kSymtabStrSize = 20;

// nlist_64 fields (16 bytes each).
inline constexpr uint64_t kNlistStrx = 0;
inline constexpr uint64_t kNlistType = 4;
inline constexpr uint64_t kNlistSect = 5;
inline constexpr uint64_t kNlistValue = 8;
inline constexpr uint64_t kNlistRecordSize = 16;

// nlist n_type bits.
inline constexpr uint8_t kNStab = 0xe0;
inline constexpr uint8_t kNTypeMask = 0x0e;
inline constexpr uint8_t kNExt = 0x01;
inline constexpr uint8_t kNUndf = 0x0;
inline constexpr uint8_t kNAbs = 0x2;
inline constexpr uint8_t kNSect = 0xe;

// dylib_command fields, relative to the load command's own base.
inline constexpr uint64_t kDylibNameOffset = 8;

// dyld_info_command fields, relative to the load command's own base.
inline constexpr uint64_t kDyldInfoRebaseOff = 8;
inline constexpr uint64_t kDyldInfoRebaseSize = 12;
inline constexpr uint64_t kDyldInfoBindOff = 16;
inline constexpr uint64_t kDyldInfoBindSize = 20;
inline constexpr uint64_t kDyldInfoLazyBindOff = 32;
inline constexpr uint64_t kDyldInfoLazyBindSize = 36;

// entry_point_command (LC_MAIN) fields, relative to the load command's own base.
inline constexpr uint64_t kEntryPointOffset = 8;

// Rebase opcodes (REBASE_OPCODE_*), packed as (opcode << 4 | immediate).
inline constexpr uint8_t kRebaseOpcodeMask = 0xf0;
inline constexpr uint8_t kRebaseImmediateMask = 0x0f;
inline constexpr uint8_t kRebaseOpDone = 0x00;
inline constexpr uint8_t kRebaseOpSetTypeImm = 0x10;
inline constexpr uint8_t kRebaseOpSetSegmentAndOffsetUleb = 0x20;
inline constexpr uint8_t kRebaseOpAddAddrUleb = 0x30;
inline constexpr uint8_t kRebaseOpAddAddrImmScaled = 0x40;
inline constexpr uint8_t kRebaseOpDoRebaseImmTimes = 0x50;
inline constexpr uint8_t kRebaseOpDoRebaseUlebTimes = 0x60;
inline constexpr uint8_t kRebaseOpDoRebaseAddAddrUleb = 0x70;
inline constexpr uint8_t kRebaseOpDoRebaseUlebTimesSkippingUleb = 0x80;

// Bind opcodes (BIND_OPCODE_*), same packing as rebase opcodes.
inline constexpr uint8_t kBindOpcodeMask = 0xf0;
inline constexpr uint8_t kBindImmediateMask = 0x0f;
inline constexpr uint8_t kBindOpDone = 0x00;
inline constexpr uint8_t kBindOpSetDylibOrdinalImm = 0x10;
inline constexpr uint8_t kBindOpSetDylibOrdinalUleb = 0x20;
inline constexpr uint8_t kBindOpSetDylibSpecialImm = 0x30;
inline constexpr uint8_t kBindOpSetSymbolTrailingFlagsImm = 0x40;
inline constexpr uint8_t kBindOpSetTypeImm = 0x50;
inline constexpr uint8_t kBindOpSetAddendSleb = 0x60;
inline constexpr uint8_t kBindOpSetSegmentAndOffsetUleb = 0x70;
inline constexpr uint8_t kBindOpAddAddrUleb = 0x80;
inline constexpr uint8_t kBindOpDoBind = 0x90;
inline constexpr uint8_t kBindOpDoBindAddAddrUleb = 0xa0;
inline constexpr uint8_t kBindOpDoBindAddAddrImmScaled = 0xb0;
inline constexpr uint8_t kBindOpDoBindUlebTimesSkippingUleb = 0xc0;

}  // namespace xdec::binary::macho
