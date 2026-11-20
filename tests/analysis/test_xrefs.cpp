#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <vector>

#include "xdec/analysis/xrefs.h"

using namespace xdec::analysis;

namespace {

[[nodiscard]] constexpr uint32_t adrp(unsigned rd, uint64_t pc, uint64_t targetPage) {
  const int64_t imm = static_cast<int64_t>(targetPage - (pc & ~uint64_t{0xfff})) >> 12;
  const uint32_t immlo = static_cast<uint32_t>(imm & 0x3);
  const uint32_t immhi = static_cast<uint32_t>((imm >> 2) & 0x7ffff);
  return 0x90000000U | (immlo << 29) | (immhi << 5) | rd;
}

[[nodiscard]] constexpr uint32_t addImm64(unsigned rd, unsigned rn, uint32_t imm12) {
  return 0x91000000U | ((imm12 & 0xfffU) << 10U) | (rn << 5U) | rd;
}

[[nodiscard]] constexpr uint32_t ldr64(unsigned rt, unsigned rn, uint64_t byteOff) {
  return 0xf9400000U | (static_cast<uint32_t>(byteOff / 8) << 10U) | (rn << 5U) | rt;
}

[[nodiscard]] constexpr uint32_t bl(uint64_t pc, uint64_t target) {
  const int64_t delta = static_cast<int64_t>(target - pc) >> 2;
  return 0x94000000U | (static_cast<uint32_t>(delta) & 0x03ffffffU);
}

[[nodiscard]] constexpr uint32_t b(uint64_t pc, uint64_t target) {
  const int64_t delta = static_cast<int64_t>(target - pc) >> 2;
  return 0x14000000U | (static_cast<uint32_t>(delta) & 0x03ffffffU);
}

void putWord(std::vector<std::byte>& buf, std::size_t off, uint32_t w) {
  if (off + 4 > buf.size()) {
    buf.resize(off + 4, std::byte{0});
  }
  std::memcpy(buf.data() + off, &w, 4);
}

}  // namespace

TEST_CASE("scanCStringSections extracts printable NUL-terminated strings with filtering",
          "[analysis][xrefs]") {
  const char raw[] = "\x01\x02hi\0WePkg\0A8KeyResultCookieWriter\0\xff\xfeok\0";
  std::vector<std::byte> bytes(sizeof(raw));
  std::memcpy(bytes.data(), raw, sizeof(raw));

  const std::array<StringSectionSpan, 1> sections{
      StringSectionSpan{0x10000, "__cstring", bytes},
  };

  StringScanOptions opts;
  opts.minLength = 4;
  opts.filter = "wepkg";  // case-insensitive
  const auto hits = scanCStringSections(sections, opts);
  REQUIRE(hits.size() == 1);
  CHECK(hits[0].va == 0x10005);
  CHECK(hits[0].section == "__cstring");
  CHECK(hits[0].text == "WePkg");
}

TEST_CASE("scanAarch64Xrefs finds direct calls, ADRP+ADD/LDR refs, and branch-island stubs",
          "[analysis][xrefs]") {
  // Layout:
  //   0x1000..0x1020: caller code
  //   0x2000: branch island `b 0x3000`
  //   0x3000: stub `adrp x16, 0x8000; ldr x16, [x16, #0x10]` -> GOT slot 0x8010
  //   0x4000: __objc_stubs `adrp x1, 0x9000; ldr x1, [x1, #0x20]; b 0x3000` -> selref 0x9020
  std::map<uint64_t, uint32_t> words;
  words[0x1000] = adrp(0, 0x1000, 0x7000);
  words[0x1004] = addImm64(0, 0, 0x123);       // -> 0x7123 (Address)
  words[0x1008] = adrp(8, 0x1008, 0x9000);
  words[0x100c] = ldr64(1, 8, 0x20);           // -> 0x9020 (DataRead)
  words[0x1010] = bl(0x1010, 0x2000);          // -> GOT 0x8010 via branch island
  words[0x1014] = bl(0x1014, 0x4000);          // -> GOT 0x8010 & selref 0x9020 via __objc_stubs

  words[0x2000] = b(0x2000, 0x3000);
  words[0x3000] = adrp(16, 0x3000, 0x8000);
  words[0x3004] = ldr64(16, 16, 0x10);
  words[0x3008] = 0xd61f0200U;                 // br x16
  words[0x4000] = adrp(1, 0x4000, 0x9000);
  words[0x4004] = ldr64(1, 1, 0x20);
  words[0x4008] = b(0x4008, 0x3000);

  std::vector<std::byte> callerBytes;
  for (uint64_t va = 0x1000; va <= 0x1014; va += 4) {
    putWord(callerBytes, static_cast<std::size_t>(va - 0x1000), words[va]);
  }
  const std::array<CodeSpan, 1> code{CodeSpan{0x1000, callerBytes}};

  const xdec::ByteReader reader = [&](uint64_t va, std::span<std::byte> out) -> xdec::Result<void> {
    for (std::size_t i = 0; i < out.size(); ++i) {
      const uint64_t wordVa = (va + i) & ~uint64_t{3};
      const auto it = words.find(wordVa);
      if (it == words.end()) {
        return xdec::err(xdec::DiagCode::UnmappedAddress, "unmapped");
      }
      const unsigned shift = static_cast<unsigned>(((va + i) & 3u) * 8u);
      out[i] = static_cast<std::byte>((it->second >> shift) & 0xffu);
    }
    return xdec::ok();
  };

  const std::array<uint64_t, 3> targets{0x7123, 0x8010, 0x9020};
  const auto hits = scanAarch64Xrefs(code, targets, reader);

  REQUIRE(hits.size() == 5);
  CHECK(hits[0].insnVa == 0x1004);
  CHECK(hits[0].targetVa == 0x7123);
  CHECK(hits[0].kind == XrefKind::Address);

  CHECK(hits[1].insnVa == 0x100c);
  CHECK(hits[1].targetVa == 0x9020);
  CHECK(hits[1].kind == XrefKind::DataRead);

  CHECK(hits[2].insnVa == 0x1010);
  CHECK(hits[2].targetVa == 0x8010);
  CHECK(hits[2].viaStubVa == 0x2000);
  CHECK(hits[2].kind == XrefKind::Call);

  CHECK(hits[3].insnVa == 0x1014);
  CHECK(hits[3].targetVa == 0x8010);
  CHECK(hits[3].viaStubVa == 0x4000);

  CHECK(hits[4].insnVa == 0x1014);
  CHECK(hits[4].targetVa == 0x9020);
  CHECK(hits[4].viaStubVa == 0x4000);
}

TEST_CASE("scanAarch64Xrefs follows reverse branch islands to target function",
          "[analysis][xrefs]") {
  // 0x1000: bl 0x2000 (branch island `b 0x5000`) -> reverse xref to 0x5000!
  std::map<uint64_t, uint32_t> words;
  words[0x1000] = bl(0x1000, 0x2000);
  words[0x2000] = b(0x2000, 0x5000);

  std::vector<std::byte> callerBytes;
  putWord(callerBytes, 0, words[0x1000]);
  const std::array<CodeSpan, 1> code{CodeSpan{0x1000, callerBytes}};

  const xdec::ByteReader reader = [&](uint64_t va, std::span<std::byte> out) -> xdec::Result<void> {
    for (std::size_t i = 0; i < out.size(); ++i) {
      const auto it = words.find((va + i) & ~uint64_t{3});
      if (it == words.end()) {
        return xdec::err(xdec::DiagCode::UnmappedAddress, "unmapped");
      }
      out[i] = static_cast<std::byte>((it->second >> (((va + i) & 3u) * 8u)) & 0xffu);
    }
    return xdec::ok();
  };

  const std::array<uint64_t, 1> targets{0x5000};
  const auto hits = scanAarch64Xrefs(code, targets, reader);
  REQUIRE(hits.size() == 1);
  CHECK(hits[0].insnVa == 0x1000);
  CHECK(hits[0].targetVa == 0x5000);
  CHECK(hits[0].viaStubVa == 0x2000);
  CHECK(hits[0].kind == XrefKind::Call);
}
