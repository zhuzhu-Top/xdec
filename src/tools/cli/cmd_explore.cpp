// Fast reverse-engineering exploration commands:
//   strings, xrefs, functions, objc, data
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common.h"
#include "session.h"
#include "xdec/analysis/plt_stub.h"
#include "xdec/analysis/xrefs.h"
#include "xdec/binary/image.h"
#include "xdec/binary/macho_metadata.h"
#include "xdec/support/json.h"

namespace xdec::cli {

namespace {

[[nodiscard]] bool containsCI(std::string_view haystack, std::string_view needle) noexcept {
  if (needle.empty()) {
    return true;
  }
  if (needle.size() > haystack.size()) {
    return false;
  }
  return std::search(
             haystack.begin(), haystack.end(), needle.begin(), needle.end(),
             [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b));
             }) != haystack.end();
}

[[nodiscard]] std::vector<xdec::analysis::CodeSpan> buildCodeSpans(const BinaryImage& image) {
  std::vector<xdec::analysis::CodeSpan> spans;
  for (const auto& sec : image.sections()) {
    if (!sec.code || sec.size == 0) {
      continue;
    }
    const auto view = image.memory().directView(sec.va, sec.size);
    if (!view.empty()) {
      spans.push_back(xdec::analysis::CodeSpan{sec.va, view});
    }
  }
  if (spans.empty()) {
    for (const auto* region : image.executableRegions()) {
      const auto view = image.memory().directView(region->va, region->fileSize);
      if (!view.empty()) {
        spans.push_back(xdec::analysis::CodeSpan{region->va, view});
      }
    }
  }
  return spans;
}

[[nodiscard]] std::vector<xdec::analysis::StringSectionSpan> buildStringSections(
    const BinaryImage& image, std::string_view onlySection) {
  std::vector<xdec::analysis::StringSectionSpan> out;
  for (const auto& sec : image.sections()) {
    if (!sec.programData || sec.code || sec.zeroFilled || sec.size == 0) {
      continue;
    }
    if (!onlySection.empty() && sec.name != onlySection) {
      continue;
    }
    const bool isStringSec =
        sec.name == "__cstring" || sec.name == "__oslogstring" ||
        sec.name == "__objc_methname" || sec.name == "__objc_classname" ||
        sec.name == "__objc_methtype" || sec.name == ".rodata" ||
        sec.name.starts_with(".rodata.str") || sec.name == ".rdata";
    if (!isStringSec && onlySection.empty()) {
      continue;
    }
    const auto view = image.memory().directView(sec.va, sec.size);
    if (!view.empty()) {
      out.push_back(xdec::analysis::StringSectionSpan{sec.va, sec.name, view});
    }
  }
  return out;
}

[[nodiscard]] xdec::ByteReader imageByteReader(const BinaryImage& image) {
  return [&image](uint64_t va, std::span<std::byte> out) { return image.read(va, out); };
}

[[nodiscard]] std::string formatEnclosingFunction(
    const BinaryImage& image, const xdec::binary::MachOMetadata* machoMeta, uint64_t va) {
  if (const auto* sym = image.symbolContaining(va); sym != nullptr) {
    const uint64_t off = va - sym->va;
    return off == 0 ? sym->name : std::format("{}+0x{:x}", sym->name, off);
  }
  uint64_t bestStart = 0;
  std::string bestName;
  if (machoMeta != nullptr) {
    if (const auto bounds = machoMeta->functionBoundsAt(image, va)) {
      bestStart = bounds->first;
      if (const auto m = machoMeta->objcMethodAt(image, bounds->first)) {
        bestName = m->displayName;
      } else {
        bestName = std::format("sub_{:x}", bounds->first);
      }
    }
    if (const auto nearestObjc = machoMeta->nearestObjcMethodBefore(image, va)) {
      if (nearestObjc->first >= bestStart &&
          (va - nearestObjc->first) <= 0x10000) {
        bestStart = nearestObjc->first;
        bestName = nearestObjc->second.displayName;
      }
    }
  }
  if (const auto* nearest = image.symbolNearestBefore(va);
      nearest != nullptr && nearest->va >= bestStart && (va - nearest->va) <= 0x10000) {
    bestStart = nearest->va;
    bestName = nearest->name;
  }
  if (!bestName.empty()) {
    const uint64_t off = va - bestStart;
    return off == 0 ? bestName : std::format("{}+0x{:x}", bestName, off);
  }
  return std::format("0x{:x}", va);
}

[[nodiscard]] std::string escapePreview(std::string_view text, std::size_t maxLen = 100) {
  std::string out;
  out.reserve(std::min(text.size(), maxLen));
  for (std::size_t i = 0; i < text.size() && out.size() < maxLen; ++i) {
    const char ch = text[i];
    if (ch == '"') {
      out += "\\\"";
    } else if (ch == '\\') {
      out += "\\\\";
    } else if (ch == '\n') {
      out += "\\n";
    } else if (ch == '\r') {
      out += "\\r";
    } else if (ch == '\t') {
      out += "\\t";
    } else {
      out.push_back(ch);
    }
  }
  if (text.size() > maxLen) {
    out += "...";
  }
  return out;
}

}  // namespace

int commandStrings(std::string_view path, std::span<const std::string_view> options) {
  xdec::analysis::StringScanOptions scanOpts;
  std::string sectionFilter;
  bool withXrefs = false;
  bool asJson = false;

  for (std::size_t i = 0; i < options.size(); ++i) {
    const std::string_view opt = options[i];
    const auto value = [&]() -> std::string_view {
      return (++i < options.size()) ? options[i] : std::string_view{};
    };
    if (opt == "--grep" || opt == "-g") {
      scanOpts.filter = std::string{value()};
    } else if (opt == "--min-len") {
      uint64_t n = 0;
      if (!parseNumber(value(), n) || n == 0) {
        printError("'--min-len' requires a positive integer");
        return 1;
      }
      scanOpts.minLength = static_cast<std::size_t>(n);
    } else if (opt == "--section") {
      sectionFilter = std::string{value()};
    } else if (opt == "--limit" || opt == "-n") {
      uint64_t n = 0;
      if (!parseNumber(value(), n)) {
        printError("'--limit' requires an integer");
        return 1;
      }
      scanOpts.limit = static_cast<std::size_t>(n);
    } else if (opt == "--xrefs") {
      withXrefs = true;
    } else if (opt == "--json") {
      asJson = true;
    } else {
      printError("unknown strings option '{}'", opt);
      return 1;
    }
  }

  auto opened = open(path);
  if (!opened) {
    return reportError(opened.error());
  }
  const BinaryImage& image = *opened.value();
  const auto* machoMeta =
      dynamic_cast<const xdec::binary::MachOMetadata*>(image.formatMetadata());

  std::vector<xdec::analysis::ImageStringHit> hits;
  if (sectionFilter != "__cfstring") {
    const auto spans = buildStringSections(image, sectionFilter);
    hits = xdec::analysis::scanCStringSections(spans, scanOpts);
  }

  // Also include Mach-O `__cfstring` literals if requested or unfiltered.
  if (machoMeta != nullptr &&
      (sectionFilter.empty() || sectionFilter == "__cfstring") &&
      (scanOpts.limit == 0 || hits.size() < scanOpts.limit)) {
    const auto& sec = machoMeta->sections().cfstring;
    for (uint64_t off = 0; off + 32 <= sec.size; off += 32) {
      const uint64_t va = sec.va + off;
      if (const auto text = machoMeta->cfStringAt(image, va)) {
        if (text->size() >= scanOpts.minLength && containsCI(*text, scanOpts.filter)) {
          hits.push_back(xdec::analysis::ImageStringHit{va, "__cfstring", *text});
          if (scanOpts.limit != 0 && hits.size() >= scanOpts.limit) {
            break;
          }
        }
      }
    }
  }

  std::unordered_map<uint64_t, std::vector<std::string>> xrefsByTarget;
  if (withXrefs && !hits.empty()) {
    std::vector<uint64_t> targetVas;
    targetVas.reserve(hits.size());
    for (const auto& h : hits) {
      targetVas.push_back(h.va);
    }
    const auto code = buildCodeSpans(image);
    xdec::analysis::XrefScanOptions xrefOpts;
    xrefOpts.followStubs = false;
    xrefOpts.limit = 0;
    const auto xrefs =
        xdec::analysis::scanAarch64Xrefs(code, targetVas, imageByteReader(image), xrefOpts);
    for (const auto& xr : xrefs) {
      auto& list = xrefsByTarget[xr.targetVa];
      if (list.size() < 5) {
        list.push_back(formatEnclosingFunction(image, machoMeta, xr.insnVa));
      }
    }
  }

  if (asJson) {
    std::vector<xdec::json::Value> items;
    items.reserve(hits.size());
    for (const auto& hit : hits) {
      std::vector<xdec::json::Value> refsJson;
      if (withXrefs) {
        if (const auto found = xrefsByTarget.find(hit.va); found != xrefsByTarget.end()) {
          for (const auto& fn : found->second) {
            refsJson.emplace_back(fn);
          }
        }
      }
      items.push_back(xdec::json::Value::object({
          {"va", std::format("0x{:x}", hit.va)},
          {"section", hit.section},
          {"text", hit.text},
          {"xrefs", xdec::json::Value::array(std::move(refsJson))},
      }));
    }
    printLine(xdec::json::Value::object({
                  {"count", static_cast<uint64_t>(hits.size())},
                  {"strings", xdec::json::Value::array(std::move(items))},
              })
                  .dump(2));
    return 0;
  }

  print("{} string(s)", hits.size());
  for (const auto& hit : hits) {
    if (!withXrefs) {
      print("  0x{:08x} {:<16} \"{}\"", hit.va, hit.section, escapePreview(hit.text));
      continue;
    }
    const auto found = xrefsByTarget.find(hit.va);
    if (found == xrefsByTarget.end() || found->second.empty()) {
      print("  0x{:08x} {:<16} \"{}\"  [no direct xrefs]", hit.va, hit.section,
            escapePreview(hit.text));
    } else {
      std::string refs;
      for (std::size_t i = 0; i < found->second.size(); ++i) {
        refs += (i == 0 ? "" : ", ") + found->second[i];
      }
      print("  0x{:08x} {:<16} \"{}\"  <- [{}]", hit.va, hit.section,
            escapePreview(hit.text), refs);
    }
  }
  return 0;
}

int commandXrefs(std::string_view path, std::string_view query,
                 std::span<const std::string_view> options) {
  xdec::analysis::XrefScanOptions scanOpts;
  bool asJson = false;
  for (std::size_t i = 0; i < options.size(); ++i) {
    const std::string_view opt = options[i];
    const auto value = [&]() -> std::string_view {
      return (++i < options.size()) ? options[i] : std::string_view{};
    };
    if (opt == "--limit" || opt == "-n") {
      uint64_t n = 0;
      if (!parseNumber(value(), n)) {
        printError("'--limit' requires an integer");
        return 1;
      }
      scanOpts.limit = static_cast<std::size_t>(n);
    } else if (opt == "--no-stubs") {
      scanOpts.followStubs = false;
    } else if (opt == "--json") {
      asJson = true;
    } else {
      printError("unknown xrefs option '{}'", opt);
      return 1;
    }
  }

  auto opened = open(path);
  if (!opened) {
    return reportError(opened.error());
  }
  const BinaryImage& image = *opened.value();
  const auto* machoMeta =
      dynamic_cast<const xdec::binary::MachOMetadata*>(image.formatMetadata());

  std::vector<uint64_t> targets;
  std::unordered_map<uint64_t, std::string> targetLabels;
  const auto addTarget = [&](uint64_t va, std::string label) {
    if (targetLabels.try_emplace(va, std::move(label)).second) {
      targets.push_back(va);
    }
  };

  uint64_t parsedVa = 0;
  if (parseNumber(query, parsedVa)) {
    std::string label = std::format("0x{:x}", parsedVa);
    if (const auto* sym = image.symbolAt(parsedVa)) {
      label = sym->name;
    } else if (const auto imp = image.importNameAt(parsedVa)) {
      label = std::format("ptr_{}", *imp);
    } else     if (machoMeta != nullptr) {
      if (const auto m = machoMeta->objcMethodAt(image, parsedVa)) {
        label = m->displayName;
        // Objective-C methods are dispatched dynamically via `__objc_selrefs`
        // rather than direct `bl IMP`, so also collect matching selector slots.
        const std::size_t sp = m->displayName.find(' ');
        const std::size_t br = m->displayName.rfind(']');
        if (sp != std::string::npos && br != std::string::npos && br > sp + 1 &&
            !machoMeta->sections().objcSelrefs.empty()) {
          const std::string_view sel =
              std::string_view{m->displayName}.substr(sp + 1, br - sp - 1);
          const auto& sr = machoMeta->sections().objcSelrefs;
          for (uint64_t off = 0; off + 8 <= sr.size; off += 8) {
            const uint64_t slotVa = sr.va + off;
            if (const auto ptr = image.readUnsigned(slotVa, 8)) {
              if (const auto s = image.readCString(*ptr, 160); s && *s == sel) {
                addTarget(slotVa, std::format("sel:\"{}\"", *s));
              }
            }
          }
        }
      } else if (const auto d = machoMeta->resolveDataSymbolAt(image, parsedVa)) {
        label = *d;
      }
    }
    addTarget(parsedVa, std::move(label));
  } else if (query.starts_with("sel:")) {
    const std::string_view selNeedle = query.substr(4);
    if (machoMeta != nullptr && !machoMeta->sections().objcSelrefs.empty()) {
      const auto& sr = machoMeta->sections().objcSelrefs;
      for (uint64_t off = 0; off + 8 <= sr.size; off += 8) {
        const uint64_t slotVa = sr.va + off;
        const auto ptr = image.readUnsigned(slotVa, 8);
        if (!ptr) {
          continue;
        }
        const auto s = image.readCString(*ptr, 160);
        if (s && (*s == selNeedle || containsCI(*s, selNeedle))) {
          addTarget(slotVa, std::format("sel:\"{}\"", *s));
        }
      }
    }
  } else if (query.starts_with("str:")) {
    const std::string_view strNeedle = query.substr(4);
    xdec::analysis::StringScanOptions strOpts;
    strOpts.filter = std::string{strNeedle};
    strOpts.minLength = 1;
    strOpts.limit = 64;
    for (const auto& s : xdec::analysis::scanCStringSections(
             buildStringSections(image, {}), strOpts)) {
      addTarget(s.va, std::format("\"{}\"", escapePreview(s.text, 40)));
    }
    if (machoMeta != nullptr) {
      const auto& sec = machoMeta->sections().cfstring;
      for (uint64_t off = 0; off + 32 <= sec.size; off += 32) {
        const uint64_t va = sec.va + off;
        if (const auto cf = machoMeta->cfStringAt(image, va);
            cf && containsCI(*cf, strNeedle)) {
          addTarget(va, std::format("@\"{}\"", escapePreview(*cf, 40)));
        }
      }
    }
  } else {
    // Symbol / import / ObjC method name lookup (exact preferred, else substring).
    for (const auto& sym : image.symbols()) {
      if (sym.defined && sym.name == query) {
        addTarget(sym.va, sym.name);
      }
    }
    for (const auto& reloc : image.relocations()) {
      if (const auto imp = image.importNameAt(reloc.va);
          imp && (*imp == query || ((*imp).size() > 1 && (*imp).substr(1) == query))) {
        addTarget(reloc.va, std::string{*imp});
      }
    }
    if (machoMeta != nullptr) {
      for (const auto& [impVa, info] : machoMeta->allObjcMethods(image)) {
        if (info.displayName == query || info.cName == query) {
          addTarget(impVa, info.displayName);
        }
      }
    }
    if (targets.empty()) {
      for (const auto& sym : image.symbols()) {
        if (sym.defined && containsCI(sym.name, query)) {
          addTarget(sym.va, sym.name);
          if (targets.size() >= 32) {
            break;
          }
        }
      }
      for (const auto& reloc : image.relocations()) {
        if (const auto imp = image.importNameAt(reloc.va); imp && containsCI(*imp, query)) {
          addTarget(reloc.va, std::string{*imp});
          if (targets.size() >= 64) {
            break;
          }
        }
      }
      if (machoMeta != nullptr && targets.size() < 64) {
        for (const auto& [impVa, info] : machoMeta->allObjcMethods(image)) {
          if (containsCI(info.displayName, query)) {
            addTarget(impVa, info.displayName);
            if (targets.size() >= 64) {
              break;
            }
          }
        }
      }
    }
  }

  if (targets.empty()) {
    printError("no target matching '{}'", query);
    return 1;
  }

  const auto code = buildCodeSpans(image);
  const auto hits =
      xdec::analysis::scanAarch64Xrefs(code, targets, imageByteReader(image), scanOpts);
  if (asJson) {
    std::vector<xdec::json::Value> items;
    items.reserve(hits.size());
    for (const auto& hit : hits) {
      items.push_back(xdec::json::Value::object({
          {"va", std::format("0x{:x}", hit.insnVa)},
          {"kind", std::string{xdec::analysis::toString(hit.kind)}},
          {"target_va", std::format("0x{:x}", hit.targetVa)},
          {"target_label", targetLabels[hit.targetVa]},
          {"via_stub", hit.viaStubVa != 0 ? std::format("0x{:x}", hit.viaStubVa) : std::string{}},
          {"function", formatEnclosingFunction(image, machoMeta, hit.insnVa)},
      }));
    }
    printLine(xdec::json::Value::object({
                  {"query", std::string{query}},
                  {"count", static_cast<uint64_t>(hits.size())},
                  {"xrefs", xdec::json::Value::array(std::move(items))},
              })
                  .dump(2));
    return 0;
  }
  print("{} xref(s) to {} target(s) matching '{}'", hits.size(), targets.size(), query);
  for (const auto& hit : hits) {
    const std::string& label = targetLabels[hit.targetVa];
    const std::string via =
        hit.viaStubVa != 0 ? std::format(" (via 0x{:x})", hit.viaStubVa) : std::string{};
    const std::string owner = formatEnclosingFunction(image, machoMeta, hit.insnVa);
    print("  0x{:08x}  {:<6} -> {}{}  in {}", hit.insnVa,
          xdec::analysis::toString(hit.kind), label, via, owner);
  }
  return 0;
}

int commandFunctions(std::string_view path, std::span<const std::string_view> options) {
  std::string filter;
  std::size_t limit = 100;
  bool asJson = false;
  for (std::size_t i = 0; i < options.size(); ++i) {
    const std::string_view opt = options[i];
    const auto value = [&]() -> std::string_view {
      return (++i < options.size()) ? options[i] : std::string_view{};
    };
    if (opt == "--grep" || opt == "-g") {
      filter = std::string{value()};
    } else if (opt == "--limit" || opt == "-n") {
      uint64_t n = 0;
      if (!parseNumber(value(), n)) {
        printError("'--limit' requires an integer");
        return 1;
      }
      limit = static_cast<std::size_t>(n);
    } else if (opt == "--json") {
      asJson = true;
    } else {
      printError("unknown functions option '{}'", opt);
      return 1;
    }
  }

  auto opened = open(path);
  if (!opened) {
    return reportError(opened.error());
  }
  const BinaryImage& image = *opened.value();
  const auto* machoMeta =
      dynamic_cast<const xdec::binary::MachOMetadata*>(image.formatMetadata());

  struct FnRow {
    uint64_t va = 0;
    uint64_t size = 0;
    std::string name;
    std::string extra;
  };
  std::unordered_map<uint64_t, FnRow> byVa;
  for (const auto& sym : image.symbols()) {
    if (!sym.defined || sym.name.empty() ||
        sym.kind != xdec::binary::SymbolKind::Function) {
      continue;
    }
    if (!containsCI(sym.name, filter)) {
      continue;
    }
    byVa[sym.va] = FnRow{sym.va, sym.size, sym.name, sym.exported ? "[exported]" : ""};
  }
  if (machoMeta != nullptr) {
    for (const auto& [impVa, info] : machoMeta->allObjcMethods(image)) {
      if (!containsCI(info.displayName, filter) && !containsCI(info.cName, filter)) {
        continue;
      }
      if (!byVa.contains(impVa)) {
        uint64_t sz = 0;
        if (const auto bounds = machoMeta->functionBoundsAt(image, impVa);
            bounds && bounds->first == impVa) {
          sz = bounds->second - bounds->first;
        }
        byVa[impVa] = FnRow{impVa, sz, info.displayName, info.typeEncoding};
      }
    }
  }

  std::vector<FnRow> rows;
  rows.reserve(byVa.size());
  for (auto& [va, row] : byVa) {
    rows.push_back(std::move(row));
  }
  std::sort(rows.begin(), rows.end(), [](const FnRow& a, const FnRow& b) { return a.va < b.va; });

  if (asJson) {
    std::vector<xdec::json::Value> items;
    const std::size_t cap = limit == 0 ? rows.size() : std::min(limit, rows.size());
    items.reserve(cap);
    for (std::size_t idx = 0; idx < cap; ++idx) {
      const auto& row = rows[idx];
      items.push_back(xdec::json::Value::object({
          {"va", std::format("0x{:x}", row.va)},
          {"size", row.size},
          {"name", row.name},
          {"extra", row.extra},
      }));
    }
    printLine(xdec::json::Value::object({
                  {"count", static_cast<uint64_t>(rows.size())},
                  {"functions", xdec::json::Value::array(std::move(items))},
              })
                  .dump(2));
    return 0;
  }

  print("{} matching function(s)", rows.size());
  std::size_t shown = 0;
  for (const auto& row : rows) {
    if (limit != 0 && shown++ >= limit) {
      print("... {} more", rows.size() - limit);
      break;
    }
    print("  0x{:08x} size={:<8} {} {}", row.va, row.size, row.name, row.extra);
  }
  return 0;
}

int commandObjc(std::string_view path, std::span<const std::string_view> options) {
  std::string classFilter;
  std::string selFilter;
  std::size_t limit = 50;
  bool asJson = false;

  for (std::size_t i = 0; i < options.size(); ++i) {
    const std::string_view opt = options[i];
    const auto value = [&]() -> std::string_view {
      return (++i < options.size()) ? options[i] : std::string_view{};
    };
    if (opt == "--class" || opt == "-c" || opt == "--grep" || opt == "-g") {
      classFilter = std::string{value()};
    } else if (opt == "--sel" || opt == "-s") {
      selFilter = std::string{value()};
    } else if (opt == "--limit" || opt == "-n") {
      uint64_t n = 0;
      if (!parseNumber(value(), n)) {
        printError("'--limit' requires an integer");
        return 1;
      }
      limit = static_cast<std::size_t>(n);
    } else if (opt == "--json") {
      asJson = true;
    } else {
      printError("unknown objc option '{}'", opt);
      return 1;
    }
  }

  auto opened = open(path);
  if (!opened) {
    return reportError(opened.error());
  }
  const BinaryImage& image = *opened.value();
  const auto* machoMeta =
      dynamic_cast<const xdec::binary::MachOMetadata*>(image.formatMetadata());
  if (machoMeta == nullptr) {
    printError("'{}' is not a Mach-O binary with Objective-C metadata", path);
    return 1;
  }

  const auto& classes = machoMeta->objcClasses(image);
  std::vector<const xdec::binary::ObjcClassSummary*> matched;
  for (const auto& cls : classes) {
    if (!containsCI(cls.name, classFilter)) {
      continue;
    }
    if (!selFilter.empty()) {
      const bool hasSel = std::any_of(
          cls.methods.begin(), cls.methods.end(),
          [&](const auto& m) { return containsCI(m.selector, selFilter); });
      if (!hasSel) {
        continue;
      }
    }
    matched.push_back(&cls);
  }

  if (asJson) {
    std::vector<xdec::json::Value> classItems;
    const std::size_t cap = limit == 0 ? matched.size() : std::min(limit, matched.size());
    classItems.reserve(cap);
    for (std::size_t idx = 0; idx < cap; ++idx) {
      const auto* cls = matched[idx];
      std::vector<xdec::json::Value> ivarItems;
      for (const auto& iv : cls->ivars) {
        ivarItems.push_back(xdec::json::Value::object({
            {"name", iv.name},
            {"type", iv.typeEncoding},
            {"offset", static_cast<int64_t>(iv.offset)},
            {"slot", std::format("0x{:x}", iv.offsetSlotVa)},
        }));
      }
      std::vector<xdec::json::Value> methodItems;
      for (const auto& m : cls->methods) {
        if (!selFilter.empty() && !containsCI(m.selector, selFilter)) {
          continue;
        }
        methodItems.push_back(xdec::json::Value::object({
            {"kind", std::string(1, m.kind)},
            {"selector", m.selector},
            {"type", m.typeEncoding},
            {"imp", std::format("0x{:x}", m.impVa)},
        }));
      }
      classItems.push_back(xdec::json::Value::object({
          {"name", cls->name},
          {"super", cls->superName},
          {"class_va", std::format("0x{:x}", cls->classVa)},
          {"instance_size", static_cast<uint64_t>(cls->instanceSize)},
          {"ivars", xdec::json::Value::array(std::move(ivarItems))},
          {"methods", xdec::json::Value::array(std::move(methodItems))},
      }));
    }
    printLine(xdec::json::Value::object({
                  {"matched", static_cast<uint64_t>(matched.size())},
                  {"total", static_cast<uint64_t>(classes.size())},
                  {"classes", xdec::json::Value::array(std::move(classItems))},
              })
                  .dump(2));
    return 0;
  }

  print("{} matching class(es) (of {} total)", matched.size(), classes.size());
  std::size_t shown = 0;
  for (const auto* cls : matched) {
    if (limit != 0 && shown++ >= limit) {
      print("... {} more", matched.size() - limit);
      break;
    }
    print("@interface {} : {}  // class=0x{:x} size=0x{:x}", cls->name,
          cls->superName.empty() ? "<root>" : cls->superName, cls->classVa, cls->instanceSize);
    for (const auto& iv : cls->ivars) {
      print("  ivar +0x{:<4x} {:<16} {}  (slot 0x{:x})", static_cast<uint32_t>(iv.offset),
            iv.typeEncoding, iv.name, iv.offsetSlotVa);
    }
    for (const auto& m : cls->methods) {
      if (!selFilter.empty() && !containsCI(m.selector, selFilter)) {
        continue;
      }
      print("  0x{:08x}  {}[{} {}]  ({})", m.impVa, m.kind, cls->name, m.selector,
            m.typeEncoding);
    }
  }
  return 0;
}

int commandData(std::string_view path, uint64_t address, uint64_t count,
                std::span<const std::string_view> options) {
  bool asJson = false;
  for (const std::string_view opt : options) {
    if (opt == "--json") {
      asJson = true;
    } else {
      printError("unknown data option '{}'", opt);
      return 1;
    }
  }
  auto opened = open(path);
  if (!opened) {
    return reportError(opened.error());
  }
  const BinaryImage& image = *opened.value();
  const auto* machoMeta =
      dynamic_cast<const xdec::binary::MachOMetadata*>(image.formatMetadata());

  if (const auto* sec = image.sectionAt(address)) {
    print("address : 0x{:x} in {} (writable={})", address, sec->name,
          image.isWritable(address) ? "yes" : "no");
  } else {
    print("address : 0x{:x}", address);
  }
  if (const auto* sym = image.symbolAt(address)) {
    print("symbol  : {}", sym->name);
  }

  if (machoMeta != nullptr) {
    if (const auto cf = machoMeta->cfStringAt(image, address)) {
      print("cfstring: @\"{}\"", escapePreview(*cf, 256));
    }
    if (const auto metaName = machoMeta->resolveDataSymbolAt(image, address)) {
      print("metadata: {}", *metaName);
    }
  }
  if (const auto str = image.readCString(address, 256); str && str->size() >= 2) {
    const auto* sec = image.sectionAt(address);
    const bool allowCstr =
        sec == nullptr || (!image.isExecutable(address) &&
                           sec->name != "__objc_data" && sec->name != "__objc_const") ||
        sec->name == "__cstring" || sec->name == "__oslogstring" ||
        sec->name == "__objc_methname" || sec->name == "__objc_classname";
    const bool printable = allowCstr && std::all_of(str->begin(), str->end(), [](unsigned char c) {
                             return c >= 0x20 && c < 0x7f;
                           });
    if (printable) {
      print("cstring : \"{}\"", escapePreview(*str, 200));
    }
  }

  const uint64_t slots = count == 0 ? 8 : std::min<uint64_t>(count, 256);
  struct SlotRow {
    uint64_t va;
    uint64_t value;
    std::string note;
  };
  std::vector<SlotRow> rows;
  for (uint64_t i = 0; i < slots; ++i) {
    const uint64_t slotVa = address + i * 8;
    const auto val = image.readUnsigned(slotVa, 8);
    if (!val) {
      break;
    }
    std::string note;
    if (const auto imp = image.importNameAt(slotVa)) {
      note = std::format("-> import {}", *imp);
    } else if (*val != 0 && image.isMapped(*val)) {
      if (const auto* s = image.symbolAt(*val)) {
        note = std::format("-> {}", s->name);
      } else if (machoMeta != nullptr) {
        if (const auto m = machoMeta->objcMethodAt(image, *val)) {
          note = std::format("-> {}", m->displayName);
        } else if (const auto d = machoMeta->resolveDataSymbolAt(image, *val)) {
          note = std::format("-> {}", *d);
        }
      }
      if (note.empty()) {
        const auto* targetSec = image.sectionAt(*val);
        const bool isStringSec =
            targetSec != nullptr &&
            (targetSec->name == "__cstring" || targetSec->name == "__oslogstring" ||
             targetSec->name == "__objc_methname" || targetSec->name == "__objc_classname" ||
             targetSec->name == "__objc_methtype" || targetSec->name == ".rodata" ||
             targetSec->name.starts_with(".rodata.str"));
        if (isStringSec) {
          if (const auto cstr = image.readCString(*val, 80); cstr && cstr->size() >= 2) {
            const bool ok = std::all_of(cstr->begin(), cstr->end(), [](unsigned char c) {
              return c >= 0x20 && c < 0x7f;
            });
            if (ok) {
              note = std::format("-> \"{}\"", escapePreview(*cstr, 60));
            }
          }
        }
      }
      if (note.empty()) {
        if (const auto* targetSec = image.sectionAt(*val)) {
          note = std::format("-> ({})", targetSec->name);
        }
      }
    }
    rows.push_back(SlotRow{slotVa, *val, std::move(note)});
  }

  if (asJson) {
    std::vector<xdec::json::Value> slotItems;
    for (const auto& r : rows) {
      slotItems.push_back(xdec::json::Value::object({
          {"va", std::format("0x{:x}", r.va)},
          {"value", std::format("0x{:x}", r.value)},
          {"note", r.note},
      }));
    }
    const auto* sec = image.sectionAt(address);
    printLine(xdec::json::Value::object({
                  {"address", std::format("0x{:x}", address)},
                  {"section", sec != nullptr ? sec->name : std::string{}},
                  {"writable", image.isWritable(address)},
                  {"slots", xdec::json::Value::array(std::move(slotItems))},
              })
                  .dump(2));
    return 0;
  }

  print("slots ({}):", slots);
  for (std::size_t i = 0; i < rows.size(); ++i) {
    print("  0x{:08x} (+0x{:02x}): 0x{:016x} {}", rows[i].va, i * 8, rows[i].value,
          rows[i].note);
  }
  return 0;
}

int commandCallers(std::string_view path, std::string_view query,
                   std::span<const std::string_view> options) {
  unsigned maxDepth = 1;
  std::size_t limit = 100;
  bool asJson = false;
  for (std::size_t i = 0; i < options.size(); ++i) {
    const std::string_view opt = options[i];
    const auto value = [&]() -> std::string_view {
      return (++i < options.size()) ? options[i] : std::string_view{};
    };
    if (opt == "--depth" || opt == "-d") {
      uint64_t n = 0;
      if (!parseNumber(value(), n) || n == 0 || n > 4) {
        printError("'--depth' requires 1..4");
        return 1;
      }
      maxDepth = static_cast<unsigned>(n);
    } else if (opt == "--limit" || opt == "-n") {
      uint64_t n = 0;
      if (!parseNumber(value(), n)) {
        printError("'--limit' requires an integer");
        return 1;
      }
      limit = static_cast<std::size_t>(n);
    } else if (opt == "--json") {
      asJson = true;
    } else {
      printError("unknown callers option '{}'", opt);
      return 1;
    }
  }

  auto opened = open(path);
  if (!opened) {
    return reportError(opened.error());
  }
  const BinaryImage& image = *opened.value();
  const auto* machoMeta =
      dynamic_cast<const xdec::binary::MachOMetadata*>(image.formatMetadata());

  const auto enclosingStartVa = [&](uint64_t va) -> std::pair<uint64_t, std::string> {
    if (const auto* sym = image.symbolContaining(va)) {
      return {sym->va, sym->name};
    }
    uint64_t bestStart = 0;
    std::string bestName;
    if (machoMeta != nullptr) {
      if (const auto b = machoMeta->functionBoundsAt(image, va)) {
        bestStart = b->first;
        if (const auto m = machoMeta->objcMethodAt(image, b->first)) {
          bestName = m->displayName;
        } else {
          bestName = std::format("sub_{:x}", b->first);
        }
      }
      if (const auto m = machoMeta->nearestObjcMethodBefore(image, va);
          m.has_value() && m->first >= bestStart && (va - m->first) <= 0x10000) {
        bestStart = m->first;
        bestName = m->second.displayName;
      }
    }
    if (const auto* nearest = image.symbolNearestBefore(va);
        nearest != nullptr && nearest->va >= bestStart && (va - nearest->va) <= 0x10000) {
      bestStart = nearest->va;
      bestName = nearest->name;
    }
    if (bestStart != 0) {
      return {bestStart, bestName};
    }
    return {va, std::format("0x{:x}", va)};
  };

  std::vector<uint64_t> frontier;
  uint64_t parsedVa = 0;
  if (parseNumber(query, parsedVa)) {
    frontier.push_back(parsedVa);
  } else {
    for (const auto& sym : image.symbols()) {
      if (sym.defined && sym.kind == xdec::binary::SymbolKind::Function && sym.name == query) {
        frontier.push_back(sym.va);
      }
    }
    for (const auto& reloc : image.relocations()) {
      if (const auto imp = image.importNameAt(reloc.va);
          imp && (*imp == query || ((*imp).size() > 1 && (*imp).substr(1) == query))) {
        frontier.push_back(reloc.va);
      }
    }
    if (machoMeta != nullptr) {
      for (const auto& [impVa, info] : machoMeta->allObjcMethods(image)) {
        if (info.displayName == query || info.cName == query) {
          frontier.push_back(impVa);
        }
      }
    }
  }
  if (frontier.empty()) {
    printError("no function/import matching '{}'", query);
    return 1;
  }

  const auto code = buildCodeSpans(image);
  const auto reader = imageByteReader(image);
  struct CallerEdge {
    unsigned depth = 1;
    uint64_t callVa = 0;
    uint64_t callerVa = 0;
    std::string callerName;
    uint64_t targetVa = 0;
  };
  std::vector<CallerEdge> edges;
  std::unordered_set<uint64_t> visitedFuncs(frontier.begin(), frontier.end());

  for (unsigned d = 1; d <= maxDepth && !frontier.empty(); ++d) {
    // Expand any ObjC IMPs in frontier with their `__objc_selrefs` slots.
    std::vector<uint64_t> scanTargets = frontier;
    if (machoMeta != nullptr && !machoMeta->sections().objcSelrefs.empty()) {
      for (const uint64_t va : frontier) {
        if (const auto m = machoMeta->objcMethodAt(image, va)) {
          const std::size_t sp = m->displayName.find(' ');
          const std::size_t br = m->displayName.rfind(']');
          if (sp != std::string::npos && br != std::string::npos && br > sp + 1) {
            const std::string_view sel =
                std::string_view{m->displayName}.substr(sp + 1, br - sp - 1);
            const auto& sr = machoMeta->sections().objcSelrefs;
            for (uint64_t off = 0; off + 8 <= sr.size; off += 8) {
              if (const auto p = image.readUnsigned(sr.va + off, 8)) {
                if (const auto s = image.readCString(*p, 160); s && *s == sel) {
                  scanTargets.push_back(sr.va + off);
                }
              }
            }
          }
        }
      }
    }

    xdec::analysis::XrefScanOptions opts;
    opts.limit = 0;
    const auto hits = xdec::analysis::scanAarch64Xrefs(code, scanTargets, reader, opts);
    std::vector<uint64_t> nextFrontier;
    std::unordered_set<uint64_t> seenAtDepth;
    for (const auto& h : hits) {
      const auto [callerVa, callerName] = enclosingStartVa(h.insnVa);
      if (!seenAtDepth.insert(callerVa).second) {
        continue;
      }
      edges.push_back(CallerEdge{d, h.insnVa, callerVa, callerName, h.targetVa});
      if (visitedFuncs.insert(callerVa).second) {
        nextFrontier.push_back(callerVa);
      }
      if (limit != 0 && edges.size() >= limit) {
        break;
      }
    }
    frontier = std::move(nextFrontier);
    if (limit != 0 && edges.size() >= limit) {
      break;
    }
  }

  if (asJson) {
    std::vector<xdec::json::Value> items;
    for (const auto& e : edges) {
      items.push_back(xdec::json::Value::object({
          {"depth", static_cast<uint64_t>(e.depth)},
          {"call_va", std::format("0x{:x}", e.callVa)},
          {"caller_va", std::format("0x{:x}", e.callerVa)},
          {"caller_name", e.callerName},
          {"target_va", std::format("0x{:x}", e.targetVa)},
      }));
    }
    printLine(xdec::json::Value::object({
                  {"query", std::string{query}},
                  {"count", static_cast<uint64_t>(edges.size())},
                  {"callers", xdec::json::Value::array(std::move(items))},
              })
                  .dump(2));
    return 0;
  }

  print("{} caller(s) for '{}'", edges.size(), query);
  for (const auto& e : edges) {
    print("{}0x{:08x}  {}  (site 0x{:08x} -> 0x{:x})",
          std::string(static_cast<std::size_t>(e.depth) * 2, ' '),
          e.callerVa, e.callerName, e.callVa, e.targetVa);
  }
  return 0;
}

int commandCallees(std::string_view path, std::string_view query,
                   std::span<const std::string_view> options) {
  bool asJson = false;
  for (const std::string_view opt : options) {
    if (opt == "--json") {
      asJson = true;
    } else {
      printError("unknown callees option '{}'", opt);
      return 1;
    }
  }

  auto opened = open(path);
  if (!opened) {
    return reportError(opened.error());
  }
  const BinaryImage& image = *opened.value();
  const auto* machoMeta =
      dynamic_cast<const xdec::binary::MachOMetadata*>(image.formatMetadata());

  uint64_t startVa = 0;
  if (!parseNumber(query, startVa)) {
    bool found = false;
    for (const auto& sym : image.symbols()) {
      if (sym.defined && sym.kind == xdec::binary::SymbolKind::Function &&
          (sym.name == query || containsCI(sym.name, query))) {
        startVa = sym.va;
        found = true;
        if (sym.name == query) {
          break;
        }
      }
    }
    if (!found && machoMeta != nullptr) {
      for (const auto& [impVa, info] : machoMeta->allObjcMethods(image)) {
        if (info.displayName == query || info.cName == query ||
            containsCI(info.displayName, query)) {
          startVa = impVa;
          found = true;
          if (info.displayName == query) {
            break;
          }
        }
      }
    }
    if (!found) {
      printError("no function matching '{}'", query);
      return 1;
    }
  }

  uint64_t endVa = startVa + 0x4000;
  if (const auto* sym = image.symbolAt(startVa); sym != nullptr && sym->size != 0) {
    endVa = startVa + sym->size;
  } else if (machoMeta != nullptr) {
    if (const auto b = machoMeta->functionBoundsAt(image, startVa);
        b.has_value() && b->first == startVa && b->second > startVa) {
      endVa = std::min(endVa, b->second);
    }
  }

  const auto bytes = image.memory().directView(startVa, endVa - startVa);
  if (bytes.empty()) {
    printError("address 0x{:x} is not backed by mapped code", startVa);
    return 1;
  }

  const auto reader = imageByteReader(image);
  struct CalleeRow {
    uint64_t siteVa = 0;
    uint64_t targetVa = 0;
    std::string name;
  };
  std::vector<CalleeRow> rows;
  std::array<uint64_t, 31> adrpPage{};
  std::array<uint32_t, 31> adrpSeq{};
  std::array<std::string, 31> regSel{};
  std::array<std::string, 31> regCls{};
  uint32_t seq = 0;

  const auto chaseBranchIslands = [&](uint64_t entryDest) {
    uint64_t cur = entryDest;
    bool chased = false;
    for (unsigned hop = 0; hop < 6; ++hop) {
      const auto wOpt = image.readUnsigned(cur, 4);
      if (!wOpt || (*wOpt & 0xfc000000u) != 0x14000000u) {
        break;
      }
      const uint32_t imm26 = static_cast<uint32_t>(*wOpt) & 0x03ffffffu;
      const int32_t sext = static_cast<int32_t>(imm26 << 6) >> 6;
      cur = cur + static_cast<uint64_t>(static_cast<int64_t>(sext) << 2);
      chased = true;
    }
    return std::pair<uint64_t, bool>{cur, chased};
  };

  const std::size_t count = bytes.size() / 4;
  for (std::size_t i = 0; i < count; ++i) {
    ++seq;
    const uint64_t pc = startVa + i * 4;
    uint32_t w = 0;
    std::memcpy(&w, bytes.data() + i * 4, 4);

    // Track ADRP + LDR for `__objc_selrefs` / `__objc_classrefs` context on `_objc_msgSend`.
    if ((w & 0xffe0ffe0u) == 0xaa0003e0u) {
      // `mov Xd, Xm` (`orr Xd, xzr, Xm`): propagate cached selector / class name.
      const unsigned rd = w & 0x1fu;
      const unsigned rm = (w >> 16) & 0x1fu;
      if (rd < 31 && rm < 31) {
        regSel[rd] = regSel[rm];
        regCls[rd] = regCls[rm];
      }
      continue;
    }
    if ((w & 0x9f000000u) == 0x90000000u) {
      const unsigned rd = w & 0x1fu;
      if (rd < 31) {
        const uint32_t immlo = (w >> 29) & 0x3u;
        const uint32_t immhi = (w >> 5) & 0x7ffffu;
        const int32_t sext = static_cast<int32_t>(((immhi << 2) | immlo) << 11) >> 11;
        adrpPage[rd] = (pc & ~uint64_t{0xfff}) + static_cast<uint64_t>(static_cast<int64_t>(sext) << 12);
        adrpSeq[rd] = seq;
        regSel[rd].clear();
        regCls[rd].clear();
      }
      continue;
    }
    if ((w & 0xffc00000u) == 0xf9400000u) {
      const unsigned rt = w & 0x1fu;
      const unsigned rn = (w >> 5) & 0x1fu;
      if (rt < 31 && rn < 31 && adrpSeq[rn] != 0 && seq - adrpSeq[rn] <= 12) {
        const uint64_t slotVa = adrpPage[rn] + (uint64_t{(w >> 10) & 0xfffu} << 3);
        regSel[rt].clear();
        regCls[rt].clear();
        if (machoMeta != nullptr && machoMeta->sections().objcSelrefs.contains(slotVa)) {
          if (const auto strPtr = image.readUnsigned(slotVa, 8)) {
            if (const auto sel = image.readCString(*strPtr, 160)) {
              regSel[rt] = *sel;
            }
          }
        } else if (machoMeta != nullptr &&
                   (machoMeta->sections().objcClassrefs.contains(slotVa) ||
                    machoMeta->sections().objcSuperrefs.contains(slotVa))) {
          if (const auto imp = image.importNameAt(slotVa)) {
            regCls[rt] = std::string{*imp};
          } else if (const auto d = machoMeta->resolveDataSymbolAt(image, slotVa)) {
            regCls[rt] = *d;
          }
        }
      }
      continue;
    }

    const uint32_t top6 = w >> 26;
    if (top6 != 0b100101u && top6 != 0b000101u) {
      continue;
    }
    const uint32_t imm26 = w & 0x03ffffffu;
    const int32_t sext = static_cast<int32_t>(imm26 << 6) >> 6;
    const uint64_t dest = pc + static_cast<uint64_t>(static_cast<int64_t>(sext) << 2);
    if (top6 == 0b000101u && dest >= startVa && dest < endVa) {
      continue;  // intra-function branch
    }
    std::string name;
    if (const auto got = xdec::analysis::pltGotSlot(reader, dest)) {
      if (const auto imp = image.importNameAt(*got)) {
        name = std::string{*imp};
      }
      if (const auto selSlot = xdec::analysis::objcStubSelectorSlot(reader, dest)) {
        if (const auto selPtr = image.readUnsigned(*selSlot, 8)) {
          if (const auto sel = image.readCString(*selPtr, 160)) {
            name += std::format("${}", *sel);
          }
        }
      }
    }
    const auto [chasedDest, chased] = chaseBranchIslands(dest);
    if (name.empty()) {
      if (const auto* s = image.symbolAt(chasedDest)) {
        name = chased ? ("j_" + s->name) : s->name;
      } else if (machoMeta != nullptr) {
        if (const auto m = machoMeta->objcMethodAt(image, chasedDest)) {
          name = chased ? ("j_" + m->displayName) : m->displayName;
        }
      }
    }
    if (name.empty()) {
      name = chased ? std::format("j_sub_{:x}", chasedDest)
                    : std::format("sub_{:x}", dest);
    }
    if ((name == "_objc_msgSend" || name == "_objc_msgSendSuper2") && !regSel[1].empty()) {
      if (!regCls[0].empty()) {
        name += std::format(" [{} {}]", regCls[0], regSel[1]);
      } else {
        name += std::format(" sel:\"{}\"", regSel[1]);
      }
    }
    rows.push_back(CalleeRow{pc, chased ? chasedDest : dest, std::move(name)});
    for (unsigned r = 0; r <= 18; ++r) {
      adrpSeq[r] = 0;
      regSel[r].clear();
      regCls[r].clear();
    }
  }

  if (asJson) {
    std::vector<xdec::json::Value> items;
    for (const auto& r : rows) {
      items.push_back(xdec::json::Value::object({
          {"site", std::format("0x{:x}", r.siteVa)},
          {"target", std::format("0x{:x}", r.targetVa)},
          {"name", r.name},
      }));
    }
    printLine(xdec::json::Value::object({
                  {"function", std::format("0x{:x}", startVa)},
                  {"count", static_cast<uint64_t>(rows.size())},
                  {"callees", xdec::json::Value::array(std::move(items))},
              })
                  .dump(2));
    return 0;
  }

  print("{} callee(s) from 0x{:x}", rows.size(), startVa);
  for (const auto& r : rows) {
    print("  0x{:08x} -> 0x{:08x}  {}", r.siteVa, r.targetVa, r.name);
  }
  return 0;
}

}  // namespace xdec::cli
