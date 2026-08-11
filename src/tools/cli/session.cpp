#include "session.h"

#include <cstdlib>
#include <cstring>

#include "common.h"
#include "xdec/analysis/plt_stub.h"
#include "xdec/spec/compile.h"
#include "xdec/spec/parse.h"
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

xdec::decompile::DriverOptions SessionContext::driverOptions() const {
  xdec::decompile::DriverOptions options;
  options.memory = memory();
  options.types = hasTypes_ ? &types : nullptr;
  options.syscalls = hasSyscalls_ ? &syscalls : nullptr;
  options.names = names();
  return options;
}

}  // namespace xdec::cli
