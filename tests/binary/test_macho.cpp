// Mach-O loading, focused on LC_DYLD_CHAINED_FIXUPS (see binary/macho.cpp):
// the newer chained-pointer scheme replaces LC_DYLD_INFO's opcode streams
// with a packed-word chain walked page by page, so a rebase's target lives
// in bitfields rather than in the on-disk pointer bytes themselves.
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

#include "xdec/binary/image.h"

using namespace xdec;
using namespace xdec::binary;

namespace {

/// A minimal little-endian ARM64 Mach-O dylib: __TEXT then __DATA, with an
/// LC_DYLD_CHAINED_FIXUPS command describing one page of __DATA. The chain
/// itself -- one rebase slot followed by one bind slot -- is written
/// separately by each test, since that is the part under test.
class MachOBuilder {
 public:
  static constexpr uint64_t kTextVa = 0;
  static constexpr uint64_t kDataVa = 0x1000;
  static constexpr uint64_t kDataFileOff = 0x1000;
  static constexpr uint64_t kFixupsDataOff = 0x100;
  static constexpr uint64_t kFixupsDataSize = 78;

  explicit MachOBuilder(uint16_t pointerFormat) : bytes_(0x2000, std::byte{0}) {
    header();
    segment(0x20, "__TEXT", kTextVa, 0x1000, 0, 0x1000, /*initprot=*/5);
    segment(0x68, "__DATA", kDataVa, 0x1000, kDataFileOff, 0x1000, /*initprot=*/3);
    chainedFixupsCommand(0xb0);
    chainedFixupsData(pointerFormat);
  }

  void putSlot(uint64_t fileOffset, uint64_t word) { put64(fileOffset, word); }

  [[nodiscard]] FileBuffer buffer() const { return FileBuffer::fromBytes(bytes_); }

 private:
  void put32(uint64_t offset, uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
      bytes_[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xff);
    }
  }
  void put16(uint64_t offset, uint16_t value) {
    for (unsigned index = 0; index < 2; ++index) {
      bytes_[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xff);
    }
  }
  void put64(uint64_t offset, uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) {
      bytes_[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xff);
    }
  }
  void putString(uint64_t offset, std::string_view text) {
    std::memcpy(bytes_.data() + offset, text.data(), text.size());
  }

  void header() {
    put32(0, 0xfeedfacfu);   // magic (MH_MAGIC_64)
    put32(4, 0x0100000cu);   // cputype (ARM64)
    put32(12, 6);            // filetype (MH_DYLIB)
    put32(16, 3);            // ncmds
    put32(20, 72 + 72 + 16); // sizeofcmds
  }

  void segment(uint64_t base, std::string_view name, uint64_t vmaddr, uint64_t vmsize,
              uint64_t fileoff, uint64_t filesize, uint32_t initprot) {
    put32(base, 0x19);      // LC_SEGMENT_64
    put32(base + 4, 72);    // cmdsize, no sections
    putString(base + 8, name);
    put64(base + 24, vmaddr);
    put64(base + 32, vmsize);
    put64(base + 40, fileoff);
    put64(base + 48, filesize);
    put32(base + 60, initprot);
  }

  void chainedFixupsCommand(uint64_t base) {
    put32(base, 0x80000034u);  // LC_DYLD_CHAINED_FIXUPS
    put32(base + 4, 16);
    put32(base + 8, static_cast<uint32_t>(kFixupsDataOff));
    put32(base + 12, static_cast<uint32_t>(kFixupsDataSize));
  }

  /// dyld_chained_fixups_header + dyld_chained_starts_in_image +
  /// dyld_chained_starts_in_segment (one page of __DATA) + one import +
  /// its name -- everything except the chain's own pointer words, which
  /// live in __DATA's own mapped bytes (see putSlot).
  void chainedFixupsData(uint16_t pointerFormat) {
    const uint64_t h = kFixupsDataOff;
    put32(h + 0, 0);    // fixups_version
    put32(h + 4, 28);   // starts_offset -> dyld_chained_starts_in_image at h+28
    put32(h + 8, 64);   // imports_offset -> h+64
    put32(h + 12, 68);  // symbols_offset -> h+68
    put32(h + 16, 1);   // imports_count
    put32(h + 20, 1);   // imports_format (DYLD_CHAINED_IMPORT)
    put32(h + 24, 0);   // symbols_format

    const uint64_t startsInImage = h + 28;
    put32(startsInImage, 2);       // seg_count (__TEXT, __DATA)
    put32(startsInImage + 4, 0);   // __TEXT: no fixups
    put32(startsInImage + 8, 12);  // __DATA: dyld_chained_starts_in_segment at +12

    const uint64_t startsInSegment = startsInImage + 12;
    put32(startsInSegment, 24);          // size
    put16(startsInSegment + 4, 0x1000);  // page_size
    put16(startsInSegment + 6, pointerFormat);
    put64(startsInSegment + 8, 0);   // segment_offset: slot 0 is __DATA's own byte 0
    put32(startsInSegment + 16, 0);  // max_valid_pointer (unused by this loader)
    put16(startsInSegment + 20, 1);  // page_count
    put16(startsInSegment + 22, 0);  // page_start[0]: chain starts at the page's own byte 0

    const uint64_t imports = h + 64;
    put32(imports, 1);  // ordinal 1, not weak, name_offset 0

    const uint64_t symbols = h + 68;
    putString(symbols, "my_import");
  }

  std::vector<std::byte> bytes_;
};

}  // namespace

TEST_CASE("a chained rebase slot reads as its decoded target, not its packed chain word",
          "[binary][macho]") {
  MachOBuilder builder{/*pointerFormat=*/6};  // DYLD_CHAINED_PTR_64_OFFSET
  // dyld_chained_ptr_64_rebase: target=0x55, high8=0, next=2 (4-byte-unit
  // stride to the bind slot 8 bytes after it -- each slot is a full 8-byte
  // pointer word, so adjacent slots must be 2 units, not 1, apart), bind=0.
  builder.putSlot(MachOBuilder::kDataFileOff, 0x55ull | (uint64_t{2} << 51));
  // dyld_chained_ptr_64_bind: ordinal=0 (the one import above), addend=0,
  // next=0 (chain ends here), bind=1.
  builder.putSlot(MachOBuilder::kDataFileOff + 8, uint64_t{1} << 63);

  auto image = loadMachO(builder.buffer(), "test.dylib");
  REQUIRE(image);

  // PTR_64_OFFSET's target is an offset from the image base (__TEXT's own
  // vmaddr, 0 here), so the decoded value is the target verbatim.
  const Result<uint64_t> rebased = (*image)->readPointer(MachOBuilder::kDataVa);
  REQUIRE(rebased);
  CHECK(*rebased == 0x55);

  // The bind slot is left symbolic (a real loader's choice of address
  // depends on which module wins the symbol), but named from the imports
  // table the same way a classic BIND opcode would be.
  const auto imported = (*image)->importNameAt(MachOBuilder::kDataVa + 8);
  REQUIRE(imported.has_value());
  CHECK(*imported == "my_import");
}

TEST_CASE("an undecoded chained pointer format leaves its slots as raw bytes",
          "[binary][macho]") {
  MachOBuilder builder{/*pointerFormat=*/1};  // DYLD_CHAINED_PTR_ARM64E, not implemented
  // Whatever bit pattern this is, it is not a target this loader will
  // decode -- the point of the test is that it comes back unchanged.
  builder.putSlot(MachOBuilder::kDataFileOff, 0x1122334455667788ull);
  builder.putSlot(MachOBuilder::kDataFileOff + 8, 0);

  auto image = loadMachO(builder.buffer(), "test.dylib");
  REQUIRE(image);

  const Result<uint64_t> raw = (*image)->readPointer(MachOBuilder::kDataVa);
  REQUIRE(raw);
  CHECK(*raw == 0x1122334455667788ull);
  CHECK_FALSE((*image)->importNameAt(MachOBuilder::kDataVa + 8).has_value());
}
