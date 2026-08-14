#include "xdec/binary/image.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>

#include "xdec/support/bits.h"
#include "xdec/support/log.h"

namespace xdec::binary {

XDEC_DEFINE_LOG_CATEGORY(logBinary, "binary")

std::string_view toString(BinaryFormat format) noexcept {
  switch (format) {
    case BinaryFormat::Unknown:
      return "unknown";
    case BinaryFormat::Elf:
      return "elf";
    case BinaryFormat::Pe:
      return "pe";
    case BinaryFormat::MachO:
      return "macho";
    case BinaryFormat::DyldCache:
      return "dyld-cache";
  }
  return "unknown";
}

std::string_view toString(BinaryKind kind) noexcept {
  switch (kind) {
    case BinaryKind::Unknown:
      return "unknown";
    case BinaryKind::Executable:
      return "executable";
    case BinaryKind::SharedObject:
      return "shared-object";
    case BinaryKind::Relocatable:
      return "relocatable";
    case BinaryKind::Core:
      return "core";
  }
  return "unknown";
}

std::string_view toString(SymbolKind kind) noexcept {
  switch (kind) {
    case SymbolKind::Unknown:
      return "unknown";
    case SymbolKind::Function:
      return "func";
    case SymbolKind::Object:
      return "object";
    case SymbolKind::SectionLabel:
      return "section";
    case SymbolKind::File:
      return "file";
    case SymbolKind::Tls:
      return "tls";
    case SymbolKind::IndirectFunction:
      return "ifunc";
  }
  return "unknown";
}

std::string_view toString(SymbolBinding binding) noexcept {
  switch (binding) {
    case SymbolBinding::Unknown:
      return "unknown";
    case SymbolBinding::Local:
      return "local";
    case SymbolBinding::Global:
      return "global";
    case SymbolBinding::Weak:
      return "weak";
  }
  return "unknown";
}

std::string_view toString(RelocKind kind) noexcept {
  switch (kind) {
    case RelocKind::Unknown:
      return "unknown";
    case RelocKind::None:
      return "none";
    case RelocKind::Absolute:
      return "absolute";
    case RelocKind::Relative:
      return "relative";
    case RelocKind::GotSlot:
      return "got";
    case RelocKind::JumpSlot:
      return "jump-slot";
    case RelocKind::IndirectFunction:
      return "irelative";
    case RelocKind::TlsModule:
      return "tls-module";
    case RelocKind::TlsOffset:
      return "tls-offset";
    case RelocKind::TlsDescriptor:
      return "tls-descriptor";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// BinaryImage
// ---------------------------------------------------------------------------

BinaryImage::BinaryImage(ImageContents contents) : contents_(std::move(contents)) {
  buildIndices();
}

void BinaryImage::buildIndices() {
  symbolsByAddress_.clear();
  symbolsByAddress_.reserve(contents_.symbols.size());
  maxSymbolSize_ = 0;
  for (std::size_t index = 0; index < contents_.symbols.size(); ++index) {
    const Symbol& symbol = contents_.symbols[index];
    if (symbol.defined && symbol.kind != SymbolKind::File) {
      symbolsByAddress_.push_back(static_cast<uint32_t>(index));
      maxSymbolSize_ = std::max(maxSymbolSize_, symbol.size);
    }
  }
  // Sort by address, then by descending size so that the widest symbol at an
  // address comes first, then prefer exported over local names.
  std::sort(symbolsByAddress_.begin(), symbolsByAddress_.end(),
            [this](uint32_t a, uint32_t b) {
              const Symbol& left = contents_.symbols[a];
              const Symbol& right = contents_.symbols[b];
              if (left.va != right.va) {
                return left.va < right.va;
              }
              if (left.size != right.size) {
                return left.size > right.size;
              }
              return static_cast<int>(left.exported) > static_cast<int>(right.exported);
            });

  relocationsByAddress_.clear();
  relocationsByAddress_.reserve(contents_.relocations.size());
  for (std::size_t index = 0; index < contents_.relocations.size(); ++index) {
    relocationsByAddress_.push_back(static_cast<uint32_t>(index));
  }
  std::sort(relocationsByAddress_.begin(), relocationsByAddress_.end(),
            [this](uint32_t a, uint32_t b) {
              return contents_.relocations[a].va < contents_.relocations[b].va;
            });

  lowestRelocationVa_ = 0;
  highestRelocationEndVa_ = 0;
  maxRelocationWidth_ = 0;
  if (!relocationsByAddress_.empty()) {
    lowestRelocationVa_ = contents_.relocations[relocationsByAddress_.front()].va;
    for (uint32_t index : relocationsByAddress_) {
      const Relocation& relocation = contents_.relocations[index];
      highestRelocationEndVa_ = std::max(highestRelocationEndVa_, relocation.va + relocation.width);
      maxRelocationWidth_ = std::max<uint64_t>(maxRelocationWidth_, relocation.width);
    }
  }
}

const Section* BinaryImage::sectionAt(uint64_t va) const noexcept {
  // Sections may nest or abut; a linear scan is fine at typical section counts
  // and avoids maintaining another index.
  const Section* best = nullptr;
  for (const Section& section : contents_.sections) {
    if (!section.allocated || !section.contains(va)) {
      continue;
    }
    if (best == nullptr || section.size < best->size) {
      best = &section;
    }
  }
  return best;
}

const Section* BinaryImage::sectionNamed(std::string_view name) const noexcept {
  for (const Section& section : contents_.sections) {
    if (section.name == name) {
      return &section;
    }
  }
  return nullptr;
}

const Symbol* BinaryImage::symbolAt(uint64_t va) const noexcept {
  const auto it = std::lower_bound(
      symbolsByAddress_.begin(), symbolsByAddress_.end(), va,
      [this](uint32_t index, uint64_t address) { return contents_.symbols[index].va < address; });
  if (it == symbolsByAddress_.end()) {
    return nullptr;
  }
  const Symbol& symbol = contents_.symbols[*it];
  return symbol.va == va ? &symbol : nullptr;
}

const Symbol* BinaryImage::symbolContaining(uint64_t va) const noexcept {
  // Walk back from the first symbol past `va`, keeping the narrowest cover so
  // that a symbol nested inside a larger one wins. The walk is bounded by the
  // widest symbol in the image: no symbol starting before that can still reach
  // `va`.
  auto it = std::upper_bound(
      symbolsByAddress_.begin(), symbolsByAddress_.end(), va,
      [this](uint64_t address, uint32_t index) { return address < contents_.symbols[index].va; });

  const uint64_t lowestPossibleStart = va >= maxSymbolSize_ ? va - maxSymbolSize_ : 0;
  const Symbol* best = nullptr;
  while (it != symbolsByAddress_.begin()) {
    --it;
    const Symbol& symbol = contents_.symbols[*it];
    if (symbol.va < lowestPossibleStart) {
      break;
    }
    if (symbol.size != 0 && va >= symbol.va && va - symbol.va < symbol.size) {
      if (best == nullptr || symbol.size < best->size) {
        best = &symbol;
      }
    }
  }
  return best;
}

const Symbol* BinaryImage::symbolNamed(std::string_view name) const noexcept {
  for (const Symbol& symbol : contents_.symbols) {
    if (symbol.name == name) {
      return &symbol;
    }
  }
  return nullptr;
}

const Relocation* BinaryImage::relocationAt(uint64_t va) const noexcept {
  const auto it = std::lower_bound(relocationsByAddress_.begin(), relocationsByAddress_.end(), va,
                                   [this](uint32_t index, uint64_t address) {
                                     return contents_.relocations[index].va < address;
                                   });
  if (it == relocationsByAddress_.end()) {
    return nullptr;
  }
  const Relocation& relocation = contents_.relocations[*it];
  return relocation.va == va ? &relocation : nullptr;
}

const Relocation* BinaryImage::relocationOverlapping(uint64_t va, uint64_t size) const noexcept {
  if (size == 0 || relocationsByAddress_.empty()) {
    return nullptr;
  }
  // Cheap rejection for the common case of a code range far from any data
  // relocation, which is what the instruction decoder asks about.
  if (va >= highestRelocationEndVa_ || va + size <= lowestRelocationVa_) {
    return nullptr;
  }

  auto it = std::upper_bound(
      relocationsByAddress_.begin(), relocationsByAddress_.end(), va + size - 1,
      [this](uint64_t address, uint32_t index) { return address < contents_.relocations[index].va; });
  const uint64_t lowestPossibleStart = va >= maxRelocationWidth_ ? va - maxRelocationWidth_ : 0;
  while (it != relocationsByAddress_.begin()) {
    --it;
    const Relocation& relocation = contents_.relocations[*it];
    if (relocation.va < lowestPossibleStart) {
      break;
    }
    if (relocation.va + relocation.width > va && relocation.va < va + size) {
      return &relocation;
    }
  }
  return nullptr;
}

std::optional<std::string_view> BinaryImage::importNameAt(uint64_t va) const noexcept {
  const Relocation* relocation = relocationAt(va);
  if (relocation == nullptr || relocation->symbolIndex == kNoSymbol) {
    return std::nullopt;
  }
  if (relocation->symbolIndex >= contents_.symbols.size()) {
    return std::nullopt;
  }
  const Symbol& symbol = contents_.symbols[relocation->symbolIndex];
  if (symbol.name.empty()) {
    return std::nullopt;
  }
  return std::string_view{symbol.name};
}

bool BinaryImage::isExecutable(uint64_t va) const noexcept {
  const MemoryRegion* region = contents_.memory.regionAt(va);
  return region != nullptr && hasPermission(region->permissions, MemoryPermissions::Execute);
}

bool BinaryImage::isWritable(uint64_t va) const noexcept {
  const MemoryRegion* region = contents_.memory.regionAt(va);
  return region != nullptr && hasPermission(region->permissions, MemoryPermissions::Write);
}

bool BinaryImage::isImmutable(uint64_t va, uint64_t size) const noexcept {
  if (size == 0 || va + size < va) {
    return false;
  }
  // Region by region rather than byte by byte, but every byte still has to be
  // accounted for: a range may legitimately span two adjacent regions, and one
  // of them being read-only says nothing about the other.
  uint64_t at = va;
  const uint64_t end = va + size;
  while (at < end) {
    const MemoryRegion* region = contents_.memory.regionAt(at);
    if (region == nullptr || hasPermission(region->permissions, MemoryPermissions::Write)) {
      return false;
    }
    at = region->endVa();
  }
  const Relocation* relocation = relocationOverlapping(va, size);
  if (relocation == nullptr) {
    return true;
  }
  // A relative rebase is the one relocation whose result this file already
  // knows. The clause exists because a symbol binding depends on which module
  // wins the symbol, which one file cannot say -- but a rebase writes back the
  // slot's own bytes plus the load bias, and the bias is the same one every
  // address in this decompilation is expressed against. Reading `hasValue` as
  // "the loader's write was reconstructed" keeps that narrow: an unresolved
  // rebase, or a bind, still makes the range an observation of memory rather
  // than a constant of the program.
  return relocation->kind == RelocKind::Relative && relocation->hasValue &&
         relocation->va >= va && relocation->va + relocation->width <= va + size;
}

Result<void> BinaryImage::read(uint64_t va, std::span<std::byte> out) const {
  XDEC_TRY_VOID(contents_.memory.read(va, out));

  // Overlay statically resolvable relocations. Without this a pointer slot
  // reads as its unrelocated placeholder, which for RELA-style relocations is
  // usually zero: every jump table walked through such a slot would look empty.
  if (out.empty()) {
    return ok();
  }
  const uint64_t size = out.size();
  if (va >= highestRelocationEndVa_ || va + size <= lowestRelocationVa_) {
    return ok();
  }

  const uint64_t searchStart = va >= maxRelocationWidth_ ? va - maxRelocationWidth_ : 0;
  auto it = std::lower_bound(relocationsByAddress_.begin(), relocationsByAddress_.end(), searchStart,
                             [this](uint32_t index, uint64_t address) {
                               return contents_.relocations[index].va < address;
                             });
  for (; it != relocationsByAddress_.end(); ++it) {
    const Relocation& relocation = contents_.relocations[*it];
    if (relocation.va >= va + size) {
      break;
    }
    if (!relocation.hasValue || relocation.width == 0) {
      continue;
    }
    const uint64_t relocationEnd = relocation.va + relocation.width;
    if (relocationEnd <= va) {
      continue;
    }

    // Splice the overlapping part of the relocated value into the output.
    std::byte encoded[8] = {};
    const unsigned width = std::min<unsigned>(relocation.width, 8);
    for (unsigned index = 0; index < width; ++index) {
      const unsigned shift = contents_.endian == Endian::Little ? index * 8 : (width - 1 - index) * 8;
      encoded[index] = static_cast<std::byte>((relocation.value >> shift) & 0xFF);
    }

    const uint64_t copyStart = std::max(va, relocation.va);
    const uint64_t copyEnd = std::min(va + size, relocationEnd);
    if (copyStart >= copyEnd) {
      continue;
    }
    std::memcpy(out.data() + (copyStart - va), encoded + (copyStart - relocation.va),
                static_cast<std::size_t>(copyEnd - copyStart));
  }

  return ok();
}

Result<uint64_t> BinaryImage::readUnsigned(uint64_t va, unsigned bytes) const {
  if (bytes == 0 || bytes > 8) {
    return err(DiagCode::OutOfRange, std::format("unsupported integer width {} bytes", bytes));
  }
  std::byte buffer[8] = {};
  XDEC_TRY_VOID(read(va, std::span<std::byte>{buffer, bytes}));

  uint64_t value = 0;
  if (contents_.endian == Endian::Little) {
    for (unsigned index = bytes; index-- > 0;) {
      value = (value << 8) | static_cast<uint64_t>(std::to_integer<uint8_t>(buffer[index]));
    }
  } else {
    for (unsigned index = 0; index < bytes; ++index) {
      value = (value << 8) | static_cast<uint64_t>(std::to_integer<uint8_t>(buffer[index]));
    }
  }
  return value;
}

Result<uint64_t> BinaryImage::readPointer(uint64_t va) const {
  const unsigned bytes = pointerBytes();
  if (bytes == 0) {
    return err(DiagCode::UnsupportedArch, "image has no known pointer width");
  }
  return readUnsigned(va, bytes);
}

Result<std::string> BinaryImage::readCString(uint64_t va, std::size_t maxLength) const {
  std::string text;
  text.reserve(std::min<std::size_t>(maxLength, 64));
  for (std::size_t offset = 0; offset < maxLength; ++offset) {
    std::byte byte{};
    XDEC_TRY_VOID(read(va + offset, std::span<std::byte>{&byte, 1}));
    const auto value = std::to_integer<uint8_t>(byte);
    if (value == 0) {
      return text;
    }
    text.push_back(static_cast<char>(value));
  }
  return err(DiagCode::OutOfRange,
             std::format("no NUL terminator within {} bytes of 0x{:x}", maxLength, va));
}

Result<std::span<const std::byte>> BinaryImage::codeView(uint64_t va, uint64_t size) const {
  const std::span<const std::byte> view = contents_.memory.directView(va, size);
  if (view.empty()) {
    if (!contents_.memory.isMapped(va)) {
      return err(Diag{DiagCode::UnmappedAddress,
                      std::format("no mapped region covers code address 0x{:x}", va)}
                     .at(va));
    }
    return err(Diag{DiagCode::OutOfRange,
                    std::format("code range [0x{:x}, 0x{:x}) is not fully backed by file bytes",
                                va, va + size)}
                   .at(va));
  }
  return view;
}

std::vector<const MemoryRegion*> BinaryImage::executableRegions() const {
  std::vector<const MemoryRegion*> result;
  for (const MemoryRegion& region : contents_.memory.regions()) {
    if (hasPermission(region.permissions, MemoryPermissions::Execute)) {
      result.push_back(&region);
    }
  }
  return result;
}

// ---------------------------------------------------------------------------
// Format dispatch
// ---------------------------------------------------------------------------

Result<std::unique_ptr<BinaryImage>> openBinary(const std::filesystem::path& path) {
  XDEC_TRY(FileBuffer file, FileBuffer::fromFile(path));

  const std::span<const std::byte> bytes = file.bytes();
  const auto magic = [&bytes](std::size_t index) -> uint8_t {
    return index < bytes.size() ? std::to_integer<uint8_t>(bytes[index]) : 0;
  };

  if (bytes.size() >= 4 && magic(0) == 0x7F && magic(1) == 'E' && magic(2) == 'L' &&
      magic(3) == 'F') {
    return loadElf(std::move(file), path.string());
  }

  // Every dyld shared cache magic starts with "dyld_v1" followed by a
  // space-padded architecture suffix ("  arm64", " arm64e", ...).
  if (bytes.size() >= 7 && std::memcmp(bytes.data(), "dyld_v1", 7) == 0) {
    return loadDyldCache(std::move(file), path);
  }

  // Recognise the formats we do not handle yet by name, so the diagnostic says
  // "not implemented" rather than "not a binary".
  if (bytes.size() >= 2 && magic(0) == 'M' && magic(1) == 'Z') {
    return err(DiagCode::NotImplemented,
               std::format("'{}' is a PE image; only ELF is implemented", path.string()));
  }
  const uint32_t leadingWord = bytes.size() >= 4 ? (static_cast<uint32_t>(magic(0)) |
                                                    (static_cast<uint32_t>(magic(1)) << 8) |
                                                    (static_cast<uint32_t>(magic(2)) << 16) |
                                                    (static_cast<uint32_t>(magic(3)) << 24))
                                                 : 0;
  if (leadingWord == 0xFEEDFACFu) {
    return loadMachO(std::move(file), path.string());
  }
  if (leadingWord == 0xFEEDFACEu || leadingWord == 0xBEBAFECAu) {
    return err(DiagCode::NotImplemented,
               std::format("'{}' is a 32-bit or fat Mach-O image; only 64-bit little-endian "
                           "Mach-O is implemented",
                           path.string()));
  }

  return err(DiagCode::UnsupportedFormat,
             std::format("'{}' is not a recognised binary format", path.string()));
}

}  // namespace xdec::binary
