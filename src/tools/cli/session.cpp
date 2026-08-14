#include "session.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <system_error>

#include "common.h"
#include "xdec/analysis/plt_stub.h"
#include "xdec/spec/compile.h"
#include "xdec/spec/parse.h"
#include "xdec/support/composite_reader.h"
#include "xdec/types/parse.h"

namespace xdec::cli {

xdec::Result<std::unique_ptr<BinaryImage>> open(std::string_view path) {
  return xdec::binary::openBinary(std::filesystem::path{path});
}

std::filesystem::path specPath() {
  if (const char* fromEnv = std::getenv("XDEC_SPEC"); fromEnv != nullptr && *fromEnv != '\0') {
    return std::filesystem::path{fromEnv};
  }
  return std::filesystem::path{XDEC_SPEC_DIR} / "arm64.xspec";
}

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

  std::vector<std::string> typeSources = load.typeSources;
  if (typeSources.empty()) {
    typeSources = ctx.profile.typePresets;
  }
  ctx.hasTypes_ = !typeSources.empty();
  if (ctx.hasTypes_) {
    XDEC_TRY(xdec::types::TypeDatabase loaded, loadTypes(typeSources, /*verbose=*/false));
    ctx.types = std::move(loaded);
    print("types: {} type(s), {} declaration(s) from {} header(s)", ctx.types.typeCount(),
          ctx.types.declarations().size(), typeSources.size());
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
      print("note: no syscall table ({}); svc will print as __xdec_syscall",
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
  // Platform defaults first, lowest priority: a sidecar's own measurement
  // (below) overrides these per binary, the same relationship
  // TargetProfile::typePresets has with an explicit `--types`.
  for (const auto& [reg, value] : profile.entryRegLiterals) {
    entryRegs_.setBinding(reg, xdec::analysis::EntryRegBinding::fromLiteral(value));
  }
  for (const auto& offset : profile.entryRegOffsets) {
    entryRegs_.setBinding(
        offset.reg, xdec::analysis::EntryRegBinding::fromBase(offset.companion, offset.offset));
  }

  // Unlike the platform defaults above, a sidecar is always worth looking
  // for even when the platform itself leaks nothing: it may carry
  // per-function facts a platform profile has no business knowing --
  // a captured argument pointer and the bytes it pointed to (MemorySeed),
  // say -- rather than a platform-wide register leak.
  //
  // `XDEC_ENTRY_SIDECAR` mirrors `XDEC_SPEC` above: an explicit override for
  // the one-off/CI case, ahead of the per-binary file convention.
  std::optional<xdec::analysis::EntrySidecar> sidecar;
  std::filesystem::path sidecarPath;
  if (const char* fromEnv = std::getenv("XDEC_ENTRY_SIDECAR");
      fromEnv != nullptr && *fromEnv != '\0') {
    sidecarPath = std::filesystem::path{fromEnv};
  } else if (const auto discovered = xdec::analysis::discoverEntrySidecar(binaryPath)) {
    sidecarPath = *discovered;
  }
  if (!sidecarPath.empty()) {
    auto loaded = xdec::analysis::loadEntrySidecar(sidecarPath);
    if (!loaded) {
      // Auto-discovered, not requested: a malformed sidecar should not stop a
      // decompilation that never asked for one, same reasoning as the
      // default syscall table above. Degrades to platform defaults only.
      print("note: entry sidecar {} did not parse ({}); using platform defaults only",
            sidecarPath.string(), loaded.error().format());
    } else {
      sidecar = std::move(*loaded);
      // Highest priority: a measurement of this exact binary/device beats
      // any formula, the same way an explicit CLI flag would have.
      for (const auto& [reg, value] : sidecar->literals) {
        entryRegs_.setBinding(reg, xdec::analysis::EntryRegBinding::fromLiteral(value));
      }
      for (const xdec::analysis::MemorySeed& seed : sidecar->memorySeeds) {
        entryRegs_.addMemorySeed(seed);
      }
    }
  }

  // Every companion name a binding actually needs, in first-seen order, so a
  // sidecar naming a companion this platform's profile never references is
  // never opened for nothing.
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
    std::filesystem::path companionPath;
    std::optional<uint64_t> explicitBase;
    if (sidecar.has_value()) {
      for (const xdec::analysis::EntryCompanion& candidate : sidecar->companions) {
        if (candidate.name == name) {
          // Relative to the binary's own directory, not the process's CWD:
          // a sidecar is meant to travel with the binary it describes (see
          // the exported "dyld" convention in ios_lldb_absd_entry.py), and a
          // bare filename in it should mean "next to me", the same as the
          // default-discovery guess just below would have found.
          companionPath = candidate.path.is_absolute()
                             ? candidate.path
                             : binaryPath.parent_path() / candidate.path;
          explicitBase = candidate.runtimeBase;
          break;
        }
      }
    }
    if (companionPath.empty()) {
      // Convention, not configuration: a companion the platform names lives
      // next to the binary under its own name (a desktop dyld dump beside
      // the absd it belongs to, say). No sidecar is needed for this to work
      // at all, only to correct it (a measured runtime base, a different
      // path).
      const std::filesystem::path directory = binaryPath.parent_path();
      for (const std::filesystem::path& guess : {directory / name, directory / (name + ".bin")}) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(guess, ec) && !ec) {
          companionPath = guess;
          break;
        }
      }
    }
    if (companionPath.empty()) {
      print("note: no companion image named '{}' found next to {} (and none given in a "
            "sidecar); entry register(s) anchored to it stay unresolved",
            name, binaryPath.string());
      continue;
    }
    auto companionImage = xdec::binary::openBinary(companionPath);
    if (!companionImage) {
      print("note: companion image '{}' at {} did not open ({}); entry register(s) "
            "anchored to it stay unresolved",
            name, companionPath.string(), companionImage.error().format());
      continue;
    }
    const uint64_t preferredBase = (*companionImage)->memory().lowestAddress();
    const uint64_t runtimeBase = explicitBase.value_or(preferredBase);
    entryRegs_.setCompanionBase(name, runtimeBase);
    composite.addRegion({.name = name,
                         .reader = imageReaderOf(**companionImage),
                         .runtimeBase = runtimeBase,
                         .fileBase = preferredBase});
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
