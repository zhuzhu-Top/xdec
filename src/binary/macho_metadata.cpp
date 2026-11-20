#include "xdec/binary/macho_metadata.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <format>
#include <vector>

namespace xdec::binary {

namespace {

constexpr uint64_t kFastDataMask = 0x00007ffffffffff8ull;
constexpr uint32_t kUnwindSecondLevelRegular = 2;
constexpr uint32_t kUnwindSecondLevelCompressed = 3;
constexpr uint32_t kRelMethodFlag = 0x80000000u;
constexpr uint32_t kDirectSelMethodFlag = 0x40000000u;

[[nodiscard]] std::optional<uint32_t> readU32(const BinaryImage& image, uint64_t va) {
  const Result<uint64_t> val = image.readUnsigned(va, 4);
  if (!val) {
    return std::nullopt;
  }
  return static_cast<uint32_t>(*val);
}

[[nodiscard]] std::optional<uint16_t> readU16(const BinaryImage& image, uint64_t va) {
  const Result<uint64_t> val = image.readUnsigned(va, 2);
  if (!val) {
    return std::nullopt;
  }
  return static_cast<uint16_t>(*val);
}

[[nodiscard]] std::optional<int32_t> readS32(const BinaryImage& image, uint64_t va) {
  const Result<uint64_t> val = image.readUnsigned(va, 4);
  if (!val) {
    return std::nullopt;
  }
  return static_cast<int32_t>(static_cast<uint32_t>(*val));
}

[[nodiscard]] std::optional<uint64_t> readU64(const BinaryImage& image, uint64_t va) {
  const Result<uint64_t> val = image.readUnsigned(va, 8);
  if (!val) {
    return std::nullopt;
  }
  return *val;
}

[[nodiscard]] std::optional<std::string> readAsciiCString(const BinaryImage& image, uint64_t va,
                                                          std::size_t maxLen = 256) {
  const Result<std::string> raw = image.readCString(va, maxLen);
  if (!raw || raw->empty()) {
    return std::nullopt;
  }
  return *raw;
}

[[nodiscard]] std::string sanitizeIdentifier(std::string_view text, std::size_t maxLen = 48) {
  std::string out;
  out.reserve(std::min(text.size(), maxLen));
  for (const char ch : text) {
    const auto uc = static_cast<unsigned char>(ch);
    if (std::isalnum(uc) != 0 || ch == '_' || ch == '$') {
      out.push_back(ch);
    } else if (out.empty() || out.back() != '_') {
      out.push_back('_');
    }
    if (out.size() >= maxLen) {
      break;
    }
  }
  if (out.empty()) {
    return "empty";
  }
  return out;
}

[[nodiscard]] std::optional<std::string> readClassName(const BinaryImage& image, uint64_t clsVa) {
  if (clsVa == 0 || !image.isMapped(clsVa)) {
    return std::nullopt;
  }
  const auto bits = readU64(image, clsVa + 0x20);
  if (!bits) {
    return std::nullopt;
  }
  const uint64_t roVa = *bits & kFastDataMask;
  if (roVa == 0 || !image.isMapped(roVa)) {
    return std::nullopt;
  }
  const auto namePtr = readU64(image, roVa + 0x18);
  if (!namePtr || *namePtr == 0) {
    return std::nullopt;
  }
  return readAsciiCString(image, *namePtr, 160);
}

struct UnwindIndexEntry {
  uint32_t funcOffset = 0;
  uint32_t secondLevelOffset = 0;
};

[[nodiscard]] std::optional<UnwindIndexEntry> readUnwindIndex(const BinaryImage& image,
                                                              uint64_t base, uint32_t index) {
  const uint64_t addr = base + uint64_t{index} * 12;
  const auto funcOff = readU32(image, addr);
  const auto secondOff = readU32(image, addr + 4);
  if (!funcOff || !secondOff) {
    return std::nullopt;
  }
  return UnwindIndexEntry{*funcOff, *secondOff};
}

/// Collects sorted `funcOffset` entries from one second-level `__unwind_info` page.
void readSecondLevelPageOffsets(const BinaryImage& image, uint64_t unwindBase,
                                const UnwindIndexEntry& pageEntry,
                                std::vector<uint32_t>& out) {
  out.clear();
  if (pageEntry.secondLevelOffset == 0) {
    return;
  }
  const uint64_t pageVa = unwindBase + pageEntry.secondLevelOffset;
  const auto kind = readU32(image, pageVa);
  const auto entryPageOffset = readU16(image, pageVa + 4);
  const auto entryCount = readU16(image, pageVa + 6);
  if (!kind || !entryPageOffset || !entryCount || *entryCount == 0) {
    return;
  }
  out.reserve(*entryCount);
  const uint64_t entriesVa = pageVa + *entryPageOffset;
  if (*kind == kUnwindSecondLevelRegular) {
    for (uint16_t i = 0; i < *entryCount; ++i) {
      const auto off = readU32(image, entriesVa + uint64_t{i} * 8);
      if (!off) {
        break;
      }
      out.push_back(*off);
    }
  } else if (*kind == kUnwindSecondLevelCompressed) {
    for (uint16_t i = 0; i < *entryCount; ++i) {
      const auto word = readU32(image, entriesVa + uint64_t{i} * 4);
      if (!word) {
        break;
      }
      out.push_back(pageEntry.funcOffset + (*word & 0x00ffffffu));
    }
  }
}

}  // namespace

std::optional<std::pair<uint64_t, uint64_t>> MachOMetadata::functionBoundsAt(
    const BinaryImage& image, uint64_t va) const {
  if (sections_.unwindInfo.size < 28 || va < sections_.imageBase) {
    return std::nullopt;
  }
  const uint64_t base = sections_.unwindInfo.va;
  const auto version = readU32(image, base);
  const auto indexOffset = readU32(image, base + 20);
  const auto indexCount = readU32(image, base + 24);
  if (!version || *version != 1 || !indexOffset || !indexCount || *indexCount < 2) {
    return std::nullopt;
  }
  if (uint64_t{*indexOffset} + uint64_t{*indexCount} * 12 > sections_.unwindInfo.size) {
    return std::nullopt;
  }
  const uint64_t targetOffset64 = va - sections_.imageBase;
  if (targetOffset64 > 0xffffffffull) {
    return std::nullopt;
  }
  const auto targetOffset = static_cast<uint32_t>(targetOffset64);
  const uint64_t indexBase = base + *indexOffset;

  const auto first = readUnwindIndex(image, indexBase, 0);
  const auto sentinel = readUnwindIndex(image, indexBase, *indexCount - 1);
  if (!first || !sentinel || targetOffset < first->funcOffset ||
      targetOffset >= sentinel->funcOffset) {
    return std::nullopt;
  }

  uint32_t lo = 0;
  uint32_t hi = *indexCount - 1;  // last is sentinel
  while (lo + 1 < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    const auto midEntry = readUnwindIndex(image, indexBase, mid);
    if (!midEntry) {
      return std::nullopt;
    }
    if (midEntry->funcOffset <= targetOffset) {
      lo = mid;
    } else {
      hi = mid;
    }
  }

  const auto pageEntry = readUnwindIndex(image, indexBase, lo);
  const auto nextPageEntry = readUnwindIndex(image, indexBase, lo + 1);
  if (!pageEntry || !nextPageEntry) {
    return std::nullopt;
  }

  std::vector<uint32_t> offsets;
  readSecondLevelPageOffsets(image, base, *pageEntry, offsets);
  if (offsets.empty()) {
    return std::nullopt;
  }

  const auto it = std::upper_bound(offsets.begin(), offsets.end(), targetOffset);
  if (it == offsets.begin()) {
    return std::nullopt;
  }
  const uint32_t startOff = *std::prev(it);
  const uint32_t endOff = it != offsets.end() ? *it : nextPageEntry->funcOffset;
  if (endOff <= startOff) {
    return std::nullopt;
  }
  return std::pair<uint64_t, uint64_t>{sections_.imageBase + startOff,
                                       sections_.imageBase + endOff};
}

std::optional<uint64_t> MachOMetadata::nextUnwindFunctionAfter(const BinaryImage& image,
                                                               uint64_t va) const {
  if (sections_.unwindInfo.size < 28 || va < sections_.imageBase) {
    return std::nullopt;
  }
  const uint64_t base = sections_.unwindInfo.va;
  const auto version = readU32(image, base);
  const auto indexOffset = readU32(image, base + 20);
  const auto indexCount = readU32(image, base + 24);
  if (!version || *version != 1 || !indexOffset || !indexCount || *indexCount < 2) {
    return std::nullopt;
  }
  if (uint64_t{*indexOffset} + uint64_t{*indexCount} * 12 > sections_.unwindInfo.size) {
    return std::nullopt;
  }
  const uint64_t targetOffset64 = va - sections_.imageBase;
  if (targetOffset64 >= 0xffffffffull) {
    return std::nullopt;
  }
  const auto targetOffset = static_cast<uint32_t>(targetOffset64);
  const uint64_t indexBase = base + *indexOffset;

  const auto first = readUnwindIndex(image, indexBase, 0);
  const auto sentinel = readUnwindIndex(image, indexBase, *indexCount - 1);
  if (!first || !sentinel || targetOffset >= sentinel->funcOffset) {
    return std::nullopt;
  }
  if (targetOffset < first->funcOffset) {
    return sections_.imageBase + first->funcOffset;
  }

  uint32_t lo = 0;
  uint32_t hi = *indexCount - 1;
  while (lo + 1 < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    const auto midEntry = readUnwindIndex(image, indexBase, mid);
    if (!midEntry) {
      return std::nullopt;
    }
    if (midEntry->funcOffset <= targetOffset) {
      lo = mid;
    } else {
      hi = mid;
    }
  }

  const auto pageEntry = readUnwindIndex(image, indexBase, lo);
  const auto nextPageEntry = readUnwindIndex(image, indexBase, lo + 1);
  if (!pageEntry || !nextPageEntry) {
    return std::nullopt;
  }

  std::vector<uint32_t> offsets;
  readSecondLevelPageOffsets(image, base, *pageEntry, offsets);
  const auto it = std::upper_bound(offsets.begin(), offsets.end(), targetOffset);
  if (it != offsets.end()) {
    return sections_.imageBase + *it;
  }
  if (nextPageEntry->funcOffset > targetOffset) {
    return sections_.imageBase + nextPageEntry->funcOffset;
  }
  return std::nullopt;
}

std::optional<std::string> MachOMetadata::cfStringAt(const BinaryImage& image,
                                                     uint64_t va) const {
  if (!sections_.cfstring.contains(va) || (va - sections_.cfstring.va) % 32 != 0) {
    return std::nullopt;
  }
  const auto flags = readU64(image, va + 8);
  const auto dataPtr = readU64(image, va + 16);
  const auto len = readU64(image, va + 24);
  if (!flags || !dataPtr || !len || *dataPtr == 0) {
    return std::nullopt;
  }
  const bool isUtf16 = (*flags & 0x10u) != 0;
  const std::size_t count = static_cast<std::size_t>(std::min<uint64_t>(*len, 256));
  std::string text;
  text.reserve(count);
  if (!isUtf16) {
    for (std::size_t i = 0; i < count; ++i) {
      const auto b = image.readUnsigned(*dataPtr + i, 1);
      if (!b) {
        return std::nullopt;
      }
      const auto ch = static_cast<unsigned char>(*b);
      if (ch == 0) {
        break;
      }
      if (ch >= 0x20 && ch <= 0x7e) {
        text.push_back(static_cast<char>(ch));
      } else {
        text.push_back('?');
      }
    }
  } else {
    for (std::size_t i = 0; i < count; ++i) {
      const auto codeUnit = readU16(image, *dataPtr + i * 2);
      if (!codeUnit) {
        return std::nullopt;
      }
      if (*codeUnit == 0) {
        break;
      }
      if (*codeUnit >= 0x20 && *codeUnit <= 0x7e) {
        text.push_back(static_cast<char>(*codeUnit));
      } else {
        text.push_back('?');
      }
    }
  }
  return text;
}

std::optional<std::string> MachOMetadata::resolveDataSymbolAt(const BinaryImage& image,
                                                              uint64_t va) const {
  if (sections_.objcSelrefs.contains(va) && (va % 8) == 0) {
    const auto strPtr = readU64(image, va);
    if (strPtr && *strPtr != 0) {
      if (const auto sel = readAsciiCString(image, *strPtr, 160)) {
        return "selRef_" + sanitizeIdentifier(*sel, 56);
      }
    }
    return std::nullopt;
  }

  if ((sections_.objcClassrefs.contains(va) || sections_.objcSuperrefs.contains(va)) &&
      (va % 8) == 0) {
    const auto clsPtr = readU64(image, va);
    if (clsPtr && *clsPtr != 0) {
      if (const auto clsName = readClassName(image, *clsPtr)) {
        const std::string_view prefix =
            sections_.objcSuperrefs.contains(va) ? "superRef_" : "clsRef_";
        return std::string{prefix} + sanitizeIdentifier(*clsName, 56);
      }
    }
    return std::nullopt;
  }

  if (sections_.objcProtorefs.contains(va) && (va % 8) == 0) {
    const auto protoPtr = readU64(image, va);
    if (protoPtr && *protoPtr != 0) {
      if (const auto namePtr = readU64(image, *protoPtr + 0x08); namePtr && *namePtr != 0) {
        if (const auto protoName = readAsciiCString(image, *namePtr, 160)) {
          return "protoRef_" + sanitizeIdentifier(*protoName, 56);
        }
      }
    }
    return std::nullopt;
  }

  if (sections_.cfstring.contains(va)) {
    if (const auto text = cfStringAt(image, va)) {
      return std::format("cfstr_{}_{:x}", sanitizeIdentifier(*text, 28), va & 0xffffu);
    }
    return std::nullopt;
  }

  if (sections_.objcIvar.contains(va) && (va % 4) == 0) {
    ensureObjcIndexed(image);
    if (const auto found = objcIvars_.find(va); found != objcIvars_.end()) {
      return found->second;
    }
    if (const auto off = readS32(image, va)) {
      return std::format("ivar_off_{:#x}_{:x}", static_cast<uint32_t>(*off), va & 0xffffu);
    }
    return std::nullopt;
  }

  return std::nullopt;
}

const std::unordered_map<uint64_t, ObjcMethodInfo>& MachOMetadata::allObjcMethods(
    const BinaryImage& image) const {
  ensureObjcIndexed(image);
  return objcMethods_;
}

const std::vector<ObjcClassSummary>& MachOMetadata::objcClasses(const BinaryImage& image) const {
  std::call_once(objcClassesOnce_, [&] {
    if (sections_.objcClasslist.empty()) {
      return;
    }
    const auto collectMethods = [&](uint64_t listVa, char kindChar,
                                    std::vector<ObjcMethodSummary>& out) {
      if (listVa == 0 || !image.isMapped(listVa)) {
        return;
      }
      const auto entsizeAndFlags = readU32(image, listVa);
      const auto count = readU32(image, listVa + 4);
      if (!entsizeAndFlags || !count || *count == 0 || *count > 65536) {
        return;
      }
      const bool relative = (*entsizeAndFlags & kRelMethodFlag) != 0;
      const bool directSel = (*entsizeAndFlags & kDirectSelMethodFlag) != 0;
      const uint32_t entsize = *entsizeAndFlags & 0x0000ffffu;
      if ((relative && entsize < 12) || (!relative && entsize < 24)) {
        return;
      }
      for (uint32_t i = 0; i < *count; ++i) {
        const uint64_t elem = listVa + 8 + uint64_t{i} * entsize;
        uint64_t selStrVa = 0;
        uint64_t typesVa = 0;
        uint64_t impVa = 0;
        if (relative) {
          const auto nameOff = readS32(image, elem);
          const auto typesOff = readS32(image, elem + 4);
          const auto impOff = readS32(image, elem + 8);
          if (!nameOff || !typesOff || !impOff) {
            break;
          }
          const uint64_t nameTarget =
              elem + static_cast<uint64_t>(static_cast<int64_t>(*nameOff));
          if (directSel) {
            selStrVa = nameTarget;
          } else {
            const auto slot = readU64(image, nameTarget);
            if (!slot) {
              continue;
            }
            selStrVa = *slot;
          }
          typesVa = (elem + 4) + static_cast<uint64_t>(static_cast<int64_t>(*typesOff));
          impVa = (elem + 8) + static_cast<uint64_t>(static_cast<int64_t>(*impOff));
        } else {
          const auto n = readU64(image, elem);
          const auto t = readU64(image, elem + 8);
          const auto p = readU64(image, elem + 16);
          if (!n || !t || !p) {
            break;
          }
          selStrVa = *n;
          typesVa = *t;
          impVa = *p;
        }
        const auto selName = readAsciiCString(image, selStrVa, 160);
        if (!selName) {
          continue;
        }
        ObjcMethodSummary m;
        m.kind = kindChar;
        m.selector = *selName;
        if (typesVa != 0) {
          if (const auto t = readAsciiCString(image, typesVa, 160)) {
            m.typeEncoding = *t;
          }
        }
        m.impVa = impVa;
        out.push_back(std::move(m));
      }
    };

    const uint64_t count = sections_.objcClasslist.size / 8;
    objcClasses_.reserve(static_cast<std::size_t>(std::min<uint64_t>(count, 100000)));
    for (uint64_t i = 0; i < count; ++i) {
      const auto clsPtr = readU64(image, sections_.objcClasslist.va + i * 8);
      if (!clsPtr || *clsPtr == 0) {
        continue;
      }
      const auto bits = readU64(image, *clsPtr + 0x20);
      if (!bits) {
        continue;
      }
      const uint64_t roVa = *bits & kFastDataMask;
      if (roVa == 0) {
        continue;
      }
      const auto namePtr = readU64(image, roVa + 0x18);
      if (!namePtr || *namePtr == 0) {
        continue;
      }
      const auto clsName = readAsciiCString(image, *namePtr, 160);
      if (!clsName) {
        continue;
      }
      ObjcClassSummary cls;
      cls.name = *clsName;
      cls.classVa = *clsPtr;
      if (const auto instSize = readU32(image, roVa + 0x08)) {
        cls.instanceSize = *instSize;
      }
      if (const auto importedSuper = image.importNameAt(*clsPtr + 0x08)) {
        cls.superName = std::string{*importedSuper};
      } else if (const auto superPtr = readU64(image, *clsPtr + 0x08);
                 superPtr && *superPtr != 0) {
        if (const auto sName = readClassName(image, *superPtr)) {
          cls.superName = *sName;
        }
      }
      if (const auto baseMethods = readU64(image, roVa + 0x20);
          baseMethods && *baseMethods != 0) {
        collectMethods(*baseMethods, '-', cls.methods);
      }
      if (const auto metaPtr = readU64(image, *clsPtr); metaPtr && *metaPtr != 0) {
        if (const auto metaBits = readU64(image, *metaPtr + 0x20)) {
          const uint64_t metaRo = *metaBits & kFastDataMask;
          if (metaRo != 0) {
            if (const auto metaMethods = readU64(image, metaRo + 0x20);
                metaMethods && *metaMethods != 0) {
              collectMethods(*metaMethods, '+', cls.methods);
            }
          }
        }
      }
      if (const auto ivarsVa = readU64(image, roVa + 0x30); ivarsVa && *ivarsVa != 0) {
        const auto entsize = readU32(image, *ivarsVa);
        const auto ivarCount = readU32(image, *ivarsVa + 4);
        if (entsize && ivarCount && *entsize >= 32 && *ivarCount > 0 && *ivarCount <= 65536) {
          for (uint32_t j = 0; j < *ivarCount; ++j) {
            const uint64_t elem = *ivarsVa + 8 + uint64_t{j} * (*entsize);
            const auto offPtr = readU64(image, elem);
            const auto ivNamePtr = readU64(image, elem + 8);
            const auto ivTypePtr = readU64(image, elem + 16);
            if (!offPtr || !ivNamePtr || *ivNamePtr == 0) {
              continue;
            }
            const auto ivName = readAsciiCString(image, *ivNamePtr, 128);
            if (!ivName) {
              continue;
            }
            ObjcIvarSummary iv;
            iv.name = *ivName;
            iv.offsetSlotVa = *offPtr;
            if (*offPtr != 0) {
              if (const auto offVal = readS32(image, *offPtr)) {
                iv.offset = *offVal;
              }
            }
            if (ivTypePtr && *ivTypePtr != 0) {
              if (const auto t = readAsciiCString(image, *ivTypePtr, 128)) {
                iv.typeEncoding = *t;
              }
            }
            cls.ivars.push_back(std::move(iv));
          }
        }
      }
      objcClasses_.push_back(std::move(cls));
    }
  });
  return objcClasses_;
}

std::optional<ObjcMethodInfo> MachOMetadata::objcMethodAt(const BinaryImage& image,
                                                          uint64_t va) const {
  if (!sections_.text.contains(va)) {
    return std::nullopt;
  }
  ensureObjcIndexed(image);
  const auto found = objcMethods_.find(va);
  if (found == objcMethods_.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::optional<std::pair<uint64_t, ObjcMethodInfo>> MachOMetadata::nearestObjcMethodBefore(
    const BinaryImage& image, uint64_t va) const {
  if (!sections_.text.contains(va)) {
    return std::nullopt;
  }
  ensureObjcIndexed(image);
  if (sortedObjcMethodVas_.empty()) {
    return std::nullopt;
  }
  const auto it = std::upper_bound(sortedObjcMethodVas_.begin(), sortedObjcMethodVas_.end(), va);
  if (it == sortedObjcMethodVas_.begin()) {
    return std::nullopt;
  }
  const uint64_t startVa = *std::prev(it);
  const auto found = objcMethods_.find(startVa);
  if (found == objcMethods_.end()) {
    return std::nullopt;
  }
  return std::pair<uint64_t, ObjcMethodInfo>{startVa, found->second};
}

void MachOMetadata::ensureObjcIndexed(const BinaryImage& image) const {
  std::call_once(objcOnce_, [&] {
    const auto indexMethods = [&](uint64_t listVa, std::string_view className, char kindChar) {
      if (listVa == 0 || !image.isMapped(listVa)) {
        return;
      }
      const auto entsizeAndFlags = readU32(image, listVa);
      const auto count = readU32(image, listVa + 4);
      if (!entsizeAndFlags || !count || *count == 0 || *count > 65536) {
        return;
      }
      const bool relative = (*entsizeAndFlags & kRelMethodFlag) != 0;
      const bool directSel = (*entsizeAndFlags & kDirectSelMethodFlag) != 0;
      const uint32_t entsize = *entsizeAndFlags & 0x0000ffffu;
      if ((relative && entsize < 12) || (!relative && entsize < 24)) {
        return;
      }
      for (uint32_t i = 0; i < *count; ++i) {
        const uint64_t elem = listVa + 8 + uint64_t{i} * entsize;
        uint64_t selStrVa = 0;
        uint64_t typesVa = 0;
        uint64_t impVa = 0;
        if (relative) {
          const auto nameOff = readS32(image, elem);
          const auto typesOff = readS32(image, elem + 4);
          const auto impOff = readS32(image, elem + 8);
          if (!nameOff || !typesOff || !impOff) {
            break;
          }
          const uint64_t nameTarget =
              elem + static_cast<uint64_t>(static_cast<int64_t>(*nameOff));
          if (directSel) {
            selStrVa = nameTarget;
          } else {
            const auto slot = readU64(image, nameTarget);
            if (!slot) {
              continue;
            }
            selStrVa = *slot;
          }
          typesVa = (elem + 4) + static_cast<uint64_t>(static_cast<int64_t>(*typesOff));
          impVa = (elem + 8) + static_cast<uint64_t>(static_cast<int64_t>(*impOff));
        } else {
          const auto n = readU64(image, elem);
          const auto t = readU64(image, elem + 8);
          const auto p = readU64(image, elem + 16);
          if (!n || !t || !p) {
            break;
          }
          selStrVa = *n;
          typesVa = *t;
          impVa = *p;
        }
        if (impVa == 0 || !sections_.text.contains(impVa)) {
          continue;
        }
        const auto selName = readAsciiCString(image, selStrVa, 160);
        if (!selName) {
          continue;
        }
        std::string typesStr;
        if (typesVa != 0) {
          if (const auto t = readAsciiCString(image, typesVa, 160)) {
            typesStr = *t;
          }
        }
        ObjcMethodInfo info;
        info.displayName = std::format("{}[{} {}]", kindChar, className, *selName);
        info.cName = std::format("objc_{}_{}_{}", kindChar == '+' ? "cls" : "inst",
                                 sanitizeIdentifier(className, 40),
                                 sanitizeIdentifier(*selName, 48));
        info.typeEncoding = std::move(typesStr);
        objcMethods_.try_emplace(impVa, std::move(info));
      }
    };

    const auto indexIvars = [&](uint64_t ivarListVa, std::string_view className) {
      if (ivarListVa == 0 || !image.isMapped(ivarListVa)) {
        return;
      }
      const auto entsize = readU32(image, ivarListVa);
      const auto count = readU32(image, ivarListVa + 4);
      if (!entsize || !count || *entsize < 32 || *count == 0 || *count > 65536) {
        return;
      }
      for (uint32_t i = 0; i < *count; ++i) {
        const uint64_t elem = ivarListVa + 8 + uint64_t{i} * (*entsize);
        const auto offsetPtr = readU64(image, elem);
        const auto namePtr = readU64(image, elem + 8);
        if (!offsetPtr || !namePtr || *offsetPtr == 0 || *namePtr == 0) {
          continue;
        }
        if (!sections_.objcIvar.contains(*offsetPtr)) {
          continue;
        }
        if (const auto ivarName = readAsciiCString(image, *namePtr, 128)) {
          objcIvars_.try_emplace(
              *offsetPtr, std::format("ivar_{}_{}", sanitizeIdentifier(className, 36),
                                      sanitizeIdentifier(*ivarName, 36)));
        }
      }
    };

    if (!sections_.objcClasslist.empty()) {
      const uint64_t count = sections_.objcClasslist.size / 8;
      for (uint64_t i = 0; i < count; ++i) {
        const auto clsPtr = readU64(image, sections_.objcClasslist.va + i * 8);
        if (!clsPtr || *clsPtr == 0) {
          continue;
        }
        const auto bits = readU64(image, *clsPtr + 0x20);
        if (!bits) {
          continue;
        }
        const uint64_t roVa = *bits & kFastDataMask;
        if (roVa == 0) {
          continue;
        }
        const auto namePtr = readU64(image, roVa + 0x18);
        if (!namePtr || *namePtr == 0) {
          continue;
        }
        const auto clsName = readAsciiCString(image, *namePtr, 160);
        if (!clsName) {
          continue;
        }
        if (const auto baseMethods = readU64(image, roVa + 0x20); baseMethods && *baseMethods != 0) {
          indexMethods(*baseMethods, *clsName, '-');
        }
        if (const auto ivars = readU64(image, roVa + 0x30); ivars && *ivars != 0) {
          indexIvars(*ivars, *clsName);
        }
        if (const auto metaPtr = readU64(image, *clsPtr); metaPtr && *metaPtr != 0) {
          if (const auto metaBits = readU64(image, *metaPtr + 0x20)) {
            const uint64_t metaRo = *metaBits & kFastDataMask;
            if (metaRo != 0) {
              if (const auto metaMethods = readU64(image, metaRo + 0x20);
                  metaMethods && *metaMethods != 0) {
                indexMethods(*metaMethods, *clsName, '+');
              }
            }
          }
        }
      }
    }

    if (!sections_.objcCatlist.empty()) {
      const uint64_t count = sections_.objcCatlist.size / 8;
      for (uint64_t i = 0; i < count; ++i) {
        const auto catPtr = readU64(image, sections_.objcCatlist.va + i * 8);
        if (!catPtr || *catPtr == 0) {
          continue;
        }
        const auto catNamePtr = readU64(image, *catPtr);
        const auto clsPtr = readU64(image, *catPtr + 0x08);
        std::string ownerName;
        if (clsPtr && *clsPtr != 0) {
          if (const auto clsName = readClassName(image, *clsPtr)) {
            ownerName = *clsName;
          }
        }
        if (catNamePtr && *catNamePtr != 0) {
          if (const auto catName = readAsciiCString(image, *catNamePtr, 80)) {
            ownerName = ownerName.empty() ? *catName : std::format("{}_{}", ownerName, *catName);
          }
        }
        if (ownerName.empty()) {
          continue;
        }
        if (const auto instMethods = readU64(image, *catPtr + 0x10);
            instMethods && *instMethods != 0) {
          indexMethods(*instMethods, ownerName, '-');
        }
        if (const auto clsMethods = readU64(image, *catPtr + 0x18);
            clsMethods && *clsMethods != 0) {
          indexMethods(*clsMethods, ownerName, '+');
        }
      }
    }

    sortedObjcMethodVas_.reserve(objcMethods_.size());
    for (const auto& [impVa, info] : objcMethods_) {
      (void)info;
      sortedObjcMethodVas_.push_back(impVa);
    }
    std::sort(sortedObjcMethodVas_.begin(), sortedObjcMethodVas_.end());
  });
}

}  // namespace xdec::binary
