#include "session.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <system_error>

#include "common.h"
#include "xdec/analysis/plt_stub.h"
#include "xdec/binary/macho_metadata.h"
#include "xdec/obs/job.h"
#include "xdec/obs/span.h"
#include "xdec/spec/compile.h"
#include "xdec/spec/parse.h"
#include "xdec/support/composite_reader.h"
#include "xdec/support/paths.h"
#include "xdec/types/parse.h"

namespace xdec::cli {

xdec::Result<std::unique_ptr<BinaryImage>> open(std::string_view path) {
  xdec::obs::reportProgress("load", std::format("loading {}", path));
  const xdec::obs::Span span{"load", "image", xdec::obs::kv("path", path)};
  return xdec::binary::openBinary(std::filesystem::path{path});
}

std::filesystem::path specPath() {
  if (const char* fromEnv = std::getenv("XDEC_SPEC"); fromEnv != nullptr && *fromEnv != '\0') {
    return std::filesystem::path{fromEnv};
  }
  return xdec::findShippedDataFile(std::filesystem::path{"specs"} / "arm64.xspec",
                                   XDEC_SPEC_DIR, "arm64.xspec");
}

xdec::Result<std::unique_ptr<xdec::spec::SpecEngine>> loadEngine() {
  const std::filesystem::path path = specPath();
  const xdec::obs::Span span{"load", "spec", xdec::obs::kv("path", path.string())};
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

xdec::Result<xdec::types::TypeDatabase> loadTypes(std::span<const std::string> sources,
                                                   bool verbose) {
  xdec::types::TypeDatabase database;
  for (const std::string& source : sources) {
    for (const std::string& layer : xdec::types::expandPresetBundle(source)) {
      const xdec::obs::Span span{"load", "types", xdec::obs::kv("layer", layer)};
      XDEC_TRY(const std::string resolved, xdec::types::resolveHeaderPath(layer));
      XDEC_TRY(const xdec::types::ParseReport report,
               xdec::types::parseHeaderFile(resolved, database));
      if (verbose) {
        print("{}", report.format(layer));
      } else if (report.skipped != 0) {
        note("{}", report.format(layer));
      }
    }
  }
  return database;
}

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

[[nodiscard]] xdec::ByteReader imageReaderOf(const BinaryImage& image) {
  return [&image](uint64_t va, std::span<std::byte> out) { return image.read(va, out); };
}

[[nodiscard]] xdec::emit::SymbolResolver symbolResolverOf(const BinaryImage& image) {
  const auto* machoMeta =
      dynamic_cast<const xdec::binary::MachOMetadata*>(image.formatMetadata());
  return [&image, machoMeta](uint64_t va) {
    xdec::emit::SymbolRef out;
    // Containing, not exact: the offset is half the answer (see emit::SymbolRef),
    // and an exact hit is just the containing symbol with an offset of zero.
    const xdec::binary::Symbol* symbol = image.symbolContaining(va);
    if (symbol != nullptr && symbol->va == va) {
      out.name = symbol->name;
      out.offset = 0;
      out.isFunction = symbol->kind == xdec::binary::SymbolKind::Function;
      return out;
    }
    if (machoMeta != nullptr) {
      if (!image.isExecutable(va)) {
        if (const auto dataName = machoMeta->resolveDataSymbolAt(image, va)) {
          out.name = *dataName;
          out.offset = 0;
          out.isFunction = false;
          return out;
        }
      } else {
        if (const auto method = machoMeta->objcMethodAt(image, va)) {
          out.name = method->cName;
          out.displayName = method->displayName;
          out.typeEncoding = method->typeEncoding;
          out.offset = 0;
          out.isFunction = true;
          return out;
        }
      }
    }
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
namespace {
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
}  // namespace

[[nodiscard]] xdec::pass::NameAt nameResolverOf(const BinaryImage& image,
                                                 const xdec::ByteReader& reader,
                                                 const xdec::MemoryFacts& facts,
                                                 const xdec::binary::TargetProfile& profile) {
  const auto* machoMeta =
      dynamic_cast<const xdec::binary::MachOMetadata*>(image.formatMetadata());
  return [&image, machoMeta, &reader, &facts, &profile](uint64_t va) {
    xdec::pass::SymbolName out;
    const xdec::binary::Symbol* symbol = image.symbolContaining(va);
    if (symbol != nullptr && symbol->va == va) {
      out.name = symbol->name;
      out.isFunction = symbol->kind == xdec::binary::SymbolKind::Function;
      return out;
    }
    if (machoMeta != nullptr && image.isExecutable(va)) {
      if (const auto method = machoMeta->objcMethodAt(image, va)) {
        out.name = method->cName;
        out.isFunction = true;
        return out;
      }
    }
    if (const std::optional<std::string> imported = pltImportAt(reader, facts, profile, va)) {
      out.name = *imported;
      out.isFunction = true;
      return out;
    }
    // Chase pure unconditional ARM64 `B <target>` linker branch islands so
    // calls across >128MB Mach-O segments name their destination (`j_<name>` /
    // `j_sub_<target>`) instead of an opaque island address.
    uint64_t cur = va;
    bool chased = false;
    for (unsigned hop = 0; hop < 6; ++hop) {
      std::array<std::byte, 4> bytes{};
      if (!reader(cur, std::span<std::byte>{bytes})) {
        break;
      }
      const uint32_t w =
          static_cast<uint32_t>(static_cast<uint8_t>(bytes[0])) |
          (static_cast<uint32_t>(static_cast<uint8_t>(bytes[1])) << 8) |
          (static_cast<uint32_t>(static_cast<uint8_t>(bytes[2])) << 16) |
          (static_cast<uint32_t>(static_cast<uint8_t>(bytes[3])) << 24);
      if ((w & 0xfc000000u) != 0x14000000u) {
        break;
      }
      const uint32_t imm26 = w & 0x03ffffffu;
      const int32_t sext = static_cast<int32_t>(imm26 << 6) >> 6;
      cur = cur + static_cast<uint64_t>(static_cast<int64_t>(sext) << 2);
      chased = true;
      const xdec::binary::Symbol* destSym = image.symbolContaining(cur);
      if (destSym != nullptr && destSym->va == cur &&
          destSym->kind == xdec::binary::SymbolKind::Function) {
        out.name = "j_" + destSym->name;
        out.isFunction = true;
        return out;
      }
      if (machoMeta != nullptr && image.isExecutable(cur)) {
        if (const auto method = machoMeta->objcMethodAt(image, cur)) {
          out.name = "j_" + method->cName;
          out.isFunction = true;
          return out;
        }
      }
    }
    if (chased && image.isExecutable(cur)) {
      out.name = std::format("j_sub_{:x}", cur);
      out.isFunction = true;
    }
    return out;
  };
}

[[nodiscard]] xdec::emit::AddressDescriber addressDescriberOf(const BinaryImage& image) {
  const auto* machoMeta =
      dynamic_cast<const xdec::binary::MachOMetadata*>(image.formatMetadata());
  return [&image, machoMeta](uint64_t va) {
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
    out.cstringSection =
        section->name == "__cstring" || section->name == "__oslogstring" ||
        section->name.starts_with(".rodata.str") ||
        (image.format() == xdec::binary::BinaryFormat::MachO && (section->rawType & 0xffu) == 2u);
    if (machoMeta != nullptr && section->name == "__cfstring") {
      out.cfstringLiteral = machoMeta->cfStringAt(image, va);
    }
    if (const std::optional<std::string_view> imported = image.importNameAt(va)) {
      out.importName = *imported;
    }
    return out;
  };
}

xdec::Result<ToolSession> ToolSession::openBinary(std::string_view path) {
  XDEC_TRY(std::unique_ptr<BinaryImage> image, open(path));
  XDEC_TRY(std::unique_ptr<xdec::spec::SpecEngine> engine, loadEngine());
  return ToolSession{std::move(image), std::move(engine)};
}

xdec::Result<SessionContext> SessionContext::open(std::string_view path,
                                                   const SessionLoadOptions& load) {
  XDEC_TRY(ToolSession session, ToolSession::openBinary(path));

  SessionContext ctx;
  static_cast<ToolSession&>(ctx) = std::move(session);
  // Built once and kept as members: everything below that closes over "the
  // image's bytes" or "what it says about an address" (names(),
  // driverOptions()) captures these by reference, and a fresh one recomputed
  // on every call would leave those closures pointing at a destroyed
  // temporary (see SessionContext::names()'s doc comment).
  ctx.memory_ = memoryFactsOf(*ctx.image);
  ctx.reader_ = imageReaderOf(*ctx.image);
  ctx.profile = xdec::binary::inferTargetProfile(*ctx.image);
  ctx.buildEntryRegFacts(std::filesystem::path{path});

  // Platform declarations are the base; a project header is an overlay, not
  // a replacement for libc/Mach/CoreFoundation. This also means spelling one
  // `--types project.hdecl` no longer silently disables the automatically
  // inferred SDK.
  std::vector<std::string> typeSources = ctx.profile.typePresets;
  for (const std::string& source : load.typeSources) {
    if (std::find(typeSources.begin(), typeSources.end(), source) == typeSources.end()) {
      typeSources.push_back(source);
    }
  }
  ctx.hasTypes_ = !typeSources.empty();
  if (ctx.hasTypes_) {
    XDEC_TRY(xdec::types::TypeDatabase loaded, loadTypes(typeSources, /*verbose=*/false));
    ctx.types = std::move(loaded);
    std::size_t headerCount = 0;
    for (const std::string& source : typeSources) {
      headerCount += xdec::types::expandPresetBundle(source).size();
    }
    note("types: {} type(s), {} declaration(s) from {} header(s)", ctx.types.typeCount(),
          ctx.types.declarations().size(), headerCount);
  }

  std::string syscallSource = load.syscallSource;
  if (syscallSource == xdec::types::SyscallTable::defaultName() &&
      !ctx.profile.syscallTable.empty()) {
    syscallSource = ctx.profile.syscallTable;
  }
  if (!syscallSource.empty()) {
    const auto loadSyscalls = [&]() -> xdec::Result<xdec::types::SyscallTable> {
      XDEC_TRY(const std::string resolved,
               xdec::types::SyscallTable::resolvePath(syscallSource));
      return xdec::types::SyscallTable::loadFile(resolved);
    };
    auto loaded = loadSyscalls();
    if (!loaded) {
      // A table the user named must load: they said which ABI this is, and
      // guessing past that would name syscalls from the wrong kernel. The
      // default is different -- it is the build's own data file, and a broken
      // installation should not stop a decompilation that never needed it.
      if (syscallSource != xdec::types::SyscallTable::defaultName()) {
        return xdec::err(std::move(loaded).error());
      }
      note("note: no syscall table ({}); svc will print as __xdec_syscall",
            loaded.error().format());
    } else {
      ctx.syscalls = std::move(*loaded);
      ctx.hasSyscalls_ = true;
    }
  }
  // Both loaded independently (the syscall table has no reason to depend on
  // whether a header was given, see SyscallTable::resolveTypes), so the link
  // between them is made here, once, rather than by either constructor.
  if (ctx.hasTypes_ && ctx.hasSyscalls_) {
    ctx.syscalls.resolveTypes(ctx.types);
  }
  return ctx;
}

void SessionContext::buildEntryRegFacts(const std::filesystem::path& binaryPath) {
  // Platform formulas and literals, true of this platform in general (see
  // binary::TargetProfile::entryRegLiterals/entryRegOffsets).
  for (const auto& [reg, value] : profile.entryRegLiterals) {
    entryRegs_.setBinding(reg, xdec::analysis::EntryRegBinding::fromLiteral(value));
  }
  for (const auto& offset : profile.entryRegOffsets) {
    entryRegs_.setBinding(
        offset.reg, xdec::analysis::EntryRegBinding::fromBase(offset.companion, offset.offset));
  }

  // Every companion name a binding actually needs, in first-seen order.
  std::vector<std::string> neededCompanions;
  for (const auto& offset : profile.entryRegOffsets) {
    if (std::find(neededCompanions.begin(), neededCompanions.end(), offset.companion) ==
        neededCompanions.end()) {
      neededCompanions.push_back(offset.companion);
    }
  }

  xdec::CompositeByteReader composite;
  composite.addRegion({.name = "", .reader = imageReaderOf(*image), .runtimeBase = 0, .fileBase = 0});

  for (const std::string& name : neededCompanions) {
    // Convention, not configuration: a companion the platform names lives
    // next to the binary under its own name (a desktop dyld dump beside the
    // absd it belongs to, say).
    std::filesystem::path companionPath;
    const std::filesystem::path directory = binaryPath.parent_path();
    for (const std::filesystem::path& guess : {directory / name, directory / (name + ".bin")}) {
      std::error_code ec;
      if (std::filesystem::is_regular_file(guess, ec) && !ec) {
        companionPath = guess;
        break;
      }
    }
    if (companionPath.empty()) {
      detail::say(obs::LogLevel::Debug,
                  std::format("no companion image named '{}' found next to {}; entry "
                              "register(s) anchored to it stay unresolved",
                              name, binaryPath.string()));
      continue;
    }
    auto companionImage = xdec::binary::openBinary(companionPath);
    if (!companionImage) {
      note("note: companion image '{}' at {} did not open ({}); entry register(s) "
           "anchored to it stay unresolved",
           name, companionPath.string(), companionImage.error().format());
      continue;
    }
    const uint64_t runtimeBase = (*companionImage)->memory().lowestAddress();
    entryRegs_.setCompanionBase(name, runtimeBase);
    composite.addRegion({.name = name,
                         .reader = imageReaderOf(**companionImage),
                         .runtimeBase = runtimeBase,
                         .fileBase = runtimeBase});
    companions_.push_back(std::move(*companionImage));
  }

  // Composing zero extra regions would change nothing; only replace the
  // plain primary-image reader once a companion actually joined it.
  if (composite.regionCount() > 1) {
    reader_ = composite.reader();
  }
}

xdec::decompile::DriverOptions SessionContext::driverOptions() const {
  xdec::decompile::DriverOptions options;
  options.memory = memory();
  options.types = hasTypes_ ? &types : nullptr;
  options.syscalls = hasSyscalls_ ? &syscalls : nullptr;
  options.names = names();
  options.entryRegs = entryRegs_.empty() ? nullptr : &entryRegs_;
  return options;
}

}  // namespace xdec::cli
