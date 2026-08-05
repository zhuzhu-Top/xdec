// ELF on-disk constants and structure offsets.
//
// Field offsets are spelled out as named constants rather than as packed C
// structs so that the same parser handles ELF32 and ELF64 in either byte order
// without reinterpret_cast or alignment assumptions.
#pragma once

#include <cstdint>

namespace xdec::binary::elf {

inline constexpr unsigned kIdentSize = 16;
inline constexpr unsigned kIdentClass = 4;
inline constexpr unsigned kIdentData = 5;
inline constexpr unsigned kIdentVersion = 6;
inline constexpr unsigned kIdentOsAbi = 7;

inline constexpr uint8_t kClass32 = 1;
inline constexpr uint8_t kClass64 = 2;
inline constexpr uint8_t kData2Lsb = 1;
inline constexpr uint8_t kData2Msb = 2;

// e_type
inline constexpr uint16_t kEtNone = 0;
inline constexpr uint16_t kEtRel = 1;
inline constexpr uint16_t kEtExec = 2;
inline constexpr uint16_t kEtDyn = 3;
inline constexpr uint16_t kEtCore = 4;

// e_machine
inline constexpr uint16_t kEmNone = 0;
inline constexpr uint16_t kEm386 = 3;
inline constexpr uint16_t kEmMips = 8;
inline constexpr uint16_t kEmPpc64 = 21;
inline constexpr uint16_t kEmArm = 40;
inline constexpr uint16_t kEmX8664 = 62;
inline constexpr uint16_t kEmAArch64 = 183;
inline constexpr uint16_t kEmRiscV = 243;
inline constexpr uint16_t kEmLoongArch = 258;

// Elf header field offsets, {elf32, elf64}.
struct HeaderLayout {
  unsigned type;
  unsigned machine;
  unsigned version;
  unsigned entry;
  unsigned phoff;
  unsigned shoff;
  unsigned flags;
  unsigned ehsize;
  unsigned phentsize;
  unsigned phnum;
  unsigned shentsize;
  unsigned shnum;
  unsigned shstrndx;
  unsigned addressSize;
};

inline constexpr HeaderLayout kHeader32{16, 18, 20, 24, 28, 32, 36, 40, 42, 44, 46, 48, 50, 4};
inline constexpr HeaderLayout kHeader64{16, 18, 20, 24, 32, 40, 48, 52, 54, 56, 58, 60, 62, 8};

// Program header
inline constexpr uint32_t kPtNull = 0;
inline constexpr uint32_t kPtLoad = 1;
inline constexpr uint32_t kPtDynamic = 2;
inline constexpr uint32_t kPtInterp = 3;
inline constexpr uint32_t kPtNote = 4;
inline constexpr uint32_t kPtPhdr = 6;
inline constexpr uint32_t kPtTls = 7;
inline constexpr uint32_t kPtGnuRelro = 0x6474e552;

inline constexpr uint32_t kPfExecute = 1;
inline constexpr uint32_t kPfWrite = 2;
inline constexpr uint32_t kPfRead = 4;

struct ProgramHeaderLayout {
  unsigned type;
  unsigned flags;
  unsigned offset;
  unsigned vaddr;
  unsigned filesz;
  unsigned memsz;
  unsigned align;
  unsigned size;
};

inline constexpr ProgramHeaderLayout kProgram32{0, 24, 4, 8, 16, 20, 28, 32};
inline constexpr ProgramHeaderLayout kProgram64{0, 4, 8, 16, 32, 40, 48, 56};

// Section header
inline constexpr uint32_t kShtNull = 0;
inline constexpr uint32_t kShtProgBits = 1;
inline constexpr uint32_t kShtSymtab = 2;
inline constexpr uint32_t kShtStrtab = 3;
inline constexpr uint32_t kShtRela = 4;
inline constexpr uint32_t kShtHash = 5;
inline constexpr uint32_t kShtDynamic = 6;
inline constexpr uint32_t kShtNote = 7;
inline constexpr uint32_t kShtNoBits = 8;
inline constexpr uint32_t kShtRel = 9;
inline constexpr uint32_t kShtDynsym = 11;
inline constexpr uint32_t kShtInitArray = 14;
inline constexpr uint32_t kShtFiniArray = 15;
inline constexpr uint32_t kShtPreInitArray = 16;
inline constexpr uint32_t kShtRelr = 19;
inline constexpr uint32_t kShtGnuHash = 0x6ffffff6;
inline constexpr uint32_t kShtAndroidRela = 0x60000002;
inline constexpr uint32_t kShtAndroidRelr = 0x6fffff00;

inline constexpr uint64_t kShfWrite = 0x1;
inline constexpr uint64_t kShfAlloc = 0x2;
inline constexpr uint64_t kShfExecInstr = 0x4;
inline constexpr uint64_t kShfTls = 0x400;

struct SectionHeaderLayout {
  unsigned name;
  unsigned type;
  unsigned flags;
  unsigned addr;
  unsigned offset;
  unsigned size;
  unsigned link;
  unsigned info;
  unsigned addralign;
  unsigned entsize;
  unsigned flagsSize;
  unsigned size_;
};

inline constexpr SectionHeaderLayout kSection32{0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 4, 40};
inline constexpr SectionHeaderLayout kSection64{0, 4, 8, 16, 24, 32, 40, 44, 48, 56, 8, 64};

// Symbols
inline constexpr uint16_t kShnUndef = 0;
inline constexpr uint16_t kShnAbs = 0xfff1;
inline constexpr uint16_t kShnCommon = 0xfff2;

inline constexpr uint8_t kSttNoType = 0;
inline constexpr uint8_t kSttObject = 1;
inline constexpr uint8_t kSttFunc = 2;
inline constexpr uint8_t kSttSection = 3;
inline constexpr uint8_t kSttFile = 4;
inline constexpr uint8_t kSttCommon = 5;
inline constexpr uint8_t kSttTls = 6;
inline constexpr uint8_t kSttGnuIfunc = 10;

inline constexpr uint8_t kStbLocal = 0;
inline constexpr uint8_t kStbGlobal = 1;
inline constexpr uint8_t kStbWeak = 2;

inline constexpr uint8_t kStvDefault = 0;
inline constexpr uint8_t kStvInternal = 1;
inline constexpr uint8_t kStvHidden = 2;
inline constexpr uint8_t kStvProtected = 3;

struct SymbolLayout {
  unsigned name;
  unsigned info;
  unsigned other;
  unsigned shndx;
  unsigned value;
  unsigned size;
  unsigned addressSize;
  unsigned entrySize;
};

inline constexpr SymbolLayout kSymbol32{0, 12, 13, 14, 4, 8, 4, 16};
inline constexpr SymbolLayout kSymbol64{0, 4, 5, 6, 8, 16, 8, 24};

// Dynamic tags
inline constexpr int64_t kDtNull = 0;
inline constexpr int64_t kDtNeeded = 1;
inline constexpr int64_t kDtPltRelSz = 2;
inline constexpr int64_t kDtPltGot = 3;
inline constexpr int64_t kDtHash = 4;
inline constexpr int64_t kDtStrtab = 5;
inline constexpr int64_t kDtSymtab = 6;
inline constexpr int64_t kDtRela = 7;
inline constexpr int64_t kDtRelaSz = 8;
inline constexpr int64_t kDtRelaEnt = 9;
inline constexpr int64_t kDtStrSz = 10;
inline constexpr int64_t kDtSymEnt = 11;
inline constexpr int64_t kDtInit = 12;
inline constexpr int64_t kDtFini = 13;
inline constexpr int64_t kDtSoname = 14;
inline constexpr int64_t kDtRel = 17;
inline constexpr int64_t kDtRelSz = 18;
inline constexpr int64_t kDtRelEnt = 19;
inline constexpr int64_t kDtPltRel = 20;
inline constexpr int64_t kDtTextRel = 22;
inline constexpr int64_t kDtJmpRel = 23;
inline constexpr int64_t kDtInitArray = 25;
inline constexpr int64_t kDtFiniArray = 26;
inline constexpr int64_t kDtInitArraySz = 27;
inline constexpr int64_t kDtFiniArraySz = 28;
inline constexpr int64_t kDtFlags = 30;
inline constexpr int64_t kDtPreInitArray = 32;
inline constexpr int64_t kDtPreInitArraySz = 33;
inline constexpr int64_t kDtRelrSz = 35;
inline constexpr int64_t kDtRelr = 36;
inline constexpr int64_t kDtRelrEnt = 37;
inline constexpr int64_t kDtGnuHash = 0x6ffffef5;
// Android packed relocations, seen in APK-shipped shared objects.
inline constexpr int64_t kDtAndroidRela = 0x60000003;
inline constexpr int64_t kDtAndroidRelaSz = 0x60000004;
inline constexpr int64_t kDtAndroidRel = 0x6000000f;
inline constexpr int64_t kDtAndroidRelSz = 0x60000010;
inline constexpr int64_t kDtAndroidRelr = 0x6fffe000;
inline constexpr int64_t kDtAndroidRelrSz = 0x6fffe001;

// Relocation record sizes
inline constexpr unsigned kRel32Size = 8;
inline constexpr unsigned kRel64Size = 16;
inline constexpr unsigned kRela32Size = 12;
inline constexpr unsigned kRela64Size = 24;

// AArch64 relocation types
inline constexpr uint32_t kAArch64None = 0;
inline constexpr uint32_t kAArch64Abs64 = 257;
inline constexpr uint32_t kAArch64Abs32 = 258;
inline constexpr uint32_t kAArch64Abs16 = 259;
inline constexpr uint32_t kAArch64Prel64 = 260;
inline constexpr uint32_t kAArch64Prel32 = 261;
inline constexpr uint32_t kAArch64GlobDat = 1025;
inline constexpr uint32_t kAArch64JumpSlot = 1026;
inline constexpr uint32_t kAArch64Relative = 1027;
inline constexpr uint32_t kAArch64TlsDtpMod = 1028;
inline constexpr uint32_t kAArch64TlsDtpRel = 1029;
inline constexpr uint32_t kAArch64TlsTpRel = 1030;
inline constexpr uint32_t kAArch64TlsDesc = 1031;
inline constexpr uint32_t kAArch64IRelative = 1032;

// x86-64 relocation types
inline constexpr uint32_t kX8664None = 0;
inline constexpr uint32_t kX866464 = 1;
inline constexpr uint32_t kX8664GlobDat = 6;
inline constexpr uint32_t kX8664JumpSlot = 7;
inline constexpr uint32_t kX8664Relative = 8;
inline constexpr uint32_t kX866432 = 10;
inline constexpr uint32_t kX8664DtpMod64 = 16;
inline constexpr uint32_t kX8664DtpOff64 = 17;
inline constexpr uint32_t kX8664TpOff64 = 18;
inline constexpr uint32_t kX8664IRelative = 37;

// ARM (32-bit) relocation types
inline constexpr uint32_t kArmNone = 0;
inline constexpr uint32_t kArmAbs32 = 2;
inline constexpr uint32_t kArmGlobDat = 21;
inline constexpr uint32_t kArmJumpSlot = 22;
inline constexpr uint32_t kArmRelative = 23;
inline constexpr uint32_t kArmTlsDtpMod32 = 17;
inline constexpr uint32_t kArmTlsDtpOff32 = 18;
inline constexpr uint32_t kArmTlsTpOff32 = 19;
inline constexpr uint32_t kArmIRelative = 160;

}  // namespace xdec::binary::elf
