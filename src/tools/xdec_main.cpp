// The xdec command line driver.
//
// Subcommands are kept deliberately thin: each one is a readable exercise of
// one layer, which doubles as the acceptance check for the phase that built it.
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "xdec/binary/image.h"
#include "xdec/binary/target_profile.h"
#include "xdec/pass/manager.h"
#include "xdec/pass/observe.h"
#include "xdec/pass/registry.h"
#include "xdec/analysis/dominators.h"
#include "xdec/analysis/expr_reuse.h"
#include "xdec/analysis/loops.h"
#include "xdec/analysis/plt_stub.h"
#include "xdec/analysis/profile.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/analysis/typed_variables.h"
#include "xdec/analysis/variables.h"
#include "xdec/decompile/driver.h"
#include "xdec/emit/c_printer.h"
#include "xdec/emit/structure.h"
#include "xdec/passes/builtin.h"
#include "xdec/plugin/loader.h"
#include "xdec/spec/check.h"
#include "xdec/il/interp.h"
#include "xdec/il/printer.h"
#include "xdec/il/verify.h"
#include "xdec/spec/compile.h"
#include "xdec/spec/engine.h"
#include "xdec/spec/lift.h"
#include "xdec/spec/parse.h"
#include "xdec/support/log.h"
#include "xdec/support/prng.h"
#include "xdec/types/parse.h"
#include "xdec/types/syscall_table.h"

namespace {

XDEC_DEFINE_LOG_CATEGORY(emitLog, "emit")

using xdec::binary::BinaryImage;

/// Times one stage of the analyse-and-emit tail, which the pass manager does not
/// cover: everything from the stack frame to the printed text runs outside the
/// pipeline, and on a large flattened function that is where the wall clock goes.
template <class Fn>
auto timed(std::string_view stage, Fn&& fn) {
  const auto started = std::chrono::steady_clock::now();
  auto out = std::forward<Fn>(fn)();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  XDEC_LOG_DEBUG(emitLog(), "{:>16} {:>6}ms", stage, elapsed.count());
  return out;
}

void printLine(std::string_view text) {
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fputc('\n', stdout);
}

template <class... Args>
void print(std::format_string<Args...> fmt, Args&&... args) {
  printLine(std::format(fmt, std::forward<Args>(args)...));
}

/// Parses a decimal or 0x-prefixed hexadecimal integer.
bool parseNumber(std::string_view text, uint64_t& out) {
  int base = 10;
  if (text.starts_with("0x") || text.starts_with("0X")) {
    text.remove_prefix(2);
    base = 16;
  }
  if (text.empty()) {
    return false;
  }
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto result = std::from_chars(begin, end, out, base);
  return result.ec == std::errc{} && result.ptr == end;
}

int reportError(const xdec::Diag& diag) {
  print("error: {}", diag.format());
  return 1;
}

int usage() {
  printLine("usage: xdec <command> [arguments]");
  printLine("");
  printLine("commands:");
  printLine("  info <binary>                    summarise an image");
  printLine("  sections <binary>                list sections");
  printLine("  symbols <binary> [count]         list defined symbols by address");
  printLine("  relocs <binary> [count]          list relocations");
  printLine("  read <binary> <address> <size>   hex dump the unified memory view");
  printLine("  spec <file.xspec> [out.bin]    check a spec, optionally emitting a blob");
  printLine("  disasm <binary> <address> <n>    disassemble n instructions");
  printLine("  lift <binary> <address> <n>      lift n instructions and print the IL");
  printLine("  observe <binary> <address> [...] lift a function, run passes, dump each step");
  printLine("      options: --to <maturity> --out <dir> --plugin <path>");
  printLine("  decompile <binary> <address> [...] the full pipeline: lift, resolve, emit C");
  printLine("      options: -o <file.c> --rounds <n> --no-annotate --allow-unresolved");
  printLine("               --types <header|preset> (repeatable)");
  printLine("               --syscall-table <file|name|none> (default aarch64-linux)");
  printLine("               --reuse-report (count same-block subexpression duplication)");
  printLine("               --dump-il (print the IL after all passes, before structuring)");
  printLine("               --helpers-header <path|none> (default xdec_helpers.h)");
  printLine("  exec <binary> <workload>         execute blocks against scripted states");
  printLine("  memdump <binary> <out>           dump the relocated memory view for emulators");
  printLine("  decode                           decode hex words from stdin (fuzzer iface)");
  printLine("  types parse <header|preset>...   import C declarations and report them");
  printLine("      options: -o <out.json> --definitions");
  printLine("  coverage <binary> [rows]         report what the spec does not decode");
  printLine("  log-categories                   list logging categories");
  printLine("");
  printLine("Set XDEC_LOG=<category>=<level> for diagnostics, e.g. XDEC_LOG=pass=debug,local=debug.");
  printLine("Set XDEC_SPEC=<file.xspec> to override the architecture spec.");
  return 2;
}

xdec::Result<std::unique_ptr<BinaryImage>> open(std::string_view path) {
  return xdec::binary::openBinary(std::filesystem::path{path});
}

/// The address-space facts beyond raw bytes that only a real image can state
/// (see pass::Context::setMemoryFacts). The image must outlive every pipeline
/// the result is handed to, which for every caller here is the whole command.
[[nodiscard]] xdec::MemoryFacts memoryFactsOf(const BinaryImage& image) {
  xdec::MemoryFacts facts;
  facts.immutable = [&image](uint64_t va, uint64_t size) {
    return image.isImmutable(va, size);
  };
  facts.executable = [&image](uint64_t va) { return image.isExecutable(va); };
  facts.loader = [&image](uint64_t va) {
    xdec::LoaderValue out;
    const xdec::binary::Relocation* relocation = image.relocationAt(va);
    if (relocation == nullptr) {
      return out;
    }
    if (relocation->hasValue) {
      out.address = relocation->value;
      out.hasAddress = true;
    }
    if (const auto imported = image.importNameAt(va)) {
      out.importName = std::string{*imported};
    }
    return out;
  };
  return facts;
}

/// The image's symbol table, as the emitter asks about it (see
/// emit::SymbolResolver). Same lifetime requirement as memoryFactsOf.
[[nodiscard]] xdec::emit::SymbolResolver symbolResolverOf(const BinaryImage& image) {
  return [&image](uint64_t va) {
    xdec::emit::SymbolRef out;
    // Containing, not exact: the offset is half the answer (see emit::SymbolRef),
    // and an exact hit is just the containing symbol with an offset of zero.
    const xdec::binary::Symbol* symbol = image.symbolContaining(va);
    if (symbol == nullptr) {
      return out;
    }
    out.name = symbol->name;
    out.offset = va - symbol->va;
    out.isFunction = symbol->kind == xdec::binary::SymbolKind::Function;
    return out;
  };
}

/// The import a fixed address reaches when no symbol names it directly: the
/// callee behind a standard AArch64 PLT stub's GOT indirection (see
/// analysis/plt_stub.h), aliased to the header's own spelling of it when
/// `profile` says the loader's name and the header's differ -- Bionic's
/// `__errno` GOT entry versus the NDK header's `__errno_location`. Nullopt
/// when `va` is not a recognised PLT stub, or its GOT slot names nothing.
[[nodiscard]] std::optional<std::string> pltImportAt(const xdec::ByteReader& reader,
                                                     const xdec::MemoryFacts& facts,
                                                     const xdec::binary::TargetProfile& profile,
                                                     uint64_t va) {
  std::optional<std::string> name = xdec::analysis::importNameForPltStub(va, reader, facts);
  if (!name.has_value()) {
    return std::nullopt;
  }
  const auto alias = profile.symbolAliases.find(*name);
  return alias == profile.symbolAliases.end() ? name : std::optional<std::string>{alias->second};
}

/// The same table, as the passes ask about it (see pass::Context::setNames):
/// exact starts only, because a name is being used to find a declaration and a
/// symbol that merely covers an address declares nothing about it -- with one
/// addition beyond the symbol table itself: a PLT stub's address is not named
/// by any symbol in a stripped binary, but the stub's own bytes and the
/// relocation its GOT slot carries are just as exact a fact about what starts
/// there (see pltImportAt). This is what turns `sub_1d28a0` into
/// `__errno_location` everywhere a name feeds a prototype lookup -- the
/// direct call's own binding, apply-types' arity trim, typed-variables'
/// return type -- from the one place names are resolved.
[[nodiscard]] xdec::pass::NameAt nameResolverOf(const BinaryImage& image,
                                                const xdec::ByteReader& reader,
                                                const xdec::MemoryFacts& facts,
                                                const xdec::binary::TargetProfile& profile) {
  return [&image, &reader, &facts, &profile](uint64_t va) {
    xdec::pass::SymbolName out;
    const xdec::binary::Symbol* symbol = image.symbolContaining(va);
    if (symbol != nullptr && symbol->va == va) {
      out.name = symbol->name;
      out.isFunction = symbol->kind == xdec::binary::SymbolKind::Function;
      return out;
    }
    if (const std::optional<std::string> imported = pltImportAt(reader, facts, profile, va)) {
      out.name = *imported;
      out.isFunction = true;
    }
    return out;
  };
}

/// Where an address lives and whether it can change (see emit::AddressFacts).
/// Same lifetime requirement as memoryFactsOf.
[[nodiscard]] xdec::emit::AddressDescriber addressDescriberOf(const BinaryImage& image) {
  return [&image](uint64_t va) {
    xdec::emit::AddressFacts out;
    out.mapped = image.isMapped(va);
    if (!out.mapped) {
      return out;
    }
    out.writable = image.isWritable(va);
    const xdec::binary::Section* section = image.sectionAt(va);
    if (section == nullptr) {
      return out;
    }
    out.section = section->name;
    // Code is not a variable either, whatever else it is.
    out.variable = section->programData && !image.isExecutable(va);
    if (const std::optional<std::string_view> imported = image.importNameAt(va)) {
      out.importName = *imported;
    }
    return out;
  };
}

int commandInfo(std::string_view path) {
  auto opened = open(path);
  if (!opened) {
    return reportError(opened.error());
  }
  const BinaryImage& image = *opened.value();

  print("path        : {}", image.path());
  print("format      : {} {}", toString(image.format()), toString(image.kind()));
  print("arch        : {} {}-bit {}-endian", toString(image.arch()), image.pointerBits(),
        toString(image.endian()));
  print("file size   : {} bytes", image.fileSize());
  if (image.hasEntryPoint()) {
    print("entry       : 0x{:x}", image.entryPoint());
  } else {
    printLine("entry       : none");
  }
  if (!image.soname().empty()) {
    print("soname      : {}", image.soname());
  }
  if (!image.neededLibraries().empty()) {
    std::string needed;
    for (const std::string& library : image.neededLibraries()) {
      if (!needed.empty()) {
        needed += ", ";
      }
      needed += library;
    }
    print("needed      : {}", needed);
  }

  const auto& memory = image.memory();
  print("memory      : [0x{:x}, 0x{:x}) in {} regions", memory.lowestAddress(),
        memory.highestAddress(), memory.regions().size());
  for (const auto& region : memory.regions()) {
    const uint64_t zeroFilled = region.size - region.fileSize;
    print("  0x{:08x}-0x{:08x} {} file=0x{:<8x} zero-filled=0x{:<6x} {}", region.va,
          region.endVa(), toString(region.permissions), region.fileSize, zeroFilled, region.name);
  }

  std::size_t definedSymbols = 0;
  std::size_t importedSymbols = 0;
  std::size_t exportedSymbols = 0;
  for (const auto& entry : image.symbols()) {
    if (entry.defined) {
      ++definedSymbols;
      if (entry.exported) {
        ++exportedSymbols;
      }
    } else if (!entry.name.empty()) {
      ++importedSymbols;
    }
  }
  print("sections    : {}", image.sections().size());
  print("symbols     : {} total, {} defined, {} exported, {} imported", image.symbols().size(),
        definedSymbols, exportedSymbols, importedSymbols);

  std::size_t resolved = 0;
  std::size_t unresolved = 0;
  for (const auto& relocation : image.relocations()) {
    if (relocation.hasValue) {
      ++resolved;
    } else {
      ++unresolved;
    }
  }
  print("relocations : {} total, {} resolved to a value, {} left symbolic",
        image.relocations().size(), resolved, unresolved);

  const auto executable = image.executableRegions();
  uint64_t executableBytes = 0;
  for (const auto* region : executable) {
    executableBytes += region->size;
  }
  print("executable  : {} regions, 0x{:x} bytes", executable.size(), executableBytes);
  return 0;
}

int commandSections(std::string_view path) {
  auto opened = open(path);
  if (!opened) {
    return reportError(opened.error());
  }
  const BinaryImage& image = *opened.value();
  print("{:<24} {:>10} {:>10} {:>10} {:<5} {}", "name", "address", "size", "file-off", "perm",
        "flags");
  for (const auto& section : image.sections()) {
    std::string flags;
    if (section.allocated) {
      flags += "alloc ";
    }
    if (section.zeroFilled) {
      flags += "zero-filled ";
    }
    print("{:<24} 0x{:08x} 0x{:08x} 0x{:08x} {:<5} {}", section.name.empty() ? "<unnamed>" : section.name,
          section.va, section.size, section.fileOffset, toString(section.permissions), flags);
  }
  return 0;
}

int commandSymbols(std::string_view path, uint64_t limit) {
  auto opened = open(path);
  if (!opened) {
    return reportError(opened.error());
  }
  const BinaryImage& image = *opened.value();

  std::vector<const xdec::binary::Symbol*> ordered;
  for (const auto& symbol : image.symbols()) {
    if (symbol.defined && !symbol.name.empty()) {
      ordered.push_back(&symbol);
    }
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const auto* a, const auto* b) { return a->va < b->va; });

  print("{} defined named symbols", ordered.size());
  uint64_t shown = 0;
  for (const auto* symbol : ordered) {
    if (shown++ >= limit) {
      print("... {} more", ordered.size() - limit);
      break;
    }
    print("  0x{:08x} size={:<8} {:<8} {:<7} {}{}", symbol->va, symbol->size,
          toString(symbol->kind), toString(symbol->binding), symbol->name,
          symbol->exported ? " [exported]" : "");
  }
  return 0;
}

// ---------------------------------------------------------------------------
// memdump: the unified memory view as a binary blob, for seeding emulators
// ---------------------------------------------------------------------------
//
// Format: little-endian, repeated records of
//   u64 va, u64 size, u8 contents[size]
// exactly as image.read() sees them — file bytes with .bss zero-filled and
// every resolved relocation applied. A differential harness (diff_unicorn.py)
// seeds Unicorn from this so both sides execute against the same image, not
// two divergent interpretations of the ELF.

int commandMemDump(std::string_view path, std::string_view outPath) {
  auto opened = open(path);
  if (!opened) {
    return reportError(opened.error());
  }
  const BinaryImage& image = *opened.value();

  std::ofstream out{std::filesystem::path{outPath},
                    std::ios::binary | std::ios::trunc};
  if (!out) {
    print("error: cannot write '{}'", outPath);
    return 1;
  }
  const auto put64 = [&out](uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
      out.put(static_cast<char>((value >> shift) & 0xFF));
    }
  };

  uint64_t regions = 0;
  for (const xdec::binary::MemoryRegion& region : image.memory().regions()) {
    if (region.size == 0) {
      continue;
    }
    std::vector<std::byte> contents(region.size);
    auto read = image.read(region.va, contents);
    if (!read) {
      print("error: cannot read region at {:#x}: {}", region.va, read.error().format());
      return 1;
    }
    put64(region.va);
    put64(region.size);
    out.write(reinterpret_cast<const char*>(contents.data()),
              static_cast<std::streamsize>(contents.size()));
    ++regions;
  }
  print("wrote {} regions to {}", regions, outPath);
  return 0;
}

int commandRelocs(std::string_view path, uint64_t limit) {
  auto opened = open(path);
  if (!opened) {
    return reportError(opened.error());
  }
  const BinaryImage& image = *opened.value();

  // Summarise by kind first: the distribution is what tells you how the image
  // was linked, and it is far more useful than the first N records.
  std::map<std::string_view, std::size_t> byKind;
  for (const auto& relocation : image.relocations()) {
    ++byKind[toString(relocation.kind)];
  }
  print("{} relocations", image.relocations().size());
  for (const auto& [kind, count] : byKind) {
    print("  {:<16} {}", kind, count);
  }

  uint64_t shown = 0;
  for (const auto& relocation : image.relocations()) {
    if (shown++ >= limit) {
      break;
    }
    std::string target;
    if (relocation.hasValue) {
      target = std::format("= 0x{:x}", relocation.value);
    } else if (const auto name = image.importNameAt(relocation.va); name.has_value()) {
      target = std::format("-> import '{}'", *name);
    } else {
      target = "unresolved";
    }
    print("  0x{:08x} {:<16} raw={:<5} addend={:<8} {}", relocation.va,
          toString(relocation.kind), relocation.rawType, relocation.addend, target);
  }
  return 0;
}

int commandRead(std::string_view path, uint64_t address, uint64_t size) {
  auto opened = open(path);
  if (!opened) {
    return reportError(opened.error());
  }
  const BinaryImage& image = *opened.value();

  if (size == 0 || size > 1u << 20) {
    print("error: size must be between 1 and 0x100000");
    return 1;
  }

  std::vector<std::byte> buffer(static_cast<std::size_t>(size));
  if (auto read = image.read(address, buffer); !read) {
    return reportError(read.error());
  }

  // Annotate what the address belongs to, so a zero-filled read is visibly a
  // real zero rather than a failed one.
  if (const auto* section = image.sectionAt(address); section != nullptr) {
    print("section {} {}", section->name, section->zeroFilled ? "(zero-filled)" : "");
  }
  if (const auto* region = image.memory().regionAt(address); region != nullptr) {
    const uint64_t offsetInRegion = address - region->va;
    const bool beyondFile = offsetInRegion >= region->fileSize;
    print("region {} {} at +0x{:x}{}", region->name, toString(region->permissions),
          offsetInRegion, beyondFile ? " (past file-backed bytes; reads as zero)" : "");
  }

  for (std::size_t offset = 0; offset < buffer.size(); offset += 16) {
    std::string hex;
    std::string ascii;
    for (std::size_t column = 0; column < 16; ++column) {
      if (offset + column >= buffer.size()) {
        hex += "   ";
        continue;
      }
      const auto value = std::to_integer<uint8_t>(buffer[offset + column]);
      hex += std::format("{:02x} ", value);
      ascii += (value >= 0x20 && value < 0x7F) ? static_cast<char>(value) : '.';
    }
    print("0x{:08x}  {} |{}|", address + offset, hex, ascii);
  }
  return 0;
}

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

/// Where to find the architecture spec. An explicit XDEC_SPEC wins; otherwise
/// the copy installed next to the binary is used, so the tool works from any
/// working directory.
std::filesystem::path specPath() {
  if (const char* fromEnv = std::getenv("XDEC_SPEC"); fromEnv != nullptr && *fromEnv != '\0') {
    return std::filesystem::path{fromEnv};
  }
  return std::filesystem::path{XDEC_SPEC_DIR} / "arm64.xspec";
}

/// Loads the spec, from either a compiled blob or source. Which one is decided
/// by the magic rather than the extension, so a stale blob cannot be mistaken
/// for source and produce a parse error twenty lines in.
xdec::Result<std::unique_ptr<xdec::spec::SpecEngine>> loadEngine() {
  const std::filesystem::path path = specPath();
  auto source = xdec::binary::FileBuffer::fromFile(path);
  if (!source) {
    return xdec::err(std::move(source).error());
  }
  const std::span<const std::byte> bytes = source.value().bytes();

  uint32_t magic = 0;
  if (bytes.size() >= sizeof(magic)) {
    std::memcpy(&magic, bytes.data(), sizeof(magic));
  }
  if (magic == xdec::spec::SpecProgram::kMagic) {
    XDEC_TRY(std::unique_ptr<xdec::spec::SpecProgram> program, xdec::spec::deserialize(bytes));
    return std::make_unique<xdec::spec::SpecEngine>(std::move(program));
  }
  return xdec::spec::loadSpecFile(path);
}

/// Disassembles a run of instructions from the unified memory view. This is the
/// smallest thing that exercises the whole front end at once: image loading,
/// the decoder tree, field extraction and the assembly templates.
int commandDisasm(std::string_view path, uint64_t address, uint64_t count) {
  auto opened = open(path);
  if (!opened) {
    return reportError(opened.error());
  }
  auto engineOrError = loadEngine();
  if (!engineOrError) {
    return reportError(engineOrError.error());
  }

  const BinaryImage& image = *opened.value();
  const xdec::spec::SpecEngine& engine = *engineOrError.value();
  const unsigned width = engine.program().insnWidth / 8;

  std::vector<std::byte> buffer(static_cast<std::size_t>(count) * width);
  if (auto read = image.read(address, buffer); !read) {
    return reportError(read.error());
  }

  std::size_t undecoded = 0;
  for (uint64_t index = 0; index < count; ++index) {
    const std::size_t offset = static_cast<std::size_t>(index) * width;
    const uint64_t va = address + offset;
    const auto insn = engine.decode(std::span{buffer}.subspan(offset, width), va);
    if (!insn.valid) {
      ++undecoded;
    }
    const auto flow = engine.probe(insn);
    print("{:#010x}  {:08x}  {:<32} {}", va, static_cast<uint32_t>(insn.word),
          engine.disassemble(insn),
          flow.kind == xdec::spec::FlowKind::Fallthrough ? "" : toString(flow.kind));
  }
  if (undecoded != 0) {
    print("{} of {} words not covered by the spec", undecoded, count);
  }
  return 0;
}

/// Lifts a run of instructions and prints the IL.
///
/// This is the two-pass shape the engine is built for, in miniature: probe every
/// instruction to find where blocks begin, create them, then elaborate. Doing it
/// in one pass is not possible, because a forward branch names a block that has
/// not been reached yet.
int commandLift(std::string_view path, uint64_t address, uint64_t count) {
  auto opened = open(path);
  if (!opened) {
    return reportError(opened.error());
  }
  auto engineOrError = loadEngine();
  if (!engineOrError) {
    return reportError(engineOrError.error());
  }

  const BinaryImage& image = *opened.value();
  const xdec::spec::SpecEngine& engine = *engineOrError.value();
  const unsigned width = engine.program().insnWidth / 8;

  std::vector<std::byte> buffer(static_cast<std::size_t>(count) * width);
  if (auto read = image.read(address, buffer); !read) {
    return reportError(read.error());
  }

  const uint64_t end = address + buffer.size();
  const auto inRange = [&](uint64_t va) { return va >= address && va < end; };
  const auto insnAt = [&](uint64_t va) {
    const std::size_t offset = static_cast<std::size_t>(va - address);
    return engine.decode(std::span{buffer}.subspan(offset, width), va);
  };

  // Pass one: every address a block can begin at. The range start is one; so is
  // any branch target inside it, and whatever follows a terminator.
  std::set<uint64_t> leaders{address};
  std::set<uint64_t> external;
  for (uint64_t va = address; va < end; va += width) {
    const auto flow = engine.probe(insnAt(va));
    if (!flow.terminates()) {
      // probe reports a fall-through address for every instruction; only an
      // actual terminator makes its successors block leaders.
      continue;
    }
    for (const uint64_t target : {flow.target, flow.fallthrough}) {
      if (target == 0) {
        continue;
      }
      (inRange(target) ? leaders : external).insert(target);
    }
    if (inRange(va + width)) {
      leaders.insert(va + width);
    }
  }

  xdec::il::Function function{engine.program().arch, engine.program().registers, address};
  function.setMaturity(xdec::il::Maturity::Lifted);
  std::map<uint64_t, xdec::il::BlockId> blocks;
  for (const uint64_t leader : leaders) {
    blocks.emplace(leader, function.createBlock(leader));
  }
  function.setEntryBlock(blocks.at(address));

  xdec::spec::LiftSite site;
  site.function = &function;
  site.blockAt = [&blocks](uint64_t target) {
    const auto found = blocks.find(target);
    return found == blocks.end() ? xdec::il::BlockId{} : found->second;
  };

  // Pass two: elaborate, moving to the next block as each leader is reached.
  auto current = blocks.begin();
  for (uint64_t va = address; va < end; va += width) {
    if (const auto leader = blocks.find(va); leader != blocks.end()) {
      if (current != leader) {
        function.block(current->second).endVa = va;
        // A block that runs off its end into the next one needs that edge
        // spelled out. The engine cannot do it: elaborating one instruction
        // gives no way to know a block boundary follows it.
        const xdec::il::Block& previous = function.block(current->second);
        if (previous.ops.empty() || !function.op(previous.ops.back()).isTerminator()) {
          // Attributed to the instruction that fell through, so the edge is
          // traceable to a real address like every other op.
          function.appendBranch(current->second, va - width, leader->second);
        }
      }
      current = leader;
    }
    site.block = current->second;
    site.address = va;
    if (auto lifted = engine.elaborate(insnAt(va), site); !lifted) {
      return reportError(lifted.error());
    }
  }
  function.block(current->second).endVa = end;
  function.rebuildEdges();

  print("{}", xdec::il::print(function));
  if (!external.empty()) {
    print("{} target(s) outside the lifted range, first {:#x}", external.size(), *external.begin());
  }

  const auto report = xdec::il::verify(function);
  for (const xdec::Diag& diag : report.errors) {
    print("verify error: {}", diag.format());
  }
  for (const xdec::Diag& diag : report.warnings) {
    print("verify warning: {}", diag.format());
  }
  return report.ok() ? 0 : 1;
}

/// Lifts a whole function by recursive descent and runs the pass pipeline
/// under the dump observer, so every pass's effect lands in a file pair
/// (NN-pass.il / .map) plus an index, instead of in a scrollback buffer.
///
/// With no real optimisation passes built in yet, the pipeline is whatever
/// --plugin brings; the command is the harness P7's passes plug into.
int commandObserve(std::string_view path, uint64_t address, std::span<const std::string_view> options) {
  xdec::il::Maturity target = xdec::il::Maturity::Lifted;
  std::filesystem::path outDir =
      std::filesystem::path{std::format("observe-{:x}", address)};
  std::vector<std::string> pluginPaths;
  unsigned rounds = 8;
  bool roundsPinned = false;  // as in decompile: a chosen count is a wall

  for (std::size_t i = 0; i < options.size(); ++i) {
    const std::string_view option = options[i];
    const auto value = [&]() -> std::string_view {
      if (++i >= options.size()) {
        return {};
      }
      return options[i];
    };
    if (option == "--to") {
      const std::string_view text = value();
      if (!xdec::il::parseMaturity(text, target)) {
        print("error: '{}' is not a maturity level", text);
        return 1;
      }
    } else if (option == "--out") {
      outDir = std::filesystem::path{std::string{value()}};
    } else if (option == "--rounds") {
      uint64_t parsed = 0;
      if (!parseNumber(value(), parsed) || parsed == 0 || parsed > 1024) {
        print("error: '{}' is not a usable round cap", value());
        return 1;
      }
      rounds = static_cast<unsigned>(parsed);
      roundsPinned = true;
    } else if (option == "--plugin") {
      pluginPaths.emplace_back(value());
    } else {
      print("error: unknown observe option '{}'", option);
      return 1;
    }
  }

  auto opened = open(path);
  if (!opened) {
    return reportError(opened.error());
  }
  auto engineOrError = loadEngine();
  if (!engineOrError) {
    return reportError(engineOrError.error());
  }
  const BinaryImage& image = *opened.value();
  const xdec::spec::SpecEngine& engine = *engineOrError.value();

  const xdec::spec::ByteReader reader =
      [&image](uint64_t va, std::span<std::byte> out) { return image.read(va, out); };

  // Declaration order is load order reversed: the Plugin must outlive the
  // Registry that owns its passes, so it unloads only after they are destroyed
  // (see plugin/abi.h). Plugins are therefore declared first.
  std::vector<xdec::plugin::Plugin> plugins;
  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);
  for (const std::string& pluginPath : pluginPaths) {
    auto plugin = xdec::plugin::Plugin::load(pluginPath, registry);
    if (!plugin) {
      return reportError(plugin.error());
    }
    print("plugin: {} ({} pass(es) so far)", pluginPath, registry.size());
    plugins.push_back(std::move(*plugin));
  }

  xdec::pass::DumpObserver observer(outDir);
  xdec::decompile::DriverOptions driverOptions;
  driverOptions.target = target;
  driverOptions.observer = &observer;
  driverOptions.maxRounds = rounds;
  driverOptions.extendWhileProving = !roundsPinned;
  driverOptions.memory = memoryFactsOf(image);
  if (const xdec::binary::Symbol* symbol = image.symbolAt(address);
      symbol != nullptr && symbol->size != 0) {
    driverOptions.fence = {address, address + symbol->size};
  }
  auto result = xdec::decompile::decompile(engine, reader, address, registry, driverOptions);
  if (!result) {
    return reportError(result.error());
  }
  if (observer.failure()) {
    return reportError(*observer.failure());
  }
  const xdec::il::Function& function = *result->function;
  print("driver: {} round(s), {} extra entrie(s), {} block(s) total{}", result->report.rounds,
        result->report.extraEntries.size(), function.blockCount(),
        result->report.converged ? "" : " (round cap reached; coverage may be partial)");
  print("profile: {}", xdec::analysis::profile(function).format());
  print("observe dumps in '{}'", outDir.string());
  return 0;
}

/// Loads one or more headers (paths or preset names) into a single database,
/// stacking them in the order given so a project header can build on a preset.
xdec::Result<xdec::types::TypeDatabase> loadTypes(std::span<const std::string> sources,
                                                  bool verbose) {
  xdec::types::TypeDatabase database;
  for (const std::string& source : sources) {
    XDEC_TRY(const std::string resolved, xdec::types::resolveHeaderPath(source));
    XDEC_TRY(const xdec::types::ParseReport report,
             xdec::types::parseHeaderFile(resolved, database));
    if (verbose || report.skipped != 0) {
      print("{}", report.format(source));
    }
  }
  return database;
}

/// Whether an address's first instruction disassembles like the start of an
/// AArch64 function: a stack frame going up (`sub sp`, a `stp`/`str` saving
/// callee-saved registers) or one of the landing-pad no-ops (BTI/PAC, or this
/// project's own obfuscator's `mov x17, x17` / `mov x16, x16`) that precede
/// one. Best-effort and deliberately permissive -- it exists to catch the
/// class of mistake decompiling `sub_627ac` in bc_lib made (an address pulled
/// from the middle of another function's jump table, decompiled as if it were
/// its own entry, which then discovers that whole table as if it were free
/// code -- see samples/manifest.json's sample_afRDLog comment for the real
/// entry), not to reject every real prologue this short list does not cover.
[[nodiscard]] bool looksLikePrologue(const xdec::spec::SpecEngine& engine,
                                     const BinaryImage& image, uint64_t address) {
  const unsigned width = engine.program().insnWidth / 8;
  std::vector<std::byte> buffer(width);
  if (!image.read(address, buffer)) {
    return true;  // unreadable is a different problem, not this check's to report
  }
  const auto insn = engine.decode(buffer, address);
  if (!insn.valid) {
    return true;
  }
  const std::string text = engine.disassemble(insn);
  static constexpr std::string_view kPrologueShapes[] = {
      "sub sp, sp", "stp x29",      "stp x28",      "str d",   "stp d",
      "mov x17, x17", "mov x16, x16", "paciasp",      "pacibsp", "bti ",
  };
  return std::ranges::any_of(
      kPrologueShapes, [&](std::string_view shape) { return text.find(shape) != std::string::npos; });
}

/// The full pipeline: discover and lift, resolve, recover variables,
/// structure, emit C. This is the deliverable every other command builds
/// towards.
int commandDecompile(std::string_view path, uint64_t address,
                     std::span<const std::string_view> options) {
  std::filesystem::path outPath;
  unsigned rounds = 8;
  // A round count the user chose is a wall, not a budget: they asked for
  // bounded work (see DriverOptions::extendWhileProving).
  bool roundsPinned = false;
  bool annotate = true;
  bool allowUnresolved = false;
  bool reuseReport = false;
  bool dumpIl = false;
  std::vector<std::string> typeSources;
  // On by default, because a syscall's number means the same thing in every
  // AArch64 Linux binary and leaving it unnamed helps nobody. `--syscall-table
  // none` is there for the case the default is wrong -- a different kernel ABI
  // -- where a plausible name would be worse than a number.
  std::string syscallSource{xdec::types::SyscallTable::defaultName()};
  // What the preamble `#include`s for rotate/bswap/popcount/cc_* (see
  // xdec_helpers.h). `none` suppresses the include for a caller who wants
  // those names some other way -- inlined, or from a different path.
  std::string helpersHeader{"xdec_helpers.h"};
  for (std::size_t i = 0; i < options.size(); ++i) {
    const std::string_view option = options[i];
    const auto value = [&]() -> std::string_view {
      if (++i >= options.size()) {
        return {};
      }
      return options[i];
    };
    if (option == "-o" || option == "--out") {
      outPath = std::filesystem::path{std::string{value()}};
    } else if (option == "--rounds") {
      uint64_t parsed = 0;
      if (!parseNumber(value(), parsed) || parsed == 0 || parsed > 1024) {
        print("error: '{}' is not a usable round cap", value());
        return 1;
      }
      rounds = static_cast<unsigned>(parsed);
      roundsPinned = true;
    } else if (option == "--no-annotate") {
      annotate = false;
    } else if (option == "--allow-unresolved") {
      allowUnresolved = true;
    } else if (option == "--reuse-report") {
      reuseReport = true;
    } else if (option == "--dump-il") {
      dumpIl = true;
    } else if (option == "--types") {
      typeSources.emplace_back(value());
    } else if (option == "--syscall-table") {
      const std::string_view text = value();
      syscallSource = text == "none" ? std::string{} : std::string{text};
    } else if (option == "--helpers-header") {
      const std::string_view text = value();
      helpersHeader = text == "none" ? std::string{} : std::string{text};
    } else {
      print("error: unknown decompile option '{}'", option);
      return 1;
    }
  }

  auto opened = open(path);
  if (!opened) {
    return reportError(opened.error());
  }
  auto engineOrError = loadEngine();
  if (!engineOrError) {
    return reportError(engineOrError.error());
  }
  const BinaryImage& image = *opened.value();
  const xdec::spec::SpecEngine& engine = *engineOrError.value();
  const xdec::spec::ByteReader reader =
      [&image](uint64_t va, std::span<std::byte> out) { return image.read(va, out); };

  // What the platform implies, applied wherever the user did not already say
  // something more specific: `--types`/`--syscall-table` are still there for
  // when the inference is wrong, but the common case -- an AArch64 .so, this
  // project's only supported target today -- no longer needs either flag
  // (see xdec/binary/target_profile.h).
  const xdec::binary::TargetProfile profile = xdec::binary::inferTargetProfile(image);
  if (typeSources.empty()) {
    typeSources = profile.typePresets;
  }
  if (syscallSource == xdec::types::SyscallTable::defaultName() && !profile.syscallTable.empty()) {
    syscallSource = profile.syscallTable;
  }

  xdec::types::TypeDatabase types;
  if (!typeSources.empty()) {
    auto loaded = loadTypes(typeSources, /*verbose=*/false);
    if (!loaded) {
      return reportError(loaded.error());
    }
    types = std::move(*loaded);
    print("types: {} type(s), {} declaration(s) from {} header(s)", types.typeCount(),
          types.declarations().size(), typeSources.size());
  }

  xdec::types::SyscallTable syscalls;
  if (!syscallSource.empty()) {
    const auto load = [&]() -> xdec::Result<xdec::types::SyscallTable> {
      XDEC_TRY(const std::string resolved,
               xdec::types::SyscallTable::resolvePath(syscallSource));
      return xdec::types::SyscallTable::loadFile(resolved);
    };
    auto loaded = load();
    if (!loaded) {
      // A table the user named must load: they said which ABI this is, and
      // guessing past that would name syscalls from the wrong kernel. The
      // default is different -- it is the build's own data file, and a broken
      // installation should not stop a decompilation that never needed it.
      if (syscallSource != xdec::types::SyscallTable::defaultName()) {
        return reportError(loaded.error());
      }
      print("note: no syscall table ({}); svc will print as __xdec_syscall",
            loaded.error().format());
    } else {
      syscalls = std::move(*loaded);
    }
  }
  // Both loaded independently (the syscall table has no reason to depend on
  // whether a header was given, see SyscallTable::resolveTypes), so the link
  // between them is made here, once, rather than by either constructor.
  if (!typeSources.empty() && !syscalls.empty()) {
    syscalls.resolveTypes(types);
  }

  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);
  xdec::decompile::DriverOptions driverOptions;
  // Vars, not Resolved: emission reads the recovered call arity off the IL, and
  // stopping a level short would leave it guessing (see passes/vars.h).
  driverOptions.target = xdec::il::Maturity::Vars;
  driverOptions.maxRounds = rounds;
  driverOptions.extendWhileProving = !roundsPinned;
  driverOptions.sealUnresolvedBranches = allowUnresolved;
  driverOptions.memory = memoryFactsOf(image);
  driverOptions.types = typeSources.empty() ? nullptr : &types;
  driverOptions.syscalls = syscalls.empty() ? nullptr : &syscalls;
  driverOptions.names = nameResolverOf(image, reader, driverOptions.memory, profile);
  if (const xdec::binary::Symbol* symbol = image.symbolAt(address);
      symbol != nullptr && symbol->size != 0) {
    driverOptions.fence = {address, address + symbol->size};
  } else if (!looksLikePrologue(engine, image, address)) {
    // No symbol to fence discovery with, and the entry itself does not look
    // like a function start either: exactly the shape that let sub_627ac
    // silently balloon into 1349 discovered addresses and 44k lines. Warn,
    // rather than fail -- an unusual but real entry (a hand-written
    // trampoline, a prologue-less leaf) is still worth decompiling, just not
    // silently mistaken for one when it might not be.
    print("warning: {:#x} has no symbol and its first instruction does not look like a "
          "function prologue; if this address is inside another function's body (a "
          "jump-table target, say) rather than a real entry, discovery has no function "
          "size to stay inside and this run may pull in unrelated code",
          address);
  }
  auto result = xdec::decompile::decompile(engine, reader, address, registry, driverOptions);
  if (!result) {
    return reportError(result.error());
  }
  const xdec::il::Function& function = *result->function;
  print("driver: {} round(s), {} extra entrie(s), {} block(s) total{}", result->report.rounds,
        result->report.extraEntries.size(), function.blockCount(),
        result->report.converged ? "" : " (round cap reached; coverage may be partial)");
  if (reuseReport) {
    const xdec::analysis::ExpressionReuseReport reuse =
        timed("reuse-report", [&] { return xdec::analysis::analyzeExpressionReuse(function); });
    print("reuse: {} exact duplicate(s), {} structural duplicate(s)",
          reuse.count(xdec::analysis::ReuseKind::ExactDuplicate),
          reuse.count(xdec::analysis::ReuseKind::StructuralDuplicate));
    if (!reuse.findings.empty()) {
      print("{}", reuse.format(function));
    }
  }
  if (dumpIl) {
    print("{}", xdec::il::print(function));
  }

  const xdec::analysis::StackFrame frame =
      timed("stack-frame", [&] { return xdec::analysis::StackFrame::compute(function); });
  xdec::analysis::VariableTable variables = timed(
      "vars", [&] { return xdec::analysis::VariableTable::recover(function, frame); });
  // Outlives this block -- COptions::typedVariables below borrows it, so it
  // has to still be alive at emission, not just while applyImportedTypes
  // runs. Default-constructed (every lookup unset) when neither a header nor
  // a syscall table was given, the same "no evidence" shape TypedVariables
  // itself returns for that case.
  xdec::analysis::TypedVariables typedVariables;
  // Built unconditionally: CContext's own binder and calleeName (below) reuse
  // this exact resolver, so a callee named through a PLT stub's import (see
  // nameResolverOf) resolves the same way whether or not a header ended up
  // loaded.
  const xdec::types::NameAt namesForTypes = [&](uint64_t va) {
    const xdec::pass::SymbolName symbol = driverOptions.names(va);
    return xdec::types::BoundName{symbol.name, symbol.isFunction};
  };
  if (driverOptions.types != nullptr || driverOptions.syscalls != nullptr) {
    typedVariables = timed("typed-variables", [&] {
      return xdec::analysis::TypedVariables::recover(function, frame, driverOptions.types,
                                                      driverOptions.syscalls, namesForTypes, {},
                                                      driverOptions.memory, &profile);
    });
    if (driverOptions.types != nullptr) {
      const xdec::types::TypeBinder binder(*driverOptions.types, namesForTypes);
      variables.applyImportedTypes(typedVariables, binder);
    }
  }
  const xdec::analysis::Dominators dominators =
      timed("dominators", [&] { return xdec::analysis::Dominators::compute(function); });
  const xdec::analysis::PostDominators postDominators = timed(
      "post-dominators", [&] { return xdec::analysis::PostDominators::compute(function); });
  const std::vector<xdec::analysis::NaturalLoop> loops =
      timed("loops", [&] { return xdec::analysis::naturalLoops(function, dominators); });
  const xdec::emit::StructuredFunction structured = timed("structure", [&] {
    return xdec::emit::structureFunction(function, dominators, postDominators, loops);
  });
  xdec::emit::COptions cOptions;
  cOptions.annotateBlocks = annotate;
  cOptions.symbols = symbolResolverOf(image);
  cOptions.addresses = addressDescriberOf(image);
  cOptions.types = driverOptions.types;
  cOptions.syscalls = driverOptions.syscalls;
  cOptions.typedVariables = &typedVariables;
  cOptions.names = namesForTypes;
  cOptions.memory = driverOptions.memory;
  cOptions.helpersHeader = helpersHeader;
  const std::string text = timed("print", [&] {
    return xdec::emit::printFunction(function, variables, frame, structured, cOptions);
  });

  print("emit: {} argument(s), {} local(s), {} temp(s), {} labeled block(s)",
        variables.arguments().size(), variables.locals().size(),
        variables.temps().size(), structured.labeled.size());
  if (outPath.empty()) {
    print("{}", text);
  } else {
    std::ofstream stream(outPath, std::ios::binary | std::ios::trunc);
    if (!stream) {
      print("error: cannot open '{}' for writing", outPath.string());
      return 1;
    }
    stream << text;
    print("wrote '{}' ({} bytes)", outPath.string(), text.size());
  }
  return 0;
}

/// `xdec types parse` — read headers and report, or cache, what they declared.
///
/// This exists because the failure mode of an import is silence: a header that
/// half-parsed produces a decompilation that is subtly less typed than
/// expected, with nothing pointing at the header. Running the parser on its own
/// makes what was imported, and what was skipped, the whole output.
int commandTypes(std::span<const std::string_view> args) {
  if (args.empty() || args[0] != "parse") {
    printLine("usage: xdec types parse <header|preset>... [-o <out.json>] [--definitions]");
    return 2;
  }

  std::vector<std::string> sources;
  std::filesystem::path outPath;
  bool definitions = false;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string_view option = args[i];
    if (option == "-o" || option == "--out") {
      if (++i >= args.size()) {
        printLine("error: -o needs a path");
        return 1;
      }
      outPath = std::filesystem::path{std::string{args[i]}};
    } else if (option == "--definitions") {
      definitions = true;
    } else if (option.starts_with("-")) {
      print("error: unknown types option '{}'", option);
      return 1;
    } else {
      sources.emplace_back(option);
    }
  }
  if (sources.empty()) {
    printLine("error: types parse needs at least one header or preset name");
    return 1;
  }

  auto database = loadTypes(sources, /*verbose=*/true);
  if (!database) {
    return reportError(database.error());
  }
  print("imported {} type(s), {} declaration(s)", database->typeCount(),
        database->declarations().size());

  if (definitions) {
    print("{}", database->formatDefinitions());
  }
  if (!outPath.empty()) {
    const std::string text = database->toJson().dump();
    std::ofstream stream(outPath, std::ios::binary | std::ios::trunc);
    if (!stream) {
      print("error: cannot open '{}' for writing", outPath.string());
      return 1;
    }
    stream << text;
    print("wrote '{}' ({} bytes)", outPath.string(), text.size());
  }
  return 0;
}

/// Decodes every executable word in the image and reports what the spec does
/// not cover, grouped by the top bits that select an encoding group.
///
/// This is the progress metric for the architecture spec. Counting mnemonics in
/// a manual is not: the distribution is so skewed that the first ten encoding
/// groups are worth more than the next hundred.
int commandCoverage(std::string_view path, uint64_t limit) {
  auto opened = open(path);
  if (!opened) {
    return reportError(opened.error());
  }
  auto engineOrError = loadEngine();
  if (!engineOrError) {
    return reportError(engineOrError.error());
  }

  const BinaryImage& image = *opened.value();
  const xdec::spec::SpecEngine& engine = *engineOrError.value();
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

int commandLogCategories() {
  printLine("logging categories (set XDEC_LOG=name=level):");
  for (const auto* category : xdec::logCategories()) {
    print("  {:<12} {}", category->name(), toString(category->level()));
  }
  return 0;
}

}  // namespace

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

// ---------------------------------------------------------------------------
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

/// The memfill byte stream: one 64-bit word per step, little-endian. Shared
/// with the tests through xdec/support/prng.h and mirrored byte-for-byte by
/// splitmix64_stream in tools/diff_unicorn.py.

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
  auto opened = open(path);
  if (!opened) {
    return reportError(opened.error());
  }
  auto engineOrError = loadEngine();
  if (!engineOrError) {
    return reportError(engineOrError.error());
  }
  const BinaryImage& image = *opened.value();
  const xdec::spec::SpecEngine& engine = *engineOrError.value();
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

int main(int argc, char** argv) {
  const std::vector<std::string_view> args{argv + 1, argv + argc};
  if (args.empty()) {
    return usage();
  }

  const std::string_view command = args[0];
  const auto requireArgs = [&args](std::size_t count) { return args.size() > count; };

  if (command == "log-categories") {
    return commandLogCategories();
  }
  if (command == "-h" || command == "--help" || command == "help") {
    usage();
    return 0;
  }
  if (command == "decode") {
    return commandDecode();
  }
  if (command == "types") {
    return commandTypes(std::span<const std::string_view>{args}.subspan(1));
  }
  if (!requireArgs(1)) {
    print("error: '{}' needs a binary path", command);
    return usage();
  }

  if (command == "spec") {
    return commandSpec(args[1], args.size() > 2 ? args[2] : std::string_view{});
  }
  if (command == "info") {
    return commandInfo(args[1]);
  }
  if (command == "sections") {
    return commandSections(args[1]);
  }
  if (command == "coverage") {
    uint64_t limit = 20;
    if (args.size() > 2 && !parseNumber(args[2], limit)) {
      print("error: '{0}' is not a number", args[2]);
      return 1;
    }
    return commandCoverage(args[1], limit);
  }
  if (command == "symbols" || command == "relocs") {
    uint64_t limit = 40;
    if (args.size() > 2 && !parseNumber(args[2], limit)) {
      print("error: '{}' is not a number", args[2]);
      return 1;
    }
    return command == "symbols" ? commandSymbols(args[1], limit) : commandRelocs(args[1], limit);
  }
  if (command == "exec") {
    if (!requireArgs(2)) {
      printLine("error: exec needs a binary and a workload file");
      return usage();
    }
    return commandExec(args[1], args[2]);
  }
  if (command == "memdump") {
    if (!requireArgs(2)) {
      printLine("error: memdump needs a binary and an output path");
      return usage();
    }
    return commandMemDump(args[1], args[2]);
  }
  if (command == "observe") {
    if (!requireArgs(2)) {
      printLine("error: observe needs a binary and a function address");
      return usage();
    }
    uint64_t address = 0;
    if (!parseNumber(args[2], address)) {
      print("error: '{}' is not a number", args[2]);
      return 1;
    }
    return commandObserve(args[1], address,
                          std::span<const std::string_view>{args}.subspan(3));
  }
  if (command == "decompile") {
    if (!requireArgs(2)) {
      printLine("error: decompile needs a binary and a function address");
      return usage();
    }
    uint64_t address = 0;
    if (!parseNumber(args[2], address)) {
      print("error: '{}' is not a number", args[2]);
      return 1;
    }
    return commandDecompile(args[1], address,
                            std::span<const std::string_view>{args}.subspan(3));
  }
  if (command == "read" || command == "disasm" || command == "lift") {
    if (!requireArgs(3)) {
      print("error: {} needs an address and a count", command);
      return usage();
    }
    uint64_t address = 0;
    uint64_t size = 0;
    if (!parseNumber(args[2], address) || !parseNumber(args[3], size)) {
      printLine("error: address and size must be numbers");
      return 1;
    }
    if (command == "read") {
      return commandRead(args[1], address, size);
    }
    return command == "disasm" ? commandDisasm(args[1], address, size)
                               : commandLift(args[1], address, size);
  }

  print("error: unknown command '{}'", command);
  return usage();
}


