// dyld shared cache loader.
//
// Scope, deliberately: 64-bit little-endian "arm64"/"arm64e" caches, in
// either their single-file historical shape or the multi-file shape every
// cache has shipped as since subcaches were introduced (a main file plus
// sibling parts). Two subcache-array encodings exist on disk --
// `dyld_subcache_entry_v1` (uuid + cacheVMOffset, siblings named
// `basename.1` .. `basename.N`) and `dyld_subcache_entry` (adds an explicit
// `fileSuffix`, e.g. `basename.05.development`) -- and are told apart by
// probing the second one's suffix field for a plausible C string; see
// `detectSubCacheEntrySize`.
//
// Three architectural choices mirror macho.cpp and elf.cpp on purpose:
//
//  * The memory map is the runtime view, built once from every part's own
//    `dyld_cache_mapping_info` table -- exactly the invariant MemoryMap
//    already promises everywhere else, just fed from several backing files
//    instead of one (see backing_store.h).
//
//  * The header is read as named-offset fields with an explicit bound
//    (`headerCovers`), never as a fixed-size struct cast, because the format
//    has only ever grown by appending fields: a cache built by an older dyld
//    has a shorter header, and trusting bytes past its true end means
//    reading into the mapping table that immediately follows it.
//
//  * Format-specific facts that no other loader has an opinion on -- cache
//    UUID, platform, the image (dylib) index -- live in DyldCacheMetadata
//    rather than growing ImageContents.
#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "xdec/binary/dyld_cache_metadata.h"
#include "xdec/binary/image.h"
#include "xdec/support/log.h"

#include "dyld_cache_format.h"
#include "field_reader.h"

namespace xdec::binary {

XDEC_DECLARE_LOG_CATEGORY(logBinary)

namespace {

using namespace dyldcache;  // NOLINT(google-build-using-namespace) - constants only

/// Whether a header field ending at `fieldEnd` is trustworthy: true when the
/// mapping table (which always sits immediately after the header) starts at
/// or past it. A cache whose dyld predates that field has a shorter header,
/// and `fieldEnd` bytes in would already be mapping-table bytes.
bool headerCovers(uint32_t mappingOffset, uint64_t fieldEnd) noexcept {
  return mappingOffset >= fieldEnd;
}

MemoryPermissions permissionsFromProt(uint32_t initProt) noexcept {
  MemoryPermissions permissions = MemoryPermissions::None;
  if ((initProt & kVmProtRead) != 0) permissions |= MemoryPermissions::Read;
  if ((initProt & kVmProtWrite) != 0) permissions |= MemoryPermissions::Write;
  if ((initProt & kVmProtExecute) != 0) permissions |= MemoryPermissions::Execute;
  return permissions;
}

CacheUuid readUuid(FieldReader& reader, uint64_t offset) {
  CacheUuid uuid{};
  const std::span<const std::byte> bytes = reader.slice(offset, kHdrUuidSize);
  if (bytes.size() == kHdrUuidSize) {
    std::memcpy(uuid.data(), bytes.data(), kHdrUuidSize);
  }
  return uuid;
}

bool isZeroUuid(const CacheUuid& uuid) noexcept {
  return std::all_of(uuid.begin(), uuid.end(), [](uint8_t byte) { return byte == 0; });
}

/// One physical file's own `dyld_cache_header`, decoded just enough to place
/// its mappings and (for the main file only) its cache-wide identity. Every
/// part -- main file, subcache, or symbols file -- starts with a full header
/// of this shape; only the main file's non-mapping fields (uuid, platform,
/// subCacheArray, images, ...) are cache-wide and used.
struct PartHeader {
  std::string magic;
  uint32_t mappingOffset = 0;
  uint32_t mappingCount = 0;
  uint64_t localSymbolsOffset = 0;
  uint64_t localSymbolsSize = 0;
  CacheUuid uuid{};
  uint64_t cacheType = kCacheTypeUniversal;
  uint64_t imagesTextOffset = 0;
  uint64_t imagesTextCount = 0;
  uint32_t platform = 0;
  uint32_t formatVersionBits = 0;
  uint64_t sharedRegionStart = 0;
  uint64_t sharedRegionSize = 0;
  uint64_t maxSlide = 0;
  uint32_t subCacheArrayOffset = 0;
  uint32_t subCacheArrayCount = 0;
  CacheUuid symbolFileUuid{};
  uint32_t imagesOffset = 0;
  uint32_t imagesCount = 0;
};

Result<PartHeader> parsePartHeader(std::span<const std::byte> bytes, std::string_view diagName) {
  FieldReader reader(bytes, Endian::Little);
  PartHeader header;

  if (bytes.size() < kHdrMinimumSize) {
    return err(DiagCode::BadFormat,
               std::format("'{}' is shorter than a dyld cache header ({} of {} bytes)", diagName,
                           bytes.size(), kHdrMinimumSize));
  }

  const std::string_view magicView{reinterpret_cast<const char*>(bytes.data()),
                                   std::min<std::size_t>(bytes.size(), kMagicSize)};
  header.magic = std::string{magicView.substr(0, magicView.find('\0'))};

  header.mappingOffset = static_cast<uint32_t>(reader.u32(kHdrMappingOffset));
  header.mappingCount = static_cast<uint32_t>(reader.u32(kHdrMappingCount));
  header.localSymbolsOffset = reader.u64(kHdrLocalSymbolsOffset);
  header.localSymbolsSize = reader.u64(kHdrLocalSymbolsSize);
  header.uuid = readUuid(reader, kHdrUuid);
  header.cacheType = reader.u64(kHdrCacheType);
  header.imagesTextOffset = reader.u64(kHdrImagesTextOffset);
  header.imagesTextCount = reader.u64(kHdrImagesTextCount);
  header.platform = static_cast<uint32_t>(reader.u32(kHdrPlatform));
  header.formatVersionBits = static_cast<uint32_t>(reader.u32(kHdrFormatVersionBits));
  header.sharedRegionStart = reader.u64(kHdrSharedRegionStart);
  header.sharedRegionSize = reader.u64(kHdrSharedRegionSize);
  header.maxSlide = reader.u64(kHdrMaxSlide);

  if (headerCovers(header.mappingOffset, kHdrSubCacheArrayCount + 4)) {
    header.subCacheArrayOffset = static_cast<uint32_t>(reader.u32(kHdrSubCacheArrayOffset));
    header.subCacheArrayCount = static_cast<uint32_t>(reader.u32(kHdrSubCacheArrayCount));
  }
  if (headerCovers(header.mappingOffset, kHdrSymbolFileUuid + kHdrUuidSize)) {
    header.symbolFileUuid = readUuid(reader, kHdrSymbolFileUuid);
  }
  if (headerCovers(header.mappingOffset, kHdrImagesCount + 4)) {
    header.imagesOffset = static_cast<uint32_t>(reader.u32(kHdrImagesOffset));
    header.imagesCount = static_cast<uint32_t>(reader.u32(kHdrImagesCount));
  }

  if (reader.failed()) {
    return err(DiagCode::BadFormat, std::format("truncated dyld cache header in '{}'", diagName));
  }
  if (!header.magic.starts_with("dyld_v1")) {
    return err(DiagCode::BadFormat,
               std::format("'{}' has magic '{}', not a dyld_v1 shared cache", diagName, header.magic));
  }
  return header;
}

/// The magic's architecture suffix is space-padded ("dyld_v1   arm64",
/// "dyld_v1  arm64e"); check the longer name first since it contains the
/// shorter one as a substring.
Result<Arch> archFromMagic(std::string_view magic) {
  if (magic.find("arm64e") != std::string_view::npos) {
    return Arch::AArch64;
  }
  if (magic.find("arm64") != std::string_view::npos) {
    return Arch::AArch64;
  }
  return err(DiagCode::UnsupportedArch,
             std::format("dyld cache magic '{}' is not an arm64/arm64e cache; only those are "
                         "implemented",
                         magic));
}

/// Distinguishes `dyld_subcache_entry_v1` (24 bytes) from `dyld_subcache_entry`
/// (56 bytes, with a `fileSuffix[32]`) by checking whether the second
/// interpretation's suffix field looks like a short NUL-terminated printable
/// string. Neither header carries an explicit version tag for this, so this
/// is the same probe real cache tooling uses.
unsigned detectSubCacheEntrySize(std::span<const std::byte> bytes, uint32_t offset, uint32_t count) {
  if (count == 0) {
    return kSubCacheV1RecordSize;
  }
  FieldReader reader(bytes, Endian::Little);
  const std::string_view suffix = reader.cstring(offset + kSubCacheV2FileSuffix);
  if (reader.failed() || suffix.empty() || suffix.size() >= kSubCacheV2FileSuffixSize) {
    return kSubCacheV1RecordSize;
  }
  const bool printable = std::all_of(suffix.begin(), suffix.end(), [](char c) {
    return c == '.' || c == '-' || c == '_' || std::isalnum(static_cast<unsigned char>(c)) != 0;
  });
  return printable ? kSubCacheV2RecordSize : kSubCacheV1RecordSize;
}

struct SubCacheEntry {
  CacheUuid uuid{};
  uint64_t vmOffset = 0;
  std::string fileSuffix;  // empty when the v1 (numbered-sibling) format was used
};

class DyldCacheLoader {
 public:
  DyldCacheLoader(FileBuffer mainFile, std::filesystem::path mainPath)
      : mainPath_(std::move(mainPath)) {
    store_.addPart(mainPath_.filename().string(), std::move(mainFile));
  }

  Result<std::unique_ptr<BinaryImage>> load() {
    XDEC_TRY(PartHeader mainHeader, parsePartHeader(store_.bytes(0), mainPath_.filename().string()));
    XDEC_TRY(Arch arch, archFromMagic(mainHeader.magic));
    arch_ = arch;
    mainHeader_ = mainHeader;

    metadata_ = std::make_unique<DyldCacheMetadata>();
    metadata_->uuid = mainHeader.uuid;
    metadata_->cacheType = mainHeader.cacheType == kCacheTypeProduction   ? DyldCacheType::Production
                           : mainHeader.cacheType == kCacheTypeDevelopment ? DyldCacheType::Development
                           : mainHeader.cacheType == kCacheTypeUniversal   ? DyldCacheType::Universal
                                                                           : DyldCacheType::Unknown;
    metadata_->platform = mainHeader.platform;
    metadata_->formatVersion = mainHeader.formatVersionBits & kFormatVersionMask;
    metadata_->builtFromChainedFixups =
        (mainHeader.formatVersionBits & kBuiltFromChainedFixupsBit) != 0;
    metadata_->sharedRegionStart = mainHeader.sharedRegionStart;
    metadata_->sharedRegionSize = mainHeader.sharedRegionSize;
    metadata_->maxSlide = mainHeader.maxSlide;
    metadata_->parts.push_back(
        DyldCachePartInfo{.fileName = mainPath_.filename().string(), .uuid = mainHeader.uuid,
                          .vmOffset = 0, .isSymbolsFile = false});

    XDEC_TRY_VOID(discoverParts());
    XDEC_TRY_VOID(buildMemoryMap());
    parseImages();

    return finish();
  }

 private:
  // -- part discovery -------------------------------------------------------

  Result<void> discoverParts() {
    if (mainHeader_.subCacheArrayCount == 0) {
      // No subcaches: everything the cache maps lives in the main file, the
      // historical (pre-split) shape.
      maybeOpenSymbolsFile();
      return ok();
    }

    const std::span<const std::byte> mainBytes = store_.bytes(0);
    const unsigned entrySize = detectSubCacheEntrySize(
        mainBytes, mainHeader_.subCacheArrayOffset, mainHeader_.subCacheArrayCount);
    FieldReader reader(mainBytes, Endian::Little);

    for (uint32_t index = 0; index < mainHeader_.subCacheArrayCount; ++index) {
      const uint64_t entryOffset = mainHeader_.subCacheArrayOffset +
                                   static_cast<uint64_t>(index) * entrySize;
      SubCacheEntry entry;
      entry.uuid = readUuid(reader, entryOffset + kSubCacheV1Uuid);
      entry.vmOffset = reader.u64(entryOffset + kSubCacheV1VmOffset);
      if (entrySize == kSubCacheV2RecordSize) {
        entry.fileSuffix = std::string{reader.cstring(entryOffset + kSubCacheV2FileSuffix)};
      }
      if (reader.failed()) {
        return err(DiagCode::BadFormat,
                   std::format("truncated subCacheArray entry {} in '{}'", index,
                               mainPath_.filename().string()));
      }

      const std::filesystem::path partPath =
          entry.fileSuffix.empty()
              ? std::filesystem::path{mainPath_.string() + "." + std::to_string(index + 1)}
              : std::filesystem::path{mainPath_.string() + entry.fileSuffix};
      XDEC_TRY(FileBuffer partFile, FileBuffer::fromFile(partPath));
      XDEC_TRY(PartHeader partHeader, parsePartHeader(partFile.bytes(), partPath.filename().string()));

      const std::size_t backingIndex = store_.addPart(partPath.filename().string(), std::move(partFile));
      partHeaders_.push_back(partHeader);
      metadata_->parts.push_back(DyldCachePartInfo{.fileName = partPath.filename().string(),
                                                    .uuid = entry.uuid,
                                                    .vmOffset = entry.vmOffset,
                                                    .isSymbolsFile = false});
      XDEC_ASSERT(backingIndex == metadata_->parts.size() - 1,
                 "BackingStore and DyldCacheMetadata::parts index mismatch");
    }

    maybeOpenSymbolsFile();
    return ok();
  }

  /// The `.symbols` file carries local (unmapped) symbol tables, keyed to the
  /// main header's `symbolFileUUID`. It contributes no MemoryRegion -- its
  /// own header describes an address range dyld never actually maps at
  /// runtime -- so it is tracked purely as an extra backing part plus its
  /// local-symbol chunk location, not registered with the MemoryMap.
  void maybeOpenSymbolsFile() {
    if (isZeroUuid(mainHeader_.symbolFileUuid)) {
      return;
    }
    const std::filesystem::path symbolsPath{mainPath_.string() + ".symbols"};
    Result<FileBuffer> symbolsFile = FileBuffer::fromFile(symbolsPath);
    if (!symbolsFile) {
      XDEC_LOG_DEBUG(logBinary(), "no '.symbols' sibling for '{}': {}", mainPath_.string(),
                     symbolsFile.error().message());
      return;
    }
    Result<PartHeader> symbolsHeader =
        parsePartHeader(symbolsFile.value().bytes(), symbolsPath.filename().string());
    if (!symbolsHeader) {
      XDEC_LOG_WARN(logBinary(), "'.symbols' sibling for '{}' did not parse: {}", mainPath_.string(),
                    symbolsHeader.error().message());
      return;
    }
    symbolsPartIndex_ = store_.addPart(symbolsPath.filename().string(), std::move(symbolsFile).value());
    metadata_->parts.push_back(DyldCachePartInfo{.fileName = symbolsPath.filename().string(),
                                                  .uuid = symbolsHeader.value().uuid,
                                                  .vmOffset = 0,
                                                  .isSymbolsFile = true});
    localSymbolsHeader_ = symbolsHeader.value();
  }

  // -- memory map -------------------------------------------------------------

  Result<void> buildMemoryMap() {
    memory_.setBackingParts(store_.spans());
    XDEC_TRY_VOID(addMappingsFrom(0, mainHeader_));
    for (std::size_t index = 0; index < partHeaders_.size(); ++index) {
      // Part 0 is the main file; subcache parts start at backing index 1.
      XDEC_TRY_VOID(addMappingsFrom(index + 1, partHeaders_[index]));
    }
    return memory_.finalize();
  }

  Result<void> addMappingsFrom(std::size_t backingIndex, const PartHeader& header) {
    if (header.mappingCount == 0) {
      return ok();
    }
    FieldReader reader(store_.bytes(backingIndex), Endian::Little);
    for (uint32_t index = 0; index < header.mappingCount; ++index) {
      const uint64_t entryOffset = header.mappingOffset + static_cast<uint64_t>(index) * kMappingRecordSize;
      MemoryRegion region;
      region.va = reader.u64(entryOffset + kMappingAddress);
      region.size = reader.u64(entryOffset + kMappingSize);
      region.fileOffset = reader.u64(entryOffset + kMappingFileOffset);
      region.fileSize = region.size;
      region.permissions = permissionsFromProt(static_cast<uint32_t>(reader.u32(entryOffset + kMappingInitProt)));
      region.backingIndex = static_cast<uint32_t>(backingIndex);
      region.name = store_.part(backingIndex).name;
      if (reader.failed()) {
        return err(DiagCode::BadFormat,
                   std::format("truncated mapping {} in '{}'", index, store_.part(backingIndex).name));
      }
      memory_.addRegion(std::move(region));
    }
    return ok();
  }

  // -- images -----------------------------------------------------------------

  void parseImages() {
    if (mainHeader_.imagesCount == 0) {
      return;
    }
    FieldReader reader(store_.bytes(0), Endian::Little);
    metadata_->images.reserve(mainHeader_.imagesCount);
    for (uint32_t index = 0; index < mainHeader_.imagesCount; ++index) {
      const uint64_t entryOffset =
          mainHeader_.imagesOffset + static_cast<uint64_t>(index) * kImageRecordSize;
      DyldCacheImageRecord image;
      image.loadAddress = reader.u64(entryOffset + kImageAddress);
      const uint64_t pathOffset = reader.u32(entryOffset + kImagePathFileOffset);
      image.path = std::string{reader.cstring(pathOffset)};
      image.index = index;
      if (reader.failed()) {
        XDEC_LOG_WARN(logBinary(), "dyld cache image {} has an unreadable path; skipping", index);
        reader.clearFailure();
        continue;
      }
      metadata_->images.push_back(std::move(image));
    }

    mergeImageTextInfo();

    std::sort(metadata_->images.begin(), metadata_->images.end(),
              [](const DyldCacheImageRecord& a, const DyldCacheImageRecord& b) {
                return a.loadAddress < b.loadAddress;
              });
  }

  /// `dyld_cache_image_text_info` carries the UUID and __TEXT size for each
  /// image, in the same order as the plain image list when both counts
  /// agree (true for every cache seen in practice). Matched by load address
  /// when they do not, so a mismatch degrades to "no size/UUID" rather than
  /// silently pairing the wrong records.
  void mergeImageTextInfo() {
    if (mainHeader_.imagesTextCount == 0) {
      return;
    }
    FieldReader reader(store_.bytes(0), Endian::Little);
    const bool sameOrder = mainHeader_.imagesTextCount == metadata_->images.size();

    for (uint64_t index = 0; index < mainHeader_.imagesTextCount; ++index) {
      const uint64_t entryOffset =
          mainHeader_.imagesTextOffset + index * kImageTextRecordSize;
      const uint64_t loadAddress = reader.u64(entryOffset + kImageTextLoadAddress);
      const auto textSize = static_cast<uint32_t>(reader.u32(entryOffset + kImageTextSegmentSize));
      const CacheUuid uuid = readUuid(reader, entryOffset + kImageTextUuid);
      if (reader.failed()) {
        reader.clearFailure();
        continue;
      }

      DyldCacheImageRecord* target = nullptr;
      if (sameOrder && index < metadata_->images.size() &&
          metadata_->images[index].loadAddress == loadAddress) {
        target = &metadata_->images[index];
      } else {
        const auto it = std::find_if(metadata_->images.begin(), metadata_->images.end(),
                                     [loadAddress](const DyldCacheImageRecord& image) {
                                       return image.loadAddress == loadAddress;
                                     });
        if (it != metadata_->images.end()) {
          target = &*it;
        }
      }
      if (target != nullptr) {
        target->textSegmentSize = textSize;
        target->uuid = uuid;
      }
    }
  }

  // -- finish -------------------------------------------------------------

  Result<std::unique_ptr<BinaryImage>> finish() {
    ImageContents contents;
    contents.format = BinaryFormat::DyldCache;
    contents.kind = BinaryKind::SharedObject;
    contents.arch = arch_;
    contents.endian = Endian::Little;
    contents.pointerBits = 64;
    contents.hasEntryPoint = false;
    contents.path = mainPath_.string();
    contents.memory = std::move(memory_);
    contents.store = std::move(store_);
    contents.formatMetadata = std::move(metadata_);

    XDEC_LOG_INFO(logBinary(),
                 "loaded dyld cache '{}': {} parts, {} regions, {} images", mainPath_.string(),
                 contents.store.partCount(), contents.memory.regions().size(),
                 static_cast<const DyldCacheMetadata*>(contents.formatMetadata.get())->images.size());

    return std::make_unique<BinaryImage>(std::move(contents));
  }

  std::filesystem::path mainPath_;
  BackingStore store_;
  MemoryMap memory_;
  Arch arch_ = Arch::Unknown;
  PartHeader mainHeader_;
  std::vector<PartHeader> partHeaders_;  // subcache parts, in backing-index order starting at 1
  std::optional<PartHeader> localSymbolsHeader_;
  std::optional<std::size_t> symbolsPartIndex_;
  std::unique_ptr<DyldCacheMetadata> metadata_;
};

}  // namespace

Result<std::unique_ptr<BinaryImage>> loadDyldCache(FileBuffer mainFile, std::filesystem::path mainPath) {
  DyldCacheLoader loader(std::move(mainFile), std::move(mainPath));
  return loader.load();
}

}  // namespace xdec::binary
