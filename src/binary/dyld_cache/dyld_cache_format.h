// dyld shared cache on-disk constants and structure offsets.
//
// Mirrors macho_format.h and elf_format.h: named field offsets rather than
// packed structs, because the header has grown by appending fields for over a
// decade and a cache built by an older dyld simply omits the tail -- reading
// a fixed-size struct would silently read the next table as header fields.
// Offsets below were verified both against Apple's public
// dyld_cache_format.h (apple-oss-distributions/dyld) and against a real
// iOS 18-era split cache (5 subcache parts + a .symbols file); see
// docs/22-dyld-shared-cache.md for how each field maps to loader behaviour.
#pragma once

#include <cstdint>

namespace xdec::binary::dyldcache {

// The full magic is "dyld_v1" followed by a space-padded architecture name
// ("  arm64", " arm64e", ...), 16 bytes total including the NUL padding.
inline constexpr std::size_t kMagicSize = 16;
inline constexpr std::size_t kMagicPrefixLen = 7;  // "dyld_v1"

// dyld_cache_header field offsets. Every loader before the split cache only
// ever needs the fields up to `imagesCount` (offset 456); a header shorter
// than a given field's end offset means that field was not yet part of the
// format when the cache was built, and the loader must not trust whatever
// bytes happen to be there (they belong to the mapping table that follows
// the header). `headerCovers()` below is how the loader enforces that.
inline constexpr uint64_t kHdrMagic = 0;
inline constexpr uint64_t kHdrMappingOffset = 16;
inline constexpr uint64_t kHdrMappingCount = 20;
inline constexpr uint64_t kHdrCodeSignatureOffset = 40;
inline constexpr uint64_t kHdrCodeSignatureSize = 48;
inline constexpr uint64_t kHdrLocalSymbolsOffset = 72;
inline constexpr uint64_t kHdrLocalSymbolsSize = 80;
inline constexpr uint64_t kHdrUuid = 88;  // 16 bytes
inline constexpr uint64_t kHdrUuidSize = 16;
inline constexpr uint64_t kHdrCacheType = 104;
inline constexpr uint64_t kHdrImagesTextOffset = 136;
inline constexpr uint64_t kHdrImagesTextCount = 144;
inline constexpr uint64_t kHdrPlatform = 216;
inline constexpr uint64_t kHdrFormatVersionBits = 220;
inline constexpr uint64_t kHdrSharedRegionStart = 224;
inline constexpr uint64_t kHdrSharedRegionSize = 232;
inline constexpr uint64_t kHdrMaxSlide = 240;
inline constexpr uint64_t kHdrMappingWithSlideOffset = 312;
inline constexpr uint64_t kHdrMappingWithSlideCount = 316;
inline constexpr uint64_t kHdrSwiftOptsOffset = 376;
inline constexpr uint64_t kHdrSwiftOptsSize = 384;
inline constexpr uint64_t kHdrSubCacheArrayOffset = 392;
inline constexpr uint64_t kHdrSubCacheArrayCount = 396;
inline constexpr uint64_t kHdrSymbolFileUuid = 400;  // 16 bytes
inline constexpr uint64_t kHdrImagesOffset = 448;
inline constexpr uint64_t kHdrImagesCount = 452;
// End of the header shape common to every cache this loader has seen
// (everything past here -- objcOptsOffset, cacheAtlas, tproMappings, ... --
// is read only when `headerCovers()` says the file's header is long enough).
inline constexpr uint64_t kHdrMinimumSize = kHdrImagesCount + 4;  // 456
inline constexpr uint64_t kHdrObjcOptsOffset = 464;
inline constexpr uint64_t kHdrObjcOptsSize = 472;

// `formatVersion : 8, dylibsExpectedOnDisk : 1, simulator : 1,
//  locallyBuiltCache : 1, builtFromChainedFixups : 1, ...` packed into the
// uint32 at kHdrFormatVersionBits.
inline constexpr uint32_t kFormatVersionMask = 0xffu;
inline constexpr uint32_t kBuiltFromChainedFixupsBit = 1u << 11;

inline constexpr uint64_t kCacheTypeDevelopment = 0;
inline constexpr uint64_t kCacheTypeProduction = 1;
inline constexpr uint64_t kCacheTypeUniversal = 2;

// dyld_cache_mapping_info (32 bytes).
inline constexpr uint64_t kMappingAddress = 0;
inline constexpr uint64_t kMappingSize = 8;
inline constexpr uint64_t kMappingFileOffset = 16;
inline constexpr uint64_t kMappingMaxProt = 24;
inline constexpr uint64_t kMappingInitProt = 28;
inline constexpr uint64_t kMappingRecordSize = 32;

// dyld_cache_mapping_and_slide_info (56 bytes) -- superset of the plain
// mapping entry, read when `mappingWithSlideCount > 0`.
inline constexpr uint64_t kMappingSlideAddress = 0;
inline constexpr uint64_t kMappingSlideSize = 8;
inline constexpr uint64_t kMappingSlideFileOffset = 16;
inline constexpr uint64_t kMappingSlideInfoFileOffset = 24;
inline constexpr uint64_t kMappingSlideInfoFileSize = 32;
inline constexpr uint64_t kMappingSlideFlags = 40;
inline constexpr uint64_t kMappingSlideMaxProt = 48;
inline constexpr uint64_t kMappingSlideInitProt = 52;
inline constexpr uint64_t kMappingSlideRecordSize = 56;

inline constexpr uint64_t kMappingFlagConstData = 1u << 2;
inline constexpr uint64_t kMappingFlagAuthData = 1u << 0;

// VM protection bits, same encoding as Mach-O.
inline constexpr uint32_t kVmProtRead = 1;
inline constexpr uint32_t kVmProtWrite = 2;
inline constexpr uint32_t kVmProtExecute = 4;

// dyld_subcache_entry_v1 (24 bytes: uuid + cacheVMOffset only -- the "legacy
// sibling" naming scheme, `basename.1` .. `basename.N`).
inline constexpr uint64_t kSubCacheV1Uuid = 0;
inline constexpr uint64_t kSubCacheV1VmOffset = 16;
inline constexpr uint64_t kSubCacheV1RecordSize = 24;

// dyld_subcache_entry (56 bytes: adds a 32-byte fileSuffix, e.g.
// ".01.development" or ".symbols") -- the newer format that names each part
// explicitly instead of relying on positional numbering.
inline constexpr uint64_t kSubCacheV2Uuid = 0;
inline constexpr uint64_t kSubCacheV2VmOffset = 16;
inline constexpr uint64_t kSubCacheV2FileSuffix = 24;
inline constexpr uint64_t kSubCacheV2FileSuffixSize = 32;
inline constexpr uint64_t kSubCacheV2RecordSize = 56;

// dyld_cache_image_info (32 bytes).
inline constexpr uint64_t kImageAddress = 0;
inline constexpr uint64_t kImageModTime = 8;
inline constexpr uint64_t kImageInode = 16;
inline constexpr uint64_t kImagePathFileOffset = 24;
inline constexpr uint64_t kImageRecordSize = 32;

// dyld_cache_image_text_info (28 bytes).
inline constexpr uint64_t kImageTextUuid = 0;  // 16 bytes
inline constexpr uint64_t kImageTextLoadAddress = 16;
inline constexpr uint64_t kImageTextSegmentSize = 24;
inline constexpr uint64_t kImageTextPathOffset = 28;
inline constexpr uint64_t kImageTextRecordSize = 32;

// dyld_cache_local_symbols_info (24 bytes), file-relative to
// header.localSymbolsOffset (or to the start of the `.symbols` subcache).
inline constexpr uint64_t kLocalSymNlistOffset = 0;
inline constexpr uint64_t kLocalSymNlistCount = 4;
inline constexpr uint64_t kLocalSymStringsOffset = 8;
inline constexpr uint64_t kLocalSymStringsSize = 12;
inline constexpr uint64_t kLocalSymEntriesOffset = 16;
inline constexpr uint64_t kLocalSymEntriesCount = 20;

// dyld_cache_local_symbols_entry_64 (16 bytes) -- every cache seen in
// practice for 64-bit architectures uses the 64-bit dylib offset variant.
inline constexpr uint64_t kLocalSymEntryDylibOffset = 0;
inline constexpr uint64_t kLocalSymEntryNlistStart = 8;
inline constexpr uint64_t kLocalSymEntryNlistCount = 12;
inline constexpr uint64_t kLocalSymEntryRecordSize = 16;

// nlist_64, same layout as macho::kNlist* (16 bytes).
inline constexpr uint64_t kNlistStrx = 0;
inline constexpr uint64_t kNlistType = 4;
inline constexpr uint64_t kNlistSect = 5;
inline constexpr uint64_t kNlistValue = 8;
inline constexpr uint64_t kNlistRecordSize = 16;
inline constexpr uint8_t kNTypeMask = 0x0e;
inline constexpr uint8_t kNSect = 0xe;
inline constexpr uint8_t kNExt = 0x01;

}  // namespace xdec::binary::dyldcache
