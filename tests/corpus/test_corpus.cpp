// Corpus replay: every tests/corpus/*.case file is a frozen bug report.
//
// A .case is self-contained — instruction words, an initial state, and the
// expected outcome — so replaying needs no binary and no emulator. The format
// is exactly what tools/diff_unicorn.py writes for a mismatch:
//
//   base 0xVA                      where the words live
//   words 0xW 0xW ...              the basic block's instructions
//   reg x0 0x...                   initial register (up to 128 bits)
//   memfill 0xADDR 0xSIZE 0xSEED   splitmix64-filled region
//   mem 0xADDR deadbeef            explicit bytes
//   expect reg x0 0x...            final register
//   expect mem 0xADDR deadbeef     final memory bytes
//   expect pc 0x...                where control went
//   expect fault 0x...             an unmapped access at this instruction
//
// Expected values come from the oracle (Unicorn, i.e. the hardware model), so
// a failing case is a semantic bug in the spec by definition. New cases land
// here before they are fixed; the suite failing is the reminder to fix them.

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "xdec/il/interp.h"
#include "xdec/spec/engine.h"
#include "xdec/spec/lift.h"
#include "xdec/spec/compile.h"
#include "xdec/support/prng.h"

#include "../spec/spec_test_support.h"

namespace {

const xdec::spec::SpecEngine& engine() {
  static const std::unique_ptr<xdec::spec::SpecEngine> kEngine = [] {
    auto loaded = xdec::spec::loadSpecFile(xdec::spec::testing::arm64SpecPath());
    if (!loaded) {
      FAIL(loaded.error().format());
    }
    return std::move(loaded).value();
  }();
  return *kEngine;
}

[[nodiscard]] bool parseHex(std::string_view text, uint64_t& value) {
  if (text.starts_with("0x")) {
    text.remove_prefix(2);
  }
  if (text.empty()) {
    return false;
  }
  value = 0;
  for (const char digit : text) {
    value <<= 4;
    if (digit >= '0' && digit <= '9') {
      value |= static_cast<uint64_t>(digit - '0');
    } else if (digit >= 'a' && digit <= 'f') {
      value |= static_cast<uint64_t>(digit - 'a' + 10);
    } else if (digit >= 'A' && digit <= 'F') {
      value |= static_cast<uint64_t>(digit - 'A' + 10);
    } else {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool parseWide(std::string_view text, xdec::il::ConcreteValue& value) {
  if (text.starts_with("0x")) {
    text.remove_prefix(2);
  }
  if (text.empty() || text.size() > 32) {
    return false;
  }
  value = {0, 0};
  const std::size_t loDigits = std::min<std::size_t>(16, text.size());
  return parseHex(text.substr(text.size() - loDigits), value.lo) &&
         (text.size() <= 16 || parseHex(text.substr(0, text.size() - 16), value.hi));
}

[[nodiscard]] bool parseBytes(std::string_view text, std::vector<std::byte>& out) {
  if (text.starts_with("0x")) {
    text.remove_prefix(2);
  }
  if (text.empty() || text.size() % 2 != 0) {
    return false;
  }
  out.clear();
  for (std::size_t index = 0; index < text.size(); index += 2) {
    uint64_t byte = 0;
    if (!parseHex(text.substr(index, 2), byte)) {
      return false;
    }
    out.push_back(static_cast<std::byte>(byte));
  }
  return true;
}

struct CorpusCase {
  uint64_t base = 0;
  std::vector<uint32_t> words;
  std::vector<std::pair<std::string, xdec::il::ConcreteValue>> regs;
  std::vector<std::array<uint64_t, 3>> fills;
  std::vector<std::pair<uint64_t, std::vector<std::byte>>> mems;
  std::vector<std::pair<std::string, xdec::il::ConcreteValue>> expectRegs;
  std::vector<std::pair<uint64_t, std::vector<std::byte>>> expectMems;
  std::optional<uint64_t> expectPc;
  std::optional<uint64_t> expectFault;
};

[[nodiscard]] bool loadCase(const std::filesystem::path& path, CorpusCase& out,
                            std::string& error) {
  std::ifstream file{path};
  if (!file) {
    error = "cannot open";
    return false;
  }
  std::string line;
  uint64_t lineNumber = 0;
  while (std::getline(file, line)) {
    ++lineNumber;
    if (const auto hash = line.find('#'); hash != std::string::npos) {
      line.resize(hash);
    }
    std::istringstream tokens{line};
    std::string keyword;
    if (!(tokens >> keyword)) {
      continue;
    }
    const auto fail = [&](std::string_view what) {
      error = "line " + std::to_string(lineNumber) + ": " + std::string{what};
      return false;
    };
    if (keyword == "base") {
      std::string text;
      if (!(tokens >> text) || !parseHex(text, out.base)) {
        return fail("bad base");
      }
    } else if (keyword == "words") {
      std::string text;
      while (tokens >> text) {
        uint64_t word = 0;
        if (!parseHex(text, word)) {
          return fail("bad word");
        }
        out.words.push_back(static_cast<uint32_t>(word));
      }
      if (out.words.empty()) {
        return fail("empty words");
      }
    } else if (keyword == "reg" || keyword == "expect") {
      std::string which;
      std::string name;
      std::string text;
      if (keyword == "expect" && !(tokens >> which)) {
        return fail("bad expect");
      }
      if (keyword == "expect" && which == "pc") {
        uint64_t value = 0;
        if (!(tokens >> text) || !parseHex(text, value)) {
          return fail("bad expect pc");
        }
        out.expectPc = value;
        continue;
      }
      if (keyword == "expect" && which == "fault") {
        uint64_t value = 0;
        if (!(tokens >> text) || !parseHex(text, value)) {
          return fail("bad expect fault");
        }
        out.expectFault = value;
        continue;
      }
      if (keyword == "expect" && which != "reg" && which != "mem") {
        return fail("unknown expect");
      }
      if (!(tokens >> name >> text)) {
        return fail("bad reg/mem line");
      }
      if ((keyword == "reg") || which == "reg") {
        xdec::il::ConcreteValue value{};
        if (!parseWide(text, value)) {
          return fail("bad register value");
        }
        (keyword == "reg" ? out.regs : out.expectRegs).emplace_back(name, value);
      } else {
        uint64_t address = 0;
        std::vector<std::byte> bytes;
        if (!parseHex(name, address) || !parseBytes(text, bytes)) {
          return fail("bad mem line");
        }
        out.expectMems.emplace_back(address, std::move(bytes));
      }
    } else if (keyword == "memfill") {
      std::string a;
      std::string s;
      std::string seed;
      std::array<uint64_t, 3> fill{};
      if (!(tokens >> a >> s >> seed) || !parseHex(a, fill[0]) || !parseHex(s, fill[1]) ||
          !parseHex(seed, fill[2])) {
        return fail("bad memfill");
      }
      out.fills.push_back(fill);
    } else if (keyword == "mem") {
      std::string a;
      std::string text;
      uint64_t address = 0;
      std::vector<std::byte> bytes;
      if (!(tokens >> a >> text) || !parseHex(a, address) || !parseBytes(text, bytes)) {
        return fail("bad mem");
      }
      out.mems.emplace_back(address, std::move(bytes));
    } else {
      return fail("unknown directive '" + keyword + "'");
    }
  }
  if (out.words.empty()) {
    error = "no words";
    return false;
  }
  return true;
}

void runCase(const CorpusCase& test) {
  const xdec::spec::SpecEngine& eng = engine();
  std::vector<std::byte> bytes(test.words.size() * 4);
  for (std::size_t index = 0; index < test.words.size(); ++index) {
    const uint32_t word = test.words[index];
    for (unsigned byte = 0; byte < 4; ++byte) {
      bytes[index * 4 + byte] = static_cast<std::byte>(word >> (byte * 8));
    }
  }
  auto lifted = xdec::spec::liftBasicBlock(eng, bytes, test.base);
  REQUIRE(lifted);

  xdec::il::Interpreter interp{*lifted->function};
  for (const auto& [address, size, seed] : test.fills) {
    std::vector<std::byte> contents(size);
    uint64_t state = seed;
    for (uint64_t offset = 0; offset < size; offset += 8) {
      const uint64_t word = xdec::splitmix64Next(state);
      const uint64_t chunk = std::min<uint64_t>(8, size - offset);
      std::memcpy(contents.data() + offset, &word, chunk);
    }
    interp.memory().fillDelta(address, contents);
  }
  for (const auto& [address, data] : test.mems) {
    interp.memory().fillDelta(address, data);
  }
  const xdec::il::RegisterFile& registers = eng.program().registers;
  for (const auto& [name, value] : test.regs) {
    const xdec::il::RegId reg = registers.find(name);
    REQUIRE(reg.valid());
    interp.writeRegister(reg, value);
  }

  const xdec::il::ExecOutcome outcome = interp.runBlock(lifted->block);

  if (test.expectFault) {
    CHECK(outcome.stop == xdec::il::ExecStop::Error);
    CHECK(outcome.va == *test.expectFault);
    return;  // state after a fault is not pinned down by the case
  }
  REQUIRE(outcome.stop != xdec::il::ExecStop::Error);
  for (const auto& [name, expected] : test.expectRegs) {
    const xdec::il::RegId reg = registers.find(name);
    REQUIRE(reg.valid());
    CHECK(interp.readRegister(reg) == expected);
  }
  for (const auto& [address, expected] : test.expectMems) {
    std::vector<std::byte> actual(expected.size());
    std::size_t offset = 0;
    while (offset < expected.size()) {
      const unsigned chunk = static_cast<unsigned>(std::min<size_t>(16, expected.size() - offset));
      auto read = interp.memory().read(address + offset, chunk);
      REQUIRE(read);
      std::memcpy(actual.data() + offset, &read->lo, std::min<unsigned>(chunk, 8));
      if (chunk > 8) {
        std::memcpy(actual.data() + offset + 8, &read->hi, chunk - 8);
      }
      offset += chunk;
    }
    CHECK(actual == expected);
  }
  if (test.expectPc) {
    uint64_t actual = 0;
    switch (outcome.stop) {
      case xdec::il::ExecStop::Branch:
      case xdec::il::ExecStop::IndirectBranch:
      case xdec::il::ExecStop::Call:
        actual = outcome.target;
        break;
      case xdec::il::ExecStop::CondBranch:
        actual = outcome.condition ? outcome.target : outcome.fallthrough;
        break;
      default:
        FAIL("case expects pc but the block ended with " << toString(outcome.stop));
    }
    CHECK(actual == *test.expectPc);
  }
}

}  // namespace

TEST_CASE("corpus replay", "[corpus]") {
  const std::filesystem::path corpusDir =
      xdec::spec::testing::specDir().parent_path() / "corpus";
  if (!std::filesystem::is_directory(corpusDir)) {
    SKIP("no corpus directory");
  }
  std::vector<std::filesystem::path> cases;
  for (const auto& entry : std::filesystem::directory_iterator(corpusDir)) {
    if (entry.path().extension() == ".case") {
      cases.push_back(entry.path());
    }
  }
  std::sort(cases.begin(), cases.end());
  for (const auto& path : cases) {
    SECTION(path.filename().string()) {
      CorpusCase test;
      std::string error;
      INFO("case file: " + path.string());
      INFO("parse error: " + error);
      REQUIRE(loadCase(path, test, error));
      runCase(test);
    }
  }
}
