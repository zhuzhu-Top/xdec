// spec, coverage, decode: commands built directly on the architecture spec
// engine, with (coverage) or without (spec, decode) a binary image.
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "common.h"
#include "session.h"
#include "xdec/binary/image.h"
#include "xdec/spec/check.h"
#include "xdec/spec/compile.h"
#include "xdec/spec/engine.h"
#include "xdec/spec/parse.h"

namespace xdec::cli {

/// Compiles a spec far enough to say whether it is well formed, and reports how
/// well the resulting decoder discriminates. A shallow tree with a large worst
/// leaf means the encodings overlap more than they should.
int commandSpec(std::string_view path, std::string_view output) {
  auto parsed = xdec::spec::parseSpecFile(std::filesystem::path{path});
  if (!parsed) {
    return reportError(parsed.error());
  }
  const xdec::spec::Module& module = *parsed.value();
  xdec::spec::CheckResult checked = xdec::spec::check(module);

  for (const xdec::Diag& diag : checked.report.errors) {
    print("error: {}", diag.format());
  }
  for (const xdec::Diag& diag : checked.report.warnings) {
    print("warning: {}", diag.format());
  }

  print("arch        : {} {}-bit {}-endian, {}-bit instructions", module.arch.name,
        module.arch.pointerBits, toString(module.arch.endian), module.arch.insnWidth);
  print("registers   : {}", checked.module->registers.size());
  print("functions   : {}", module.functions.size());
  print("instructions: {}", module.instructions.size());
  print("decoder     : {} nodes, depth {}, worst leaf {}", checked.module->decoder.nodeCount(),
        checked.module->decoder.depth(), checked.module->decoder.worstLeaf());

  if (!checked.report.ok()) {
    print("{} error(s)", checked.report.errors.size());
    return 1;
  }

  if (!output.empty()) {
    auto program = xdec::spec::compile(module, *checked.module);
    if (!program) {
      return reportError(program.error());
    }
    const std::vector<std::byte> blob = xdec::spec::serialize(*program.value());
    std::ofstream file{std::filesystem::path{output}, std::ios::binary};
    file.write(reinterpret_cast<const char*>(blob.data()),
               static_cast<std::streamsize>(blob.size()));
    if (!file) {
      print("error: could not write '{}'", output);
      return 1;
    }
    print("blob        : {} bytes -> {}", blob.size(), output);
  }

  printLine("ok");
  return 0;
}

/// Decodes every executable word in the image and reports what the spec does
/// not cover, grouped by the top bits that select an encoding group.
///
/// This is the progress metric for the architecture spec. Counting mnemonics in
/// a manual is not: the distribution is so skewed that the first ten encoding
/// groups are worth more than the next hundred.
int commandCoverage(std::string_view path, uint64_t limit) {
  auto session = ToolSession::openBinary(path);
  if (!session) {
    return reportError(session.error());
  }

  const BinaryImage& image = *session->image;
  const xdec::spec::SpecEngine& engine = *session->engine;
  const unsigned width = engine.program().insnWidth / 8;

  uint64_t total = 0;
  uint64_t covered = 0;
  std::map<std::string, uint64_t> byRule;
  // Bits 28..25 are the top-level encoding group in AArch64, so grouping the
  // failures this way points at which section of the manual is missing rather
  // than at individual words.
  std::map<uint32_t, uint64_t> missingGroup;
  std::map<uint32_t, uint32_t> missingExample;

  // Sections rather than segments: the executable segment of a shared object
  // also holds .rodata, and counting string literals as undecoded instructions
  // makes the number meaningless.
  for (const xdec::binary::Section& section : image.sections()) {
    const bool executable =
        (section.permissions & xdec::binary::MemoryPermissions::Execute) !=
        xdec::binary::MemoryPermissions::None;
    if (!executable || section.zeroFilled || section.size == 0) {
      continue;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(section.size));
    if (auto read = image.read(section.va, bytes); !read) {
      continue;
    }
    for (std::size_t offset = 0; offset + width <= bytes.size(); offset += width) {
      const uint64_t va = section.va + offset;
      const auto insn = engine.decode(std::span{bytes}.subspan(offset, width), va);
      ++total;
      if (insn.valid) {
        ++covered;
        ++byRule[engine.program().instructions[insn.instruction].name];
      } else {
        const auto word = static_cast<uint32_t>(insn.word);
        const uint32_t group = (word >> 25) & 0xF;
        ++missingGroup[group];
        missingExample.emplace(group, word);
      }
    }
  }

  if (total == 0) {
    printLine("no executable words found");
    return 1;
  }
  print("coverage: {} of {} words ({:.2f}%)", covered, total,
        100.0 * static_cast<double>(covered) / static_cast<double>(total));

  std::vector<std::pair<uint32_t, uint64_t>> groups{missingGroup.begin(), missingGroup.end()};
  std::sort(groups.begin(), groups.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  if (!groups.empty()) {
    printLine("");
    printLine("uncovered by encoding group (bits 28..25):");
    for (std::size_t index = 0; index < groups.size() && index < limit; ++index) {
      const auto [group, count] = groups[index];
      print("  op0={:04b}  {:>8} words {:5.2f}%  e.g. {:08x}", group, count,
            100.0 * static_cast<double>(count) / static_cast<double>(total),
            missingExample[group]);
    }
  }

  std::vector<std::pair<std::string, uint64_t>> rules{byRule.begin(), byRule.end()};
  std::sort(rules.begin(), rules.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  if (!rules.empty()) {
    printLine("");
    printLine("covered by rule:");
    for (std::size_t index = 0; index < rules.size() && index < limit; ++index) {
      print("  {:<26} {:>8} {:5.2f}%", rules[index].first, rules[index].second,
            100.0 * static_cast<double>(rules[index].second) / static_cast<double>(total));
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// decode: raw instruction words from stdin, no binary needed
// ---------------------------------------------------------------------------
//
// The fuzzer's interface: one hex word per line on stdin, one line per word on
// stdout. `0xD503201F  nop`-style, or `undecodable` when the spec has no rule.
// Keeping this text-based means tools/fuzz_decode.py never has to parse IL.

int commandDecode() {
  auto engineOrError = loadEngine();
  if (!engineOrError) {
    return reportError(engineOrError.error());
  }
  const xdec::spec::SpecEngine& engine = *engineOrError.value();
  const unsigned width = engine.program().insnWidth / 8;

  std::string line;
  uint64_t failures = 0;
  while (std::getline(std::cin, line)) {
    if (const auto hash = line.find('#'); hash != std::string::npos) {
      line.resize(hash);
    }
    std::istringstream tokens{line};
    std::string text;
    if (!(tokens >> text)) {
      continue;
    }
    uint64_t word = 0;
    if (!parseNumber(text, word)) {
      print("error: '{}' is not a number", text);
      ++failures;
      continue;
    }
    std::array<std::byte, 4> bytes{};
    for (unsigned index = 0; index < width; ++index) {
      bytes[index] = static_cast<std::byte>(word >> (index * 8));
    }
    const xdec::spec::DecodedInsn insn =
        engine.decode(std::span<std::byte>{bytes.data(), width}, 0);
    if (!insn.valid) {
      print("{:#010x}  undecodable", word & 0xFFFFFFFFull);
      continue;
    }
    print("{:#010x}  {}", word & 0xFFFFFFFFull, engine.disassemble(insn));
  }
  return failures == 0 ? 0 : 1;
}

}  // namespace xdec::cli
