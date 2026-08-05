#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "xdec/binary/image.h"

using namespace xdec;
using namespace xdec::binary;

namespace {

/// Builds a minimal but well-formed AArch64 ELF64 shared object in memory.
///
/// Layout is chosen so every interesting case is present: a code segment where
/// virtual addresses equal file offsets, a data segment where they do not, a
/// zero-filled `.bss` tail, a defined symbol, an undefined import, and one
/// relocation of each resolvable and unresolvable kind.
class ElfBuilder {
 public:
  static constexpr uint64_t kTextVa = 0x400;
  static constexpr uint64_t kTextSize = 0x10;
  static constexpr uint64_t kDataVa = 0x2000;
  static constexpr uint64_t kDataSize = 0x100;
  static constexpr uint64_t kBssVa = 0x2100;
  static constexpr uint64_t kBssSize = 0x100;
  static constexpr uint64_t kRelativeSlot = 0x2000;
  static constexpr uint64_t kRelativeTarget = 0x400;
  static constexpr uint64_t kImportSlot = 0x2008;

  ElfBuilder() : bytes_(0x1100, std::byte{0}) { build(); }

  [[nodiscard]] std::span<const std::byte> bytes() const { return bytes_; }
  [[nodiscard]] std::vector<std::byte>& mutableBytes() { return bytes_; }

  void put8(uint64_t offset, uint8_t value) { bytes_[offset] = static_cast<std::byte>(value); }

  void put16(uint64_t offset, uint16_t value) {
    for (unsigned index = 0; index < 2; ++index) {
      put8(offset + index, static_cast<uint8_t>(value >> (index * 8)));
    }
  }

  void put32(uint64_t offset, uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
      put8(offset + index, static_cast<uint8_t>(value >> (index * 8)));
    }
  }

  void put64(uint64_t offset, uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) {
      put8(offset + index, static_cast<uint8_t>(value >> (index * 8)));
    }
  }

  void putString(uint64_t offset, std::string_view text) {
    std::memcpy(bytes_.data() + offset, text.data(), text.size());
  }

 private:
  void build() {
    // -- ELF header ---------------------------------------------------------
    put8(0, 0x7F);
    putString(1, "ELF");
    put8(4, 2);  // ELFCLASS64
    put8(5, 1);  // ELFDATA2LSB
    put8(6, 1);  // EV_CURRENT
    put16(16, 3);    // ET_DYN
    put16(18, 183);  // EM_AARCH64
    put32(20, 1);
    put64(24, 0);      // e_entry
    put64(32, 0x40);   // e_phoff
    put64(40, 0x600);  // e_shoff
    put32(48, 0);
    put16(52, 64);  // e_ehsize
    put16(54, 56);  // e_phentsize
    put16(56, 2);   // e_phnum
    put16(58, 64);  // e_shentsize
    put16(60, 8);   // e_shnum
    put16(62, 7);   // e_shstrndx

    // -- program headers ----------------------------------------------------
    // Code: virtual address equals file offset.
    programHeader(0x40, 1, 5, 0, 0, 0x1000, 0x1000);
    // Data: virtual address diverges from the file offset, and the memory size
    // exceeds the file size so the tail is the .bss.
    programHeader(0x78, 1, 6, 0x1000, kDataVa, kDataSize, kDataSize + kBssSize);

    // -- .dynstr ------------------------------------------------------------
    putString(0x100 + 1, "imported_func");
    putString(0x100 + 15, "my_function");

    // -- .dynsym ------------------------------------------------------------
    // Entry 0 is the reserved null symbol and stays zeroed.
    symbol(0x200 + 24, /*nameOffset=*/1, /*info=*/0x12, /*shndx=*/0, /*value=*/0, /*size=*/0);
    symbol(0x200 + 48, /*nameOffset=*/15, /*info=*/0x12, /*shndx=*/1, kTextVa, kTextSize);

    // -- .rela.dyn ----------------------------------------------------------
    // R_AARCH64_RELATIVE: resolvable to addend at a load bias of zero.
    relocation(0x300, kRelativeSlot, /*symbolIndex=*/0, /*type=*/1027, kRelativeTarget);
    // R_AARCH64_GLOB_DAT against an undefined symbol: must stay symbolic.
    relocation(0x300 + 24, kImportSlot, /*symbolIndex=*/1, /*type=*/1025, 0);

    // -- .text --------------------------------------------------------------
    // ret; nop; nop; nop
    put32(0x400, 0xD65F03C0);
    put32(0x404, 0xD503201F);
    put32(0x408, 0xD503201F);
    put32(0x40C, 0xD503201F);

    // -- .shstrtab ----------------------------------------------------------
    putString(0x500 + 1, ".text");
    putString(0x500 + 7, ".data");
    putString(0x500 + 13, ".bss");
    putString(0x500 + 18, ".dynsym");
    putString(0x500 + 26, ".dynstr");
    putString(0x500 + 34, ".rela.dyn");
    putString(0x500 + 44, ".shstrtab");

    // -- section headers ----------------------------------------------------
    // 0: null section, left zeroed.
    sectionHeader(0x600 + 64 * 1, 1, 1, 0x2 | 0x4, kTextVa, 0x400, kTextSize, 0, 0);
    sectionHeader(0x600 + 64 * 2, 7, 1, 0x2 | 0x1, kDataVa, 0x1000, kDataSize, 0, 0);
    sectionHeader(0x600 + 64 * 3, 13, 8, 0x2 | 0x1, kBssVa, 0x1100, kBssSize, 0, 0);
    sectionHeader(0x600 + 64 * 4, 18, 11, 0x2, 0x200, 0x200, 24 * 3, 5, 24);
    sectionHeader(0x600 + 64 * 5, 26, 3, 0x2, 0x100, 0x100, 27, 0, 0);
    sectionHeader(0x600 + 64 * 6, 34, 4, 0x2, 0x300, 0x300, 24 * 2, 4, 24);
    sectionHeader(0x600 + 64 * 7, 44, 3, 0, 0, 0x500, 54, 0, 0);
  }

  void programHeader(uint64_t at, uint32_t type, uint32_t flags, uint64_t offset, uint64_t vaddr,
                     uint64_t fileSize, uint64_t memorySize) {
    put32(at + 0, type);
    put32(at + 4, flags);
    put64(at + 8, offset);
    put64(at + 16, vaddr);
    put64(at + 24, vaddr);
    put64(at + 32, fileSize);
    put64(at + 40, memorySize);
    put64(at + 48, 0x1000);
  }

  void sectionHeader(uint64_t at, uint32_t nameOffset, uint32_t type, uint64_t flags, uint64_t addr,
                     uint64_t offset, uint64_t size, uint32_t link, uint64_t entrySize) {
    put32(at + 0, nameOffset);
    put32(at + 4, type);
    put64(at + 8, flags);
    put64(at + 16, addr);
    put64(at + 24, offset);
    put64(at + 32, size);
    put32(at + 40, link);
    put32(at + 44, 0);
    put64(at + 48, 8);
    put64(at + 56, entrySize);
  }

  void symbol(uint64_t at, uint32_t nameOffset, uint8_t info, uint16_t sectionIndex, uint64_t value,
              uint64_t size) {
    put32(at + 0, nameOffset);
    put8(at + 4, info);
    put8(at + 5, 0);
    put16(at + 6, sectionIndex);
    put64(at + 8, value);
    put64(at + 16, size);
  }

  void relocation(uint64_t at, uint64_t offset, uint32_t symbolIndex, uint32_t type,
                  uint64_t addend) {
    put64(at + 0, offset);
    put64(at + 8, (static_cast<uint64_t>(symbolIndex) << 32) | type);
    put64(at + 16, addend);
  }

  std::vector<std::byte> bytes_;
};

Result<std::unique_ptr<BinaryImage>> loadBuilt(const ElfBuilder& builder) {
  return loadElf(FileBuffer::fromBytes(builder.bytes()), "synthetic.so");
}

}  // namespace

TEST_CASE("elf loader identifies the target", "[elf]") {
  const ElfBuilder builder;
  auto loaded = loadBuilt(builder);
  REQUIRE(loaded);
  const BinaryImage& image = *loaded.value();

  CHECK(image.format() == BinaryFormat::Elf);
  CHECK(image.kind() == BinaryKind::SharedObject);
  CHECK(image.arch() == Arch::AArch64);
  CHECK(image.endian() == Endian::Little);
  CHECK(image.pointerBits() == 64);
  CHECK(image.pointerBytes() == 8);
  CHECK_FALSE(image.hasEntryPoint());
}

TEST_CASE("elf memory map comes from segments, not sections", "[elf]") {
  const ElfBuilder builder;
  auto loaded = loadBuilt(builder);
  REQUIRE(loaded);
  const BinaryImage& image = *loaded.value();

  REQUIRE(image.memory().regions().size() == 2);
  const auto& code = image.memory().regions()[0];
  const auto& data = image.memory().regions()[1];

  CHECK(code.va == 0);
  CHECK(code.size == 0x1000);
  CHECK(hasPermission(code.permissions, MemoryPermissions::Execute));

  CHECK(data.va == ElfBuilder::kDataVa);
  // The memory size exceeds the file size by exactly the .bss.
  CHECK(data.size == ElfBuilder::kDataSize + ElfBuilder::kBssSize);
  CHECK(data.fileSize == ElfBuilder::kDataSize);
  CHECK_FALSE(hasPermission(data.permissions, MemoryPermissions::Execute));

  CHECK(image.isExecutable(ElfBuilder::kTextVa));
  CHECK_FALSE(image.isExecutable(ElfBuilder::kDataVa));
  CHECK(image.isWritable(ElfBuilder::kDataVa));
}

TEST_CASE("elf bss reads as zeros rather than failing", "[elf]") {
  const ElfBuilder builder;
  auto loaded = loadBuilt(builder);
  REQUIRE(loaded);
  const BinaryImage& image = *loaded.value();

  const Section* bss = image.sectionNamed(".bss");
  REQUIRE(bss != nullptr);
  CHECK(bss->zeroFilled);
  CHECK(bss->va == ElfBuilder::kBssVa);

  // The whole point: a `.bss` address has no file bytes behind it, and reading
  // it must yield the zero it actually holds at analysis time.
  auto value = image.readUnsigned(ElfBuilder::kBssVa, 8);
  REQUIRE(value);
  CHECK(value.value() == 0);

  std::vector<std::byte> buffer(0x80);
  REQUIRE(image.read(ElfBuilder::kBssVa + 0x40, buffer));
  for (const std::byte byte : buffer) {
    CHECK(std::to_integer<uint8_t>(byte) == 0);
  }
}

TEST_CASE("elf reads fail on unmapped addresses", "[elf]") {
  const ElfBuilder builder;
  auto loaded = loadBuilt(builder);
  REQUIRE(loaded);
  const BinaryImage& image = *loaded.value();

  // The gap between the two segments is genuinely absent, and saying so is more
  // useful than inventing zeros for it.
  CHECK_FALSE(image.isMapped(0x1800));
  auto read = image.readUnsigned(0x1800, 4);
  REQUIRE_FALSE(read);
  CHECK(read.error().code() == DiagCode::UnmappedAddress);
}

TEST_CASE("elf resolves relative relocations and keeps imports symbolic", "[elf]") {
  const ElfBuilder builder;
  auto loaded = loadBuilt(builder);
  REQUIRE(loaded);
  const BinaryImage& image = *loaded.value();

  REQUIRE(image.relocations().size() == 2);

  const Relocation* relative = image.relocationAt(ElfBuilder::kRelativeSlot);
  REQUIRE(relative != nullptr);
  CHECK(relative->kind == RelocKind::Relative);
  CHECK(relative->hasValue);
  CHECK(relative->value == ElfBuilder::kRelativeTarget);

  // The slot's file bytes are zero; only the overlay makes the pointer visible.
  auto slot = image.readUnsigned(ElfBuilder::kRelativeSlot, 8);
  REQUIRE(slot);
  CHECK(slot.value() == ElfBuilder::kRelativeTarget);

  const Relocation* import = image.relocationAt(ElfBuilder::kImportSlot);
  REQUIRE(import != nullptr);
  CHECK(import->kind == RelocKind::GotSlot);
  // An import's value is only known once a real loader binds it, so it must be
  // reported as unresolved rather than guessed at.
  CHECK_FALSE(import->hasValue);
  const auto name = image.importNameAt(ElfBuilder::kImportSlot);
  REQUIRE(name.has_value());
  CHECK(*name == "imported_func");

  CHECK(image.relocationOverlapping(ElfBuilder::kRelativeSlot + 4, 8) != nullptr);
  CHECK(image.relocationOverlapping(ElfBuilder::kTextVa, 0x10) == nullptr);
}

TEST_CASE("elf parses symbols and their addresses", "[elf]") {
  const ElfBuilder builder;
  auto loaded = loadBuilt(builder);
  REQUIRE(loaded);
  const BinaryImage& image = *loaded.value();

  REQUIRE(image.symbols().size() == 3);

  const Symbol* defined = image.symbolNamed("my_function");
  REQUIRE(defined != nullptr);
  CHECK(defined->va == ElfBuilder::kTextVa);
  CHECK(defined->size == ElfBuilder::kTextSize);
  CHECK(defined->kind == SymbolKind::Function);
  CHECK(defined->binding == SymbolBinding::Global);
  CHECK(defined->defined);
  CHECK(defined->exported);

  const Symbol* imported = image.symbolNamed("imported_func");
  REQUIRE(imported != nullptr);
  CHECK_FALSE(imported->defined);
  CHECK_FALSE(imported->exported);

  CHECK(image.symbolAt(ElfBuilder::kTextVa) == defined);
  CHECK(image.symbolAt(ElfBuilder::kTextVa + 4) == nullptr);
  CHECK(image.symbolContaining(ElfBuilder::kTextVa + 4) == defined);
  CHECK(image.symbolContaining(ElfBuilder::kTextVa + ElfBuilder::kTextSize) == nullptr);
}

TEST_CASE("elf exposes code bytes for decoding", "[elf]") {
  const ElfBuilder builder;
  auto loaded = loadBuilt(builder);
  REQUIRE(loaded);
  const BinaryImage& image = *loaded.value();

  auto code = image.codeView(ElfBuilder::kTextVa, 16);
  REQUIRE(code);
  REQUIRE(code.value().size() == 16);
  // Little-endian encoding of `ret`.
  CHECK(std::to_integer<uint8_t>(code.value()[0]) == 0xC0);
  CHECK(std::to_integer<uint8_t>(code.value()[3]) == 0xD6);

  // A code range in the zero-filled tail is not file-backed, so a zero-copy
  // view is impossible and the caller must be told rather than given zeros.
  auto inBss = image.codeView(ElfBuilder::kBssVa, 4);
  REQUIRE_FALSE(inBss);
  CHECK(inBss.error().code() == DiagCode::OutOfRange);
}

TEST_CASE("elf loader rejects malformed images", "[elf]") {
  SECTION("bad magic") {
    ElfBuilder builder;
    builder.put8(1, 'X');
    auto loaded = loadElf(FileBuffer::fromBytes(builder.bytes()), "bad.so");
    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().code() == DiagCode::BadFormat);
  }

  SECTION("unknown class") {
    ElfBuilder builder;
    builder.put8(4, 9);
    auto loaded = loadElf(FileBuffer::fromBytes(builder.bytes()), "bad.so");
    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().code() == DiagCode::BadFormat);
  }

  SECTION("unknown machine") {
    ElfBuilder builder;
    builder.put16(18, 0xFFFF);
    auto loaded = loadElf(FileBuffer::fromBytes(builder.bytes()), "bad.so");
    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().code() == DiagCode::UnsupportedArch);
  }

  SECTION("segment reaching past the file") {
    ElfBuilder builder;
    // Grow both p_filesz and p_memsz of the first segment beyond the buffer.
    builder.put64(0x40 + 32, 0x99999);
    builder.put64(0x40 + 40, 0x99999);
    auto loaded = loadElf(FileBuffer::fromBytes(builder.bytes()), "bad.so");
    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().code() == DiagCode::BadFormat);
  }

  SECTION("segment with more file bytes than memory bytes") {
    ElfBuilder builder;
    // p_filesz > p_memsz is malformed. Clamping would silently discard the
    // contradiction, so the loader has to refuse it.
    builder.put64(0x40 + 32, 0x2000);
    builder.put64(0x40 + 40, 0x1000);
    auto loaded = loadElf(FileBuffer::fromBytes(builder.bytes()), "bad.so");
    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().code() == DiagCode::BadFormat);
  }

  SECTION("truncated file") {
    ElfBuilder builder;
    std::vector<std::byte> truncated{builder.bytes().begin(), builder.bytes().begin() + 8};
    auto loaded = loadElf(FileBuffer::fromBytes(truncated), "bad.so");
    REQUIRE_FALSE(loaded);
  }
}

TEST_CASE("openBinary names formats it cannot load yet", "[elf]") {
  const std::filesystem::path missing = "definitely-not-here.bin";
  auto loaded = openBinary(missing);
  REQUIRE_FALSE(loaded);
  CHECK(loaded.error().code() == DiagCode::IoError);
}
