// The CLI's binary/spec-engine plumbing: opening an image, loading the
// architecture spec, and the small adapters that connect a BinaryImage to
// the pass/emit layers' own view of the world (MemoryFacts, NameAt, ...).
//
// Every command that touches a real binary goes through this file instead of
// repeating "open the image, load the spec" at each call site.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xdec/binary/image.h"
#include "xdec/binary/target_profile.h"
#include "xdec/decompile/driver.h"
#include "xdec/emit/c_printer.h"
#include "xdec/pass/pass.h"
#include "xdec/spec/engine.h"
#include "xdec/support/reader.h"
#include "xdec/support/result.h"
#include "xdec/types/database.h"
#include "xdec/types/syscall_table.h"

namespace xdec::cli {

using xdec::binary::BinaryImage;

xdec::Result<std::unique_ptr<BinaryImage>> open(std::string_view path);

/// Where to find the architecture spec. An explicit XDEC_SPEC wins; otherwise
/// the copy installed next to the binary is used, so the tool works from any
/// working directory.
std::filesystem::path specPath();

/// Loads the spec, from either a compiled blob or source. Which one is decided
/// by the magic rather than the extension, so a stale blob cannot be mistaken
/// for source and produce a parse error twenty lines in.
xdec::Result<std::unique_ptr<xdec::spec::SpecEngine>> loadEngine();

/// Loads one or more headers (paths or preset names) into a single database,
/// stacking them in the order given so a project header can build on a preset.
xdec::Result<xdec::types::TypeDatabase> loadTypes(std::span<const std::string> sources,
                                                   bool verbose);

/// The address-space facts beyond raw bytes that only a real image can state
/// (see pass::Context::setMemoryFacts). The image must outlive every pipeline
/// the result is handed to, which for every caller here is the whole command.
[[nodiscard]] xdec::MemoryFacts memoryFactsOf(const BinaryImage& image);

/// The image's symbol table, as the emitter asks about it (see
/// emit::SymbolResolver). Same lifetime requirement as memoryFactsOf.
[[nodiscard]] xdec::emit::SymbolResolver symbolResolverOf(const BinaryImage& image);

/// Raw bytes, as the emitter's AddressRenderer reads them to recover a
/// constant pointer argument's referent (see emit::COptions::imageReader).
/// Same lifetime requirement as memoryFactsOf.
[[nodiscard]] xdec::ByteReader imageReaderOf(const BinaryImage& image);

/// The same table, as the passes ask about it (see pass::Context::setNames):
/// exact starts only, with one addition beyond the symbol table itself: a PLT
/// stub's address is not named by any symbol in a stripped binary, but the
/// stub's own bytes and the relocation its GOT slot carries are just as exact
/// a fact about what starts there. This is what turns `sub_1d28a0` into
/// `__errno_location` everywhere a name feeds a prototype lookup.
[[nodiscard]] xdec::pass::NameAt nameResolverOf(const BinaryImage& image,
                                                 const xdec::ByteReader& reader,
                                                 const xdec::MemoryFacts& facts,
                                                 const xdec::binary::TargetProfile& profile);

/// Where an address lives and whether it can change (see emit::AddressFacts).
/// Same lifetime requirement as memoryFactsOf.
[[nodiscard]] xdec::emit::AddressDescriber addressDescriberOf(const BinaryImage& image);

/// Bundles the two things almost every binary-facing command needs: the
/// opened image and the loaded architecture spec.
struct ToolSession {
  std::unique_ptr<BinaryImage> image;
  std::unique_ptr<xdec::spec::SpecEngine> engine;

  static xdec::Result<ToolSession> openBinary(std::string_view path);
};

/// What SessionContext::open() loads beyond the image and spec, mirroring
/// `decompile`'s own `--types`/`--syscall-table` flags: a caller that wants
/// the CLI's exact defaulting behaviour passes these through unchanged from
/// option parsing.
struct SessionLoadOptions {
  /// Header paths or preset names, stacked in order (see loadTypes below).
  /// Empty defers to the inferred TargetProfile's own typePresets.
  std::vector<std::string> typeSources;
  /// A preset name, a path, or `SyscallTable::defaultName()` for "use
  /// whatever the build ships" (see types::SyscallTable::resolvePath). Empty
  /// means no syscall table at all, same as `--syscall-table none`.
  std::string syscallSource{xdec::types::SyscallTable::defaultName()};
};

/// Everything a full decompile needs beyond ToolSession, loaded together
/// because they are consulted together: `profile` decides where `types` and
/// `syscalls` come from when the caller did not say (see
/// binary::inferTargetProfile), and driverOptions()/emitOptions() below build
/// the DriverOptions/COptions fields that read them, so a caller cannot wire
/// `types` to the pass pipeline while `syscalls` came from a different run's
/// database -- the shape of bug this replaces (`commandDecompile` built both
/// by hand, once for DriverOptions and once more for COptions, before this
/// existed).
///
/// Deliberately does not decide `target`/`maxRounds`/`fence` -- those are
/// per-call, not per-session (the same session decompiles many addresses,
/// each with its own fence), so driverOptions() leaves them at DriverOptions'
/// own defaults for the caller to set.
struct SessionContext : ToolSession {
  /// What the platform implies about this image (see
  /// binary::inferTargetProfile): its own type presets and syscall table name,
  /// consulted by open() wherever the caller did not already say something
  /// more specific.
  xdec::binary::TargetProfile profile;
  /// Empty (every lookup answers "no header for that") when `load.typeSources`
  /// resolved to nothing -- no `--types` flag and no preset from `profile`.
  xdec::types::TypeDatabase types;
  /// Empty (every lookup answers "no such syscall") when `load.syscallSource`
  /// resolved to nothing, which `--syscall-table none` asks for explicitly.
  xdec::types::SyscallTable syscalls;

  /// Opens `path`, infers its TargetProfile, and loads whatever `load` (or
  /// the profile's own defaults, where `load` left a field to infer) names.
  /// A syscall source the caller named explicitly must load -- they said
  /// which ABI this is, and guessing past a load failure would name syscalls
  /// from the wrong kernel; the default source is different, since it is the
  /// build's own data file, and a broken installation should not stop a
  /// decompilation that never needed it (see cmd_pipeline.cpp's prior inline
  /// version of this same reasoning).
  static xdec::Result<SessionContext> open(std::string_view path,
                                            const SessionLoadOptions& load = SessionLoadOptions{});

  /// What the image says beyond its bytes (see xdec::MemoryFacts). Same
  /// lifetime requirement as memoryFactsOf: the session must outlive it.
  [[nodiscard]] const xdec::MemoryFacts& memory() const { return memory_; }
  /// Raw image bytes (see xdec::ByteReader). Same lifetime requirement.
  [[nodiscard]] const xdec::ByteReader& reader() const { return reader_; }
  /// The image's symbol table plus PLT-stub import aliasing, aliased through
  /// `profile` the same way nameResolverOf always has (see its own doc
  /// comment). Same lifetime requirement.
  ///
  /// Built from `reader_`/`memory_` (this session's own persistent members),
  /// never from a fresh reader()/memory() call: nameResolverOf's returned
  /// closure captures its `reader`/`facts` arguments *by reference*, so
  /// binding it to a temporary -- what reader()/memory() would hand back --
  /// would leave it pointing at an already-destroyed object the moment this
  /// method returns. Passing the members themselves keeps the reference
  /// valid for as long as this session is.
  [[nodiscard]] xdec::pass::NameAt names() const {
    return nameResolverOf(*image, reader_, memory_, profile);
  }
  /// Where an address lives and whether it can change (see
  /// emit::AddressDescriber). Same lifetime requirement.
  [[nodiscard]] xdec::emit::AddressDescriber addresses() const {
    return addressDescriberOf(*image);
  }
  /// How to name a callee/the function itself (see emit::SymbolResolver).
  /// Same lifetime requirement.
  [[nodiscard]] xdec::emit::SymbolResolver symbols() const { return symbolResolverOf(*image); }

  /// A DriverOptions with `memory`/`types`/`syscalls`/`names` set from this
  /// session -- everything decompile()/decompileToC() need that this session
  /// alone determines. The caller still sets `target`, round/fence options,
  /// and anything else that varies per call.
  [[nodiscard]] xdec::decompile::DriverOptions driverOptions() const;

 private:
  bool hasTypes_ = false;
  bool hasSyscalls_ = false;
  /// Built once, in open(): every closure this session hands out
  /// (names()/driverOptions()) captures these by reference (see names()'s
  /// own doc comment), so they must be stable member storage, not a fresh
  /// memoryFactsOf(*image)/imageReaderOf(*image) each time they are read.
  xdec::MemoryFacts memory_;
  xdec::ByteReader reader_;
};

}  // namespace xdec::cli
