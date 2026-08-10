// pltGotSlot / importNameForPltStub: AArch64 PLT stub decode (see the header
// for why the lifter and every import-aware pass need this).
#include "xdec/analysis/plt_stub.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace xdec::analysis {

namespace {

[[nodiscard]] uint32_t loadWordLe(std::span<const std::byte> bytes, std::size_t wordIndex) {
  uint32_t word = 0;
  for (std::size_t offset = 4; offset-- > 0;) {
    word = (word << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(bytes[wordIndex * 4 + offset]));
  }
  return word;
}

[[nodiscard]] uint32_t field(uint32_t word, int hi, int lo) noexcept {
  return (word >> lo) & ((uint32_t{1} << (hi - lo + 1)) - 1);
}

// ADRP Xd, label: op=1, immlo(2), 10000, immhi(19), Rd(5). The page offset is
// the sign-extended immhi:immlo, already scaled to bytes (<< 12) since ADRP
// addresses whole 4K pages.
[[nodiscard]] bool isAdrp(uint32_t word) noexcept {
  return field(word, 31, 31) == 1 && field(word, 28, 24) == 0b10000;
}

[[nodiscard]] int64_t adrpPageOffset(uint32_t word) noexcept {
  const uint32_t immlo = field(word, 30, 29);
  const uint32_t immhi = field(word, 23, 5);
  const uint32_t imm21 = (immhi << 2) | immlo;
  // Sign-extend a 21-bit field held in the low bits of a 32-bit value.
  const int32_t signExtended = static_cast<int32_t>(imm21 << 11) >> 11;
  return static_cast<int64_t>(signExtended) << 12;
}

[[nodiscard]] uint32_t adrpDestReg(uint32_t word) noexcept { return field(word, 4, 0); }

// LDR Xt, [Xn, #imm] (unsigned offset, 64-bit): size=11, 111, V=0, 01, opc=01.
// The byte offset is imm12 scaled by the 8-byte access size.
[[nodiscard]] bool isLdrUnsignedOffset64(uint32_t word) noexcept {
  return field(word, 31, 30) == 0b11 && field(word, 29, 27) == 0b111 &&
        field(word, 26, 26) == 0 && field(word, 25, 24) == 0b01 &&
        field(word, 23, 22) == 0b01;
}

[[nodiscard]] uint32_t ldrBaseReg(uint32_t word) noexcept { return field(word, 9, 5); }
[[nodiscard]] uint64_t ldrByteOffset(uint32_t word) noexcept {
  return uint64_t{field(word, 21, 10)} * 8;
}

}  // namespace

std::optional<uint64_t> pltGotSlot(const ByteReader& reader, uint64_t stubVa) {
  std::array<std::byte, 8> bytes{};
  if (!reader(stubVa, std::span<std::byte>{bytes})) {
    return std::nullopt;
  }
  const uint32_t adrp = loadWordLe(bytes, 0);
  const uint32_t ldr = loadWordLe(bytes, 1);
  if (!isAdrp(adrp) || !isLdrUnsignedOffset64(ldr)) {
    return std::nullopt;
  }
  if (ldrBaseReg(ldr) != adrpDestReg(adrp)) {
    return std::nullopt;
  }
  const uint64_t page = (stubVa & ~uint64_t{0xfff}) + static_cast<uint64_t>(adrpPageOffset(adrp));
  return page + ldrByteOffset(ldr);
}

std::optional<std::string> importNameForPltStub(uint64_t stubVa, const ByteReader& reader,
                                                const MemoryFacts& facts) {
  const std::optional<uint64_t> got = pltGotSlot(reader, stubVa);
  if (!got.has_value()) {
    return std::nullopt;
  }
  const LoaderValue bound = facts.loaderValueAt(*got);
  if (bound.importName.empty()) {
    return std::nullopt;
  }
  return bound.importName;
}

}  // namespace xdec::analysis
