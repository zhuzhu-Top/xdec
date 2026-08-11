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
#include "xdec/emit/c_printer.h"
#include "xdec/pass/pass.h"
#include "xdec/spec/engine.h"
#include "xdec/support/reader.h"
#include "xdec/support/result.h"
#include "xdec/types/database.h"

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

}  // namespace xdec::cli
