// info, sections, symbols, relocs, read, memdump: commands that only need an
// opened BinaryImage, no architecture spec.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "common.h"
#include "session.h"
#include "xdec/binary/image.h"

namespace xdec::cli {

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

}  // namespace xdec::cli
