// pltGotSlot / importNameForPltStub: AArch64 PLT/Mach-O stub decode (see the
// header for why the lifter and every import-aware pass need this).
#include "xdec/analysis/plt_stub.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace xdec::analysis {

namespace {

constexpr unsigned kMaxBranchIslandHops = 16;

[[nodiscard]] uint32_t loadWordLe(std::span<const std::byte> bytes, std::size_t wordIndex) {
  uint32_t word = 0;
  for (std::size_t offset = 4; offset-- > 0;) {
    word = (word << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(bytes[wordIndex * 4 + offset]));
  }
  return word;
}

[[nodiscard]] std::optional<uint32_t> readInsn(const ByteReader& reader, uint64_t va) {
  std::array<std::byte, 4> bytes{};
  if (!reader(va, std::span<std::byte>{bytes})) {
    return std::nullopt;
  }
  return loadWordLe(bytes, 0);
}

[[nodiscard]] uint32_t field(uint32_t word, int hi, int lo) noexcept {
  return (word >> lo) & ((uint32_t{1} << (hi - lo + 1)) - 1);
}

// B <label>: op=0, 00101, imm26.
[[nodiscard]] bool isUncondBranch(uint32_t word) noexcept {
  return (word & 0xfc000000u) == 0x14000000u;
}

[[nodiscard]] uint64_t branchTarget(uint64_t pc, uint32_t word) noexcept {
  const uint32_t imm26 = word & 0x03ffffffu;
  const int32_t signExtended = static_cast<int32_t>(imm26 << 6) >> 6;
  return pc + static_cast<uint64_t>(static_cast<int64_t>(signExtended) << 2);
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

// ADD Xd, Xn, #imm12{, lsl #12}: sf=1, op=0, S=0, 100010, sh, imm12, Rn, Rd.
[[nodiscard]] bool isAddImm64(uint32_t word) noexcept {
  return (word & 0xff800000u) == 0x91000000u;
}

[[nodiscard]] uint32_t addDestReg(uint32_t word) noexcept { return field(word, 4, 0); }
[[nodiscard]] uint32_t addBaseReg(uint32_t word) noexcept { return field(word, 9, 5); }
[[nodiscard]] uint64_t addImm64(uint32_t word) noexcept {
  const uint64_t imm12 = field(word, 21, 10);
  const uint32_t shift = field(word, 22, 22);
  return shift != 0 ? (imm12 << 12) : imm12;
}

// LDR Xt, [Xn, #imm] (unsigned offset, 64-bit): size=11, 111, V=0, 01, opc=01.
// The byte offset is imm12 scaled by the 8-byte access size.
[[nodiscard]] bool isLdrUnsignedOffset64(uint32_t word) noexcept {
  return field(word, 31, 30) == 0b11 && field(word, 29, 27) == 0b111 &&
        field(word, 26, 26) == 0 && field(word, 25, 24) == 0b01 &&
        field(word, 23, 22) == 0b01;
}

[[nodiscard]] uint32_t ldrDestReg(uint32_t word) noexcept { return field(word, 4, 0); }
[[nodiscard]] uint32_t ldrBaseReg(uint32_t word) noexcept { return field(word, 9, 5); }
[[nodiscard]] uint64_t ldrByteOffset(uint32_t word) noexcept {
  return uint64_t{field(word, 21, 10)} * 8;
}

struct StubDecode {
  std::optional<uint64_t> gotSlot;
  std::optional<uint64_t> objcSelectorSlot;
};

[[nodiscard]] StubDecode decodeStub(const ByteReader& reader, uint64_t entryVa) {
  std::vector<uint64_t> visited;
  visited.reserve(4);
  uint64_t stubVa = entryVa;
  std::optional<uint64_t> selectorSlot;

  for (unsigned hop = 0; hop < kMaxBranchIslandHops; ++hop) {
    if (std::find(visited.begin(), visited.end(), stubVa) != visited.end()) {
      return {};
    }
    visited.push_back(stubVa);

    const std::optional<uint32_t> w0 = readInsn(reader, stubVa);
    if (!w0.has_value()) {
      return {};
    }
    if (isUncondBranch(*w0)) {
      stubVa = branchTarget(stubVa, *w0);
      continue;
    }

    const std::optional<uint32_t> w1 = readInsn(reader, stubVa + 4);
    if (!w1.has_value() || !isAdrp(*w0)) {
      return {};
    }

    const uint64_t page =
        (stubVa & ~uint64_t{0xfff}) + static_cast<uint64_t>(adrpPageOffset(*w0));
    const uint32_t adrpReg = adrpDestReg(*w0);

    if (isLdrUnsignedOffset64(*w1) && ldrBaseReg(*w1) == adrpReg) {
      const uint64_t slot = page + ldrByteOffset(*w1);
      // Apple `__objc_stubs`: `adrp x1, page; ldr x1, [x1, #selref]; b _objc_msgSend`.
      // Note that unit tests for two-word stubs only map 8 bytes, so failure to
      // read `stubVa + 8` simply means a standard two-word stub.
      if (adrpReg == 1 && ldrDestReg(*w1) == 1) {
        if (const std::optional<uint32_t> w2 = readInsn(reader, stubVa + 8);
            w2.has_value() && isUncondBranch(*w2)) {
          if (!selectorSlot.has_value()) {
            selectorSlot = slot;
          }
          stubVa = branchTarget(stubVa + 8, *w2);
          continue;
        }
      }
      return StubDecode{slot, selectorSlot};
    }

    // arm64e stub shape: `adrp x17, page; add x17, x17, #off; ldr x16, [x17]; braa x16, x17`.
    if (isAddImm64(*w1) && addBaseReg(*w1) == adrpReg && addDestReg(*w1) == adrpReg) {
      if (const std::optional<uint32_t> w2 = readInsn(reader, stubVa + 8);
          w2.has_value() && isLdrUnsignedOffset64(*w2) && ldrBaseReg(*w2) == adrpReg &&
          ldrByteOffset(*w2) == 0) {
        return StubDecode{page + addImm64(*w1), selectorSlot};
      }
    }

    return {};
  }
  return {};
}

}  // namespace

std::optional<uint64_t> pltGotSlot(const ByteReader& reader, uint64_t stubVa) {
  return decodeStub(reader, stubVa).gotSlot;
}

std::optional<uint64_t> objcStubSelectorSlot(const ByteReader& reader, uint64_t stubVa) {
  return decodeStub(reader, stubVa).objcSelectorSlot;
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
