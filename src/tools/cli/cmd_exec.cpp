// exec: batch concrete execution of basic blocks against scripted states
// ---------------------------------------------------------------------------
//
// The workload file is a tiny line language, built for the differential
// driver:
//
//   block 0xVA COUNT        lift COUNT instructions at VA as one basic block
//   reg x0 0x112233...      register value for the next run (128 bits allowed)
//   mem 0xADDR deadbeef     explicit memory bytes for the next run
//   memfill 0xADDR 0xSIZE 0xSEED   splitmix64-filled region for the next run
//   run                     execute, print state, reset for the next run
//
// `memfill` uses splitmix64 because the other side of the differential (the
// Python Unicorn driver) generates the identical byte stream from the same
// seed; both sides then see the same "random" stack and scratch contents
// without shipping megabytes through the workload file.
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "common.h"
#include "session.h"
#include "xdec/il/interp.h"
#include "xdec/spec/engine.h"
#include "xdec/spec/lift.h"
#include "xdec/support/prng.h"

namespace xdec::cli {

bool parseHexBytes(std::string_view text, std::vector<std::byte>& out) {
  if (text.starts_with("0x")) {
    text.remove_prefix(2);
  }
  if (text.empty() || text.size() % 2 != 0) {
    return false;
  }
  out.clear();
  out.reserve(text.size() / 2);
  for (std::size_t index = 0; index < text.size(); index += 2) {
    uint64_t byte = 0;
    const auto result = std::from_chars(text.data() + index, text.data() + index + 2, byte, 16);
    if (result.ec != std::errc{} || result.ptr != text.data() + index + 2) {
      return false;
    }
    out.push_back(static_cast<std::byte>(byte));
  }
  return true;
}

/// Parses up to 128 bits of hexadecimal into (lo, hi).
bool parseHexWide(std::string_view text, uint64_t& lo, uint64_t& hi) {
  if (text.starts_with("0x")) {
    text.remove_prefix(2);
  }
  if (text.empty() || text.size() > 32) {
    return false;
  }
  lo = 0;
  hi = 0;
  if (text.size() > 16) {
    const std::size_t hiDigits = text.size() - 16;
    if (!parseNumber("0x" + std::string{text.substr(0, hiDigits)}, hi) ||
        !parseNumber("0x" + std::string{text.substr(hiDigits)}, lo)) {
      return false;
    }
    return true;
  }
  return parseNumber("0x" + std::string{text}, lo);
}

std::string hexOf(std::span<const std::byte> bytes) {
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const std::byte byte : bytes) {
    out += std::format("{:02x}", static_cast<unsigned>(byte));
  }
  return out;
}

int commandExec(std::string_view path, std::string_view workloadPath) {
  auto session = ToolSession::openBinary(path);
  if (!session) {
    return reportError(session.error());
  }
  const BinaryImage& image = *session->image;
  const xdec::spec::SpecEngine& engine = *session->engine;
  const xdec::il::RegisterFile& registers = engine.program().registers;

  std::ifstream workload{std::filesystem::path{workloadPath}};
  if (!workload) {
    print("error: cannot open workload '{}'", workloadPath);
    return 1;
  }

  // The image is seeded once and shared by every run; per-run state lives in
  // the delta layer and is cleared between runs.
  xdec::il::ExecMemory sharedMemory;
  for (const xdec::binary::MemoryRegion& region : image.memory().regions()) {
    if (region.size == 0) {
      continue;
    }
    std::vector<std::byte> contents(region.size);
    if (auto read = image.read(region.va, contents); read) {
      sharedMemory.seed(region.va, contents);
    }
  }

  // Pending state for the next run.
  uint64_t blockVa = 0;
  uint64_t blockCount = 0;
  bool haveBlock = false;
  std::unique_ptr<xdec::spec::LiftedBlock> lifted;
  std::vector<std::pair<xdec::il::RegId, xdec::il::ConcreteValue>> pendingRegs;
  std::vector<std::pair<uint64_t, std::vector<std::byte>>> pendingMems;
  std::vector<std::array<uint64_t, 3>> pendingFills;
  uint64_t runNumber = 0;
  uint64_t failures = 0;

  const auto fail = [&](std::string_view message) {
    ++failures;
    print("error: {}", message);
  };

  std::string line;
  uint64_t lineNumber = 0;
  while (std::getline(workload, line)) {
    ++lineNumber;
    if (const auto hash = line.find('#'); hash != std::string::npos) {
      line.resize(hash);
    }
    std::istringstream tokens{line};
    std::string keyword;
    if (!(tokens >> keyword)) {
      continue;
    }

    if (keyword == "block") {
      std::string vaText;
      if (!(tokens >> vaText >> blockCount) || !parseNumber(vaText, blockVa) ||
          blockCount == 0) {
        fail(std::format("line {}: malformed block directive", lineNumber));
        continue;
      }
      haveBlock = true;
      lifted.reset();
      continue;
    }
    if (keyword == "reg") {
      std::string name;
      std::string valueText;
      if (!(tokens >> name >> valueText)) {
        fail(std::format("line {}: malformed reg directive", lineNumber));
        continue;
      }
      const xdec::il::RegId reg = registers.find(name);
      if (!reg.valid()) {
        fail(std::format("line {}: unknown register '{}'", lineNumber, name));
        continue;
      }
      uint64_t lo = 0;
      uint64_t hi = 0;
      if (!parseHexWide(valueText, lo, hi)) {
        fail(std::format("line {}: bad register value '{}'", lineNumber, valueText));
        continue;
      }
      pendingRegs.emplace_back(reg, xdec::il::ConcreteValue{lo, hi});
      continue;
    }
    if (keyword == "mem") {
      std::string addressText;
      std::string hexText;
      uint64_t address = 0;
      std::vector<std::byte> bytes;
      if (!(tokens >> addressText >> hexText) || !parseNumber(addressText, address) ||
          !parseHexBytes(hexText, bytes)) {
        fail(std::format("line {}: malformed mem directive", lineNumber));
        continue;
      }
      pendingMems.emplace_back(address, std::move(bytes));
      continue;
    }
    if (keyword == "memfill") {
      std::string addressText;
      std::string sizeText;
      std::string seedText;
      uint64_t address = 0;
      uint64_t size = 0;
      uint64_t seed = 0;
      if (!(tokens >> addressText >> sizeText >> seedText) ||
          !parseNumber(addressText, address) || !parseNumber(sizeText, size) ||
          !parseNumber(seedText, seed) || size == 0) {
        fail(std::format("line {}: malformed memfill directive", lineNumber));
        continue;
      }
      pendingFills.push_back({address, size, seed});
      continue;
    }
    if (keyword != "run") {
      fail(std::format("line {}: unknown directive '{}'", lineNumber, keyword));
      continue;
    }

    // -- run ----------------------------------------------------------------
    ++runNumber;
    if (!haveBlock) {
      fail(std::format("line {}: run without a block directive", lineNumber));
      continue;
    }
    if (!lifted) {
      std::vector<std::byte> code(blockCount * (engine.program().insnWidth / 8));
      if (auto read = image.read(blockVa, code); !read) {
        fail(std::format("run {}: cannot read block bytes: {}", runNumber,
                         read.error().format()));
        continue;
      }
      auto liftedOrError = xdec::spec::liftBasicBlock(engine, code, blockVa);
      if (!liftedOrError) {
        fail(std::format("run {}: lift failed: {}", runNumber,
                         liftedOrError.error().format()));
        continue;
      }
      lifted = std::make_unique<xdec::spec::LiftedBlock>(std::move(liftedOrError).value());
    }

    xdec::il::Interpreter interp{*lifted->function, &sharedMemory};
    sharedMemory.clearDelta();
    for (const auto& [reg, value] : pendingRegs) {
      interp.writeRegister(reg, value);
    }
    for (const auto& [address, bytes] : pendingMems) {
      sharedMemory.fillDelta(address, bytes);
    }
    for (const auto& [address, size, seed] : pendingFills) {
      std::vector<std::byte> contents(size);
      uint64_t state = seed;
      for (uint64_t offset = 0; offset < size; offset += 8) {
        const uint64_t word = xdec::splitmix64Next(state);
        const uint64_t chunk = std::min<uint64_t>(8, size - offset);
        std::memcpy(contents.data() + offset, &word, chunk);
      }
      sharedMemory.fillDelta(address, contents);
    }
    pendingRegs.clear();
    pendingMems.clear();
    pendingFills.clear();

    const xdec::il::ExecOutcome outcome = interp.runBlock(lifted->block);

    print("run {} block 0x{:x} 0x{:x}", runNumber, blockVa,
          blockVa + blockCount * (engine.program().insnWidth / 8));
    switch (outcome.stop) {
      case xdec::il::ExecStop::Branch:
        print("flow branch 0x{:x}", outcome.target);
        break;
      case xdec::il::ExecStop::CondBranch:
        print("flow cond {} 0x{:x}", outcome.condition ? "taken" : "fall", outcome.target);
        break;
      case xdec::il::ExecStop::IndirectBranch:
        print("flow indirect 0x{:x}", outcome.target);
        break;
      case xdec::il::ExecStop::Call:
        print("flow call 0x{:x}", outcome.target);
        break;
      case xdec::il::ExecStop::Return:
        printLine("flow return");
        break;
      case xdec::il::ExecStop::Unreachable:
        printLine("flow unreachable");
        break;
      case xdec::il::ExecStop::Unimplemented:
        print("flow unimplemented {}", outcome.detail);
        break;
      case xdec::il::ExecStop::Intrinsic:
        print("flow intrinsic {}", outcome.detail);
        break;
      case xdec::il::ExecStop::Error:
        print("flow error 0x{:x} {}", outcome.va, outcome.detail);
        break;
    }
    for (std::size_t index = 0; index < registers.size(); ++index) {
      const xdec::il::RegisterInfo& info = registers.all()[index];
      if (info.isSubRegister() || info.regClass == xdec::il::RegClass::Zero) {
        continue;
      }
      const xdec::il::ConcreteValue value =
          interp.readRegister(xdec::il::RegId{static_cast<uint32_t>(index)});
      if (info.bits > 64) {
        print("reg {} 0x{:016x}{:016x}", info.name, value.hi, value.lo);
      } else {
        print("reg {} 0x{:x}", info.name, value.lo);
      }
    }
    for (const xdec::il::WrittenRange& range : sharedMemory.writtenRanges()) {
      std::vector<std::byte> bytes(range.size);
      uint64_t offset = 0;
      while (offset < range.size) {
        const unsigned chunk =
            static_cast<unsigned>(std::min<uint64_t>(16, range.size - offset));
        auto contents = sharedMemory.read(range.address + offset, chunk);
        if (!contents) {
          break;
        }
        std::memcpy(bytes.data() + offset, &contents->lo, std::min<unsigned>(chunk, 8));
        if (chunk > 8) {
          std::memcpy(bytes.data() + offset + 8, &contents->hi, chunk - 8);
        }
        offset += chunk;
      }
      print("mem 0x{:x} 0x{:x} {}", range.address, range.size, hexOf(bytes));
    }
    printLine("end");
  }

  if (failures != 0) {
    print("error: {} workload directive(s) failed", failures);
    return 1;
  }
  return 0;
}

}  // namespace xdec::cli
