// pltGotSlot / importNameForPltStub: decoding the standard AArch64 ELF PLT
// stub (`adrp; ldr`) into the GOT slot it loads from, and that slot into the
// import the loader bound it to.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <span>

#include "xdec/analysis/plt_stub.h"

using xdec::LoaderValue;
using xdec::MemoryFacts;
using xdec::Result;
using xdec::analysis::importNameForPltStub;
using xdec::analysis::pltGotSlot;

namespace {

/// `adrp x<rd>, <page>`: op=1, immlo(2), 10000, immhi(19), Rd(5). `pageOffset`
/// is the byte distance from the stub's own page to the target page -- it
/// must be a multiple of 4096, the granularity ADRP addresses.
[[nodiscard]] constexpr uint32_t adrp(unsigned rd, int64_t pageOffset) {
  const int64_t imm = pageOffset >> 12;
  const uint32_t immlo = static_cast<uint32_t>(imm & 0x3);
  const uint32_t immhi = static_cast<uint32_t>((imm >> 2) & 0x7ffff);
  return 0x90000000U | (immlo << 29) | (immhi << 5) | rd;
}

/// `ldr x<rt>, [x<rn>, #byteOffset]` (unsigned offset, 64-bit): the second
/// half of the stub, reading the GOT slot the `adrp` above pointed at the page
/// of.
[[nodiscard]] constexpr uint32_t ldr(unsigned rt, unsigned rn, uint64_t byteOffset) {
  return 0xf9400000U | (static_cast<uint32_t>(byteOffset / 8) << 10U) | (rn << 5U) | rt;
}

/// A byte-addressable image: just enough to answer `pltGotSlot`'s one
/// eight-byte read of the stub.
class Bytes {
 public:
  void word(uint64_t va, uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
      bytes_[va + index] = static_cast<std::byte>((value >> (index * 8)) & 0xff);
    }
  }

  [[nodiscard]] xdec::ByteReader reader() const {
    return [this](uint64_t va, std::span<std::byte> out) -> Result<void> {
      for (std::size_t index = 0; index < out.size(); ++index) {
        const auto found = bytes_.find(va + index);
        if (found == bytes_.end()) {
          return xdec::err(xdec::DiagCode::BadFormat, "unmapped read at {:#x}", va + index);
        }
        out[index] = found->second;
      }
      return xdec::ok();
    };
  }

 private:
  std::map<uint64_t, std::byte> bytes_;
};

/// A standard two-instruction PLT stub at `stubVa`, loading its target out of
/// `gotVa` through `reg`.
void putStub(Bytes& image, uint64_t stubVa, uint64_t gotVa, unsigned reg = 17) {
  const uint64_t stubPage = stubVa & ~uint64_t{0xfff};
  const uint64_t gotPage = gotVa & ~uint64_t{0xfff};
  image.word(stubVa, adrp(reg, static_cast<int64_t>(gotPage - stubPage)));
  image.word(stubVa + 4, ldr(reg, reg, gotVa - gotPage));
}

/// Unconditional `b <targetVa>` instruction.
[[nodiscard]] constexpr uint32_t branch(uint64_t pc, uint64_t targetVa) {
  const int64_t delta = static_cast<int64_t>(targetVa - pc) >> 2;
  return 0x14000000U | (static_cast<uint32_t>(delta) & 0x03ffffffU);
}

/// `add x<rd>, x<rn>, #imm12` (64-bit, unshifted).
[[nodiscard]] constexpr uint32_t addImm64(unsigned rd, unsigned rn, uint32_t imm12) {
  return 0x91000000U | ((imm12 & 0xfffU) << 10U) | (rn << 5U) | rd;
}

}  // namespace

TEST_CASE("adrp+ldr decodes to the GOT slot it addresses", "[analysis][plt-stub]") {
  Bytes image;
  putStub(image, 0x2000, 0xa008);
  const std::optional<uint64_t> slot = pltGotSlot(image.reader(), 0x2000);
  REQUIRE(slot.has_value());
  CHECK(*slot == 0xa008);
}

TEST_CASE("a negative page offset still resolves", "[analysis][plt-stub]") {
  Bytes image;
  putStub(image, 0xb000, 0x1010);
  const std::optional<uint64_t> slot = pltGotSlot(image.reader(), 0xb000);
  REQUIRE(slot.has_value());
  CHECK(*slot == 0x1010);
}

TEST_CASE("a non-adrp first instruction is not a PLT stub", "[analysis][plt-stub]") {
  Bytes image;
  image.word(0x2000, 0xd65f03c0U);  // ret
  image.word(0x2004, ldr(17, 17, 8));
  CHECK_FALSE(pltGotSlot(image.reader(), 0x2000).has_value());
}

TEST_CASE("an ldr through a different register than adrp's destination is not a stub",
          "[analysis][plt-stub]") {
  Bytes image;
  image.word(0x2000, adrp(17, 0x8000));
  image.word(0x2004, ldr(16, 8, 8));  // reads through x8, not x17
  CHECK_FALSE(pltGotSlot(image.reader(), 0x2000).has_value());
}

TEST_CASE("an unmapped stub address resolves to nothing", "[analysis][plt-stub]") {
  Bytes image;
  CHECK_FALSE(pltGotSlot(image.reader(), 0x2000).has_value());
}

TEST_CASE("importNameForPltStub follows the stub to the loader's name for its slot",
          "[analysis][plt-stub]") {
  Bytes image;
  putStub(image, 0x1d28a0, 0x1f9b78);
  MemoryFacts facts;
  facts.loader = [](uint64_t va) {
    LoaderValue value;
    if (va == 0x1f9b78) {
      value.importName = "__errno";
    }
    return value;
  };
  const std::optional<std::string> name = importNameForPltStub(0x1d28a0, image.reader(), facts);
  REQUIRE(name.has_value());
  CHECK(*name == "__errno");
}

TEST_CASE("importNameForPltStub is nothing when the slot is not loader-bound",
          "[analysis][plt-stub]") {
  Bytes image;
  putStub(image, 0x1d28a0, 0x1f9b78);
  CHECK_FALSE(importNameForPltStub(0x1d28a0, image.reader(), MemoryFacts{}).has_value());
}

TEST_CASE("pltGotSlot chases multi-hop unconditional branch islands", "[analysis][plt-stub]") {
  Bytes image;
  image.word(0x100000, branch(0x100000, 0x200000));
  image.word(0x200000, branch(0x200000, 0x300000));
  putStub(image, 0x300000, 0x401080, 16);

  const std::optional<uint64_t> slot = pltGotSlot(image.reader(), 0x100000);
  REQUIRE(slot.has_value());
  CHECK(*slot == 0x401080);
}

TEST_CASE("pltGotSlot terminates cleanly on a branch-island cycle", "[analysis][plt-stub]") {
  Bytes image;
  image.word(0x1000, branch(0x1000, 0x2000));
  image.word(0x2000, branch(0x2000, 0x1000));
  CHECK_FALSE(pltGotSlot(image.reader(), 0x1000).has_value());
}

TEST_CASE("Apple __objc_stubs selector trampoline resolves both GOT slot and selector slot",
          "[analysis][plt-stub]") {
  Bytes image;
  // Branch island -> __objc_stubs (`adrp x1; ldr x1, [x1, #off]; b _objc_msgSend`) -> __stubs.
  image.word(0x100000, branch(0x100000, 0x200000));
  putStub(image, 0x200000, 0x500c88, /*reg=*/1);
  image.word(0x200008, branch(0x200008, 0x300000));
  putStub(image, 0x300000, 0x4019b8, /*reg=*/16);
  image.word(0x300008, 0xd61f0200U);  // br x16

  const std::optional<uint64_t> got = pltGotSlot(image.reader(), 0x100000);
  REQUIRE(got.has_value());
  CHECK(*got == 0x4019b8);

  const std::optional<uint64_t> sel =
      xdec::analysis::objcStubSelectorSlot(image.reader(), 0x100000);
  REQUIRE(sel.has_value());
  CHECK(*sel == 0x500c88);
}

TEST_CASE("arm64e adrp+add+ldr stub decodes to the GOT slot", "[analysis][plt-stub]") {
  Bytes image;
  const uint64_t stubVa = 0x10000;
  const uint64_t gotVa = 0x20abc;
  const uint64_t stubPage = stubVa & ~uint64_t{0xfff};
  const uint64_t gotPage = gotVa & ~uint64_t{0xfff};
  image.word(stubVa, adrp(17, static_cast<int64_t>(gotPage - stubPage)));
  image.word(stubVa + 4, addImm64(17, 17, static_cast<uint32_t>(gotVa - gotPage)));
  image.word(stubVa + 8, ldr(16, 17, 0));

  const std::optional<uint64_t> got = pltGotSlot(image.reader(), stubVa);
  REQUIRE(got.has_value());
  CHECK(*got == gotVa);
}
