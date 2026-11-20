// Mach-O loading, focused on LC_DYLD_CHAINED_FIXUPS (see binary/macho.cpp):
// the newer chained-pointer scheme replaces LC_DYLD_INFO's opcode streams
// with a packed-word chain walked page by page, so a rebase's target lives
// in bitfields rather than in the on-disk pointer bytes themselves.
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "xdec/binary/image.h"
#include "xdec/binary/macho_metadata.h"

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
  static constexpr uint64_t kFixupsDataSize = 96;

  explicit MachOBuilder(uint16_t pointerFormat, uint32_t importsFormat = 1)
      : bytes_(0x2000, std::byte{0}) {
    header();
    segment(0x20, "__TEXT", kTextVa, 0x1000, 0, 0x1000, /*initprot=*/5);
    segment(0x68, "__DATA", kDataVa, 0x1000, kDataFileOff, 0x1000, /*initprot=*/3);
    chainedFixupsCommand(0xb0);
    chainedFixupsData(pointerFormat, importsFormat);
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
  void chainedFixupsData(uint16_t pointerFormat, uint32_t importsFormat) {
    const uint64_t h = kFixupsDataOff;
    const uint32_t importSize = importsFormat == 1 ? 4 : (importsFormat == 2 ? 8 : 16);
    const uint32_t symbolsOffset = 64 + importSize;
    put32(h + 0, 0);    // fixups_version
    put32(h + 4, 28);   // starts_offset -> dyld_chained_starts_in_image at h+28
    put32(h + 8, 64);   // imports_offset -> h+64
    put32(h + 12, symbolsOffset);
    put32(h + 16, 1);   // imports_count
    put32(h + 20, importsFormat);
    put32(h + 24, 0);   // symbols_format

    const uint64_t startsInImage = h + 28;
    put32(startsInImage, 2);       // seg_count (__TEXT, __DATA)
    put32(startsInImage + 4, 0);   // __TEXT: no fixups
    put32(startsInImage + 8, 12);  // __DATA: dyld_chained_starts_in_segment at +12

    const uint64_t startsInSegment = startsInImage + 12;
    put32(startsInSegment, 24);          // size
    put16(startsInSegment + 4, 0x1000);  // page_size
    put16(startsInSegment + 6, pointerFormat);
    put64(startsInSegment + 8, kDataVa - kTextVa);  // image-relative __DATA offset
    put32(startsInSegment + 16, 0);  // max_valid_pointer (unused by this loader)
    put16(startsInSegment + 20, 1);  // page_count
    put16(startsInSegment + 22, 0);  // page_start[0]: chain starts at the page's own byte 0

    const uint64_t imports = h + 64;
    put32(imports, 1);  // ordinal 1, not weak, name_offset 0 (formats 1/2)
    if (importsFormat == 2) {
      put32(imports + 4, 0x1234);  // signed addend
    } else if (importsFormat == 3) {
      put64(imports, 1);           // ordinal 1, name_offset 0
      put64(imports + 8, 0x1234);  // signed addend
    }

    const uint64_t symbols = h + symbolsOffset;
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

TEST_CASE("addend-bearing chained import formats preserve import names",
          "[binary][macho]") {
  for (const uint32_t importsFormat : {2u, 3u}) {
    CAPTURE(importsFormat);
    MachOBuilder builder{/*pointerFormat=*/6, importsFormat};
    builder.putSlot(MachOBuilder::kDataFileOff, uint64_t{1} << 63);

    auto image = loadMachO(builder.buffer(), "test.dylib");
    REQUIRE(image);

    const auto imported = (*image)->importNameAt(MachOBuilder::kDataVa);
    REQUIRE(imported.has_value());
    CHECK(*imported == "my_import");
  }
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

TEST_CASE("MachOMetadata resolves compact unwind bounds, __cfstring, and __objc_selrefs",
          "[binary][macho]") {
  // Minimal Mach-O with __TEXT (__text, __cstring, __unwind_info) and __DATA (__cfstring, __objc_selrefs).
  std::vector<std::byte> bytes(0x3000, std::byte{0});
  const auto put16 = [&](uint64_t off, uint16_t v) {
    bytes[off] = static_cast<std::byte>(v & 0xff);
    bytes[off + 1] = static_cast<std::byte>((v >> 8) & 0xff);
  };
  const auto put32 = [&](uint64_t off, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) {
      bytes[off + i] = static_cast<std::byte>((v >> (i * 8)) & 0xff);
    }
  };
  const auto put64 = [&](uint64_t off, uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) {
      bytes[off + i] = static_cast<std::byte>((v >> (i * 8)) & 0xff);
    }
  };
  const auto putName = [&](uint64_t off, std::string_view text) {
    std::memcpy(bytes.data() + off, text.data(), text.size());
  };

  // mach_header_64: 2 LC_SEGMENT_64 commands (__TEXT with 3 sections, __DATA with 2 sections).
  put32(0, 0xfeedfacfu);
  put32(4, 0x0100000cu);
  put32(12, 6);
  put32(16, 2);
  put32(20, (72 + 3 * 80) + (72 + 2 * 80));

  uint64_t cur = 32;
  put32(cur, 0x19);
  put32(cur + 4, 72 + 3 * 80);
  putName(cur + 8, "__TEXT");
  put64(cur + 24, 0x1000);
  put64(cur + 32, 0x1000);
  put64(cur + 40, 0x1000);
  put64(cur + 48, 0x1000);
  put32(cur + 60, 5);
  put32(cur + 64, 3);
  cur += 72;

  const auto addSection = [&](std::string_view sect, std::string_view seg, uint64_t addr,
                              uint64_t size, uint32_t foff, uint32_t flags) {
    putName(cur + 0, sect);
    putName(cur + 16, seg);
    put64(cur + 32, addr);
    put64(cur + 40, size);
    put32(cur + 48, foff);
    put32(cur + 64, flags);
    cur += 80;
  };
  addSection("__text", "__TEXT", 0x1400, 0x200, 0x1400, 0x80000400u);
  addSection("__cstring", "__TEXT", 0x1600, 0x40, 0x1600, 2);
  addSection("__unwind_info", "__TEXT", 0x1700, 0x80, 0x1700, 0);

  put32(cur, 0x19);
  put32(cur + 4, 72 + 2 * 80);
  putName(cur + 8, "__DATA");
  put64(cur + 24, 0x2000);
  put64(cur + 32, 0x1000);
  put64(cur + 40, 0x2000);
  put64(cur + 48, 0x1000);
  put32(cur + 60, 3);
  put32(cur + 64, 2);
  cur += 72;
  addSection("__cfstring", "__DATA", 0x2000, 32, 0x2000, 0);
  addSection("__objc_selrefs", "__DATA", 0x2020, 8, 0x2020, 0);

  // __cstring content at file offset 0x1600 (VA 0x1600): "hello" and "doWork:"
  putName(0x1600, "hello");
  putName(0x1610, "doWork:");

  // __cfstring entry at 0x2000 pointing to "hello"
  put64(0x2008, 0x07c8);
  put64(0x2010, 0x1600);
  put64(0x2018, 5);

  // __objc_selrefs entry at 0x2020 pointing to "doWork:"
  put64(0x2020, 0x1610);

  // __unwind_info at file offset 0x1700:
  // Header (28 bytes): indexOffset=28, indexCount=2; first-level entries at +28 (24 bytes);
  // regular second-level page at +52: kind=2, entryPageOffset=8, entryCount=2.
  put32(0x1700 + 0, 1);    // version
  put32(0x1700 + 20, 28);  // indexSectionOffset
  put32(0x1700 + 24, 2);   // indexCount
  put32(0x1700 + 28, 0x400);  // funcOffset 0 (0x1400 - 0x1000)
  put32(0x1700 + 32, 52);     // secondLevelPagesSectionOffset
  put32(0x1700 + 40, 0x520);  // sentinel funcOffset (0x1520 - 0x1000)
  put32(0x1700 + 44, 0);      // sentinel
  put32(0x1700 + 52, 2);      // regular page kind
  put16(0x1700 + 56, 8);      // entryPageOffset
  put16(0x1700 + 58, 2);      // entryCount
  put32(0x1700 + 60, 0x400);  // entry 0: 0x1400..0x1480
  put32(0x1700 + 68, 0x480);  // entry 1: 0x1480..0x1520

  auto image = loadMachO(FileBuffer::fromBytes(bytes), "meta.dylib");
  REQUIRE(image);
  const auto* meta = dynamic_cast<const MachOMetadata*>((*image)->formatMetadata());
  REQUIRE(meta != nullptr);

  const auto bounds = meta->functionBoundsAt(**image, 0x1400);
  REQUIRE(bounds.has_value());
  CHECK(bounds->first == 0x1400);
  CHECK(bounds->second == 0x1480);

  const auto nextFn = meta->nextUnwindFunctionAfter(**image, 0x1480);
  REQUIRE(nextFn.has_value());
  CHECK(*nextFn == 0x1520);

  const auto cf = meta->cfStringAt(**image, 0x2000);
  REQUIRE(cf.has_value());
  CHECK(*cf == "hello");

  const auto cfSym = meta->resolveDataSymbolAt(**image, 0x2000);
  REQUIRE(cfSym.has_value());
  CHECK(cfSym->starts_with("cfstr_hello_"));

  const auto selSym = meta->resolveDataSymbolAt(**image, 0x2020);
  REQUIRE(selSym.has_value());
  CHECK(*selSym == "selRef_doWork_");
}

TEST_CASE("FileBuffer::fromFile maps a binary read-only with exact contents",
          "[binary][backing-store]") {
  const std::filesystem::path tmpPath =
      std::filesystem::temp_directory_path() / "xdec_mmap_test.bin";
  {
    std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
    const char payload[] = "XDEC_MMAP_TEST_1234567890";
    out.write(payload, sizeof(payload) - 1);
  }
  {
    auto buf = FileBuffer::fromFile(tmpPath);
    REQUIRE(buf);
    CHECK(buf->size() == 25);
    const std::string_view view(reinterpret_cast<const char*>(buf->bytes().data()), buf->size());
    CHECK(view == "XDEC_MMAP_TEST_1234567890");
    // Move transfers mapping cleanly without invalidating bytes().
    FileBuffer moved = std::move(*buf);
    CHECK(moved.size() == 25);
  }
  std::error_code ec;
  std::filesystem::remove(tmpPath, ec);
}
