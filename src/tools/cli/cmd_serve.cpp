// Persistent in-memory MCP / JSON-RPC 2.0 stdio server (`xdec serve <binary>`).
//
// Opens the BinaryImage (mmap), SpecEngine, SessionContext, and any --plugin
// once at startup, then answers newline-delimited JSON-RPC requests in <50ms
// without paying cold-start or re-indexing overhead per command.
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common.h"
#include "session.h"
#include "xdec/support/paths.h"
#include "xdec/analysis/plt_stub.h"
#include "xdec/analysis/xrefs.h"
#include "xdec/binary/image.h"
#include "xdec/binary/macho_metadata.h"
#include "xdec/decompile/driver.h"
#include "xdec/decompile/emit.h"
#include "xdec/passes/builtin.h"
#include "xdec/plugin/loader.h"
#include "xdec/support/json.h"

namespace xdec::cli {

namespace {

[[nodiscard]] bool matchTextCI(std::string_view hay, std::string_view needle) noexcept {
  if (needle.empty()) {
    return true;
  }
  return hay.size() >= needle.size() &&
         std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                     [](char a, char b) {
                       return std::tolower(static_cast<unsigned char>(a)) ==
                              std::tolower(static_cast<unsigned char>(b));
                     }) != hay.end();
}

[[nodiscard]] xdec::json::Value mcpToolDef(std::string name, std::string description,
                                           xdec::json::Value schema) {
  return xdec::json::Value::object({
      {"name", std::move(name)},
      {"description", std::move(description)},
      {"inputSchema", std::move(schema)},
  });
}

[[nodiscard]] xdec::json::Value mcpTextResult(std::string text, bool isError = false) {
  return xdec::json::Value::object({
      {"content", xdec::json::Value::array({xdec::json::Value::object({
                      {"type", "text"},
                      {"text", std::move(text)},
                  })})},
      {"isError", isError},
  });
}

[[nodiscard]] xdec::json::Value jsonRpcOk(const xdec::json::Value& id,
                                          xdec::json::Value result) {
  return xdec::json::Value::object({
      {"jsonrpc", "2.0"},
      {"id", id},
      {"result", std::move(result)},
  });
}

[[nodiscard]] xdec::json::Value jsonRpcErr(const xdec::json::Value& id, int64_t code,
                                           std::string message) {
  return xdec::json::Value::object({
      {"jsonrpc", "2.0"},
      {"id", id},
      {"error", xdec::json::Value::object({
                    {"code", code},
                    {"message", std::move(message)},
                })},
  });
}

[[nodiscard]] std::string defaultObjcPluginPath() {
  return xdec::defaultPluginPath("objc_libcxx");
}

}  // namespace

int commandServe(std::string_view path, std::span<const std::string_view> options) {
  std::vector<std::string> typeSources;
  std::vector<std::string> pluginPaths;
  std::string syscallSource{xdec::types::SyscallTable::defaultName()};
  bool defaultPlugins = true;

  for (std::size_t i = 0; i < options.size(); ++i) {
    const std::string_view opt = options[i];
    const auto value = [&]() -> std::string_view {
      return (++i < options.size()) ? options[i] : std::string_view{};
    };
    if (opt == "--mcp") {
      // Default protocol is MCP JSON-RPC 2.0 over stdio; flag accepted explicitly.
    } else if (opt == "--plugin") {
      pluginPaths.emplace_back(value());
    } else if (opt == "--no-default-plugins") {
      defaultPlugins = false;
    } else if (opt == "--types") {
      typeSources.emplace_back(value());
    } else if (opt == "--syscall-table") {
      const std::string_view text = value();
      syscallSource = text == "none" ? std::string{} : std::string{text};
    } else {
      printError("unknown serve option '{}'", opt);
      return 1;
    }
  }

  SessionLoadOptions loadOpts;
  loadOpts.typeSources = typeSources;
  loadOpts.syscallSource = syscallSource;
  auto session = SessionContext::open(path, loadOpts);
  if (!session) {
    return reportError(session.error());
  }
  const BinaryImage& image = *session->image;
  const xdec::spec::SpecEngine& engine = *session->engine;
  const xdec::spec::ByteReader reader = session->reader();
  const auto* machoMeta =
      dynamic_cast<const xdec::binary::MachOMetadata*>(image.formatMetadata());

  if (defaultPlugins && image.format() == xdec::binary::BinaryFormat::MachO) {
    const bool already = std::any_of(pluginPaths.begin(), pluginPaths.end(), [](const std::string& p) {
      return p.find("objc_libcxx") != std::string::npos;
    });
    if (!already) {
      if (const std::string p = defaultObjcPluginPath(); !p.empty()) {
        pluginPaths.push_back(p);
      }
    }
  }

  std::vector<xdec::plugin::Plugin> plugins;
  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);
  std::vector<xdec::emit::AstTransformFn> pluginAstTransforms;
  std::vector<xdec::emit::HlAstTransformFn> pluginHlTransforms;
  for (const std::string& pluginPath : pluginPaths) {
    auto plugin = xdec::plugin::Plugin::load(pluginPath, registry, &pluginAstTransforms,
                                             &pluginHlTransforms);
    if (!plugin) {
      return reportError(plugin.error());
    }
    plugins.push_back(std::move(*plugin));
  }

  // Pre-build reusable code/string spans once over the mmap'd image.
  std::vector<xdec::analysis::CodeSpan> codeSpans;
  for (const auto& sec : image.sections()) {
    if (sec.code && sec.size != 0) {
      const auto view = image.memory().directView(sec.va, sec.size);
      if (!view.empty()) {
        codeSpans.push_back(xdec::analysis::CodeSpan{sec.va, view});
      }
    }
  }
  std::vector<xdec::analysis::StringSectionSpan> stringSpans;
  for (const auto& sec : image.sections()) {
    if (!sec.programData || sec.code || sec.zeroFilled || sec.size == 0) {
      continue;
    }
    if (sec.name == "__cstring" || sec.name == "__oslogstring" ||
        sec.name == "__objc_methname" || sec.name == "__objc_classname" ||
        sec.name == ".rodata" || sec.name.starts_with(".rodata.str")) {
      const auto view = image.memory().directView(sec.va, sec.size);
      if (!view.empty()) {
        stringSpans.push_back(xdec::analysis::StringSectionSpan{sec.va, sec.name, view});
      }
    }
  }
  const xdec::ByteReader rawReader = [&image](uint64_t va, std::span<std::byte> out) {
    return image.read(va, out);
  };

  const auto formatOwner = [&](uint64_t va) -> std::string {
    if (const auto* sym = image.symbolContaining(va)) {
      const uint64_t off = va - sym->va;
      return off == 0 ? sym->name : std::format("{}+0x{:x}", sym->name, off);
    }
    if (machoMeta != nullptr) {
      if (const auto m = machoMeta->nearestObjcMethodBefore(image, va);
          m.has_value() && (va - m->first) <= 0x10000) {
        const uint64_t off = va - m->first;
        return off == 0 ? m->second.displayName
                        : std::format("{}+0x{:x}", m->second.displayName, off);
      }
      if (const auto b = machoMeta->functionBoundsAt(image, va)) {
        const uint64_t off = va - b->first;
        return off == 0 ? std::format("sub_{:x}", b->first)
                        : std::format("sub_{:x}+0x{:x}", b->first, off);
      }
    }
    return std::format("0x{:x}", va);
  };

  const auto callTool = [&](std::string_view name,
                            const xdec::json::Value& args) -> xdec::json::Value {
    if (name == "xdec_info") {
      return mcpTextResult(
          xdec::json::Value::object({
              {"path", image.path()},
              {"format", std::string{toString(image.format())}},
              {"arch", std::string{toString(image.arch())}},
              {"file_size", static_cast<uint64_t>(image.fileSize())},
              {"sections", static_cast<uint64_t>(image.sections().size())},
              {"symbols", static_cast<uint64_t>(image.symbols().size())},
              {"relocations", static_cast<uint64_t>(image.relocations().size())},
          })
              .dump(2));
    }

    if (name == "xdec_objc") {
      if (machoMeta == nullptr) {
        return mcpTextResult("not a Mach-O image with ObjC metadata", true);
      }
      const std::string clsFilter = args.stringAt("class").value_or("");
      const std::string selFilter = args.stringAt("sel").value_or("");
      const std::size_t limit =
          static_cast<std::size_t>(args.intAt("limit").value_or(30));
      const auto& classes = machoMeta->objcClasses(image);
      std::vector<xdec::json::Value> out;
      for (const auto& cls : classes) {
        if (!matchTextCI(cls.name, clsFilter)) {
          continue;
        }
        if (!selFilter.empty() &&
            std::none_of(cls.methods.begin(), cls.methods.end(), [&](const auto& m) {
              return matchTextCI(m.selector, selFilter);
            })) {
          continue;
        }
        std::vector<xdec::json::Value> ivars;
        for (const auto& iv : cls.ivars) {
          ivars.push_back(xdec::json::Value::object({
              {"name", iv.name},
              {"type", iv.typeEncoding},
              {"offset", static_cast<int64_t>(iv.offset)},
              {"slot", std::format("0x{:x}", iv.offsetSlotVa)},
          }));
        }
        std::vector<xdec::json::Value> methods;
        for (const auto& m : cls.methods) {
          if (!selFilter.empty() && !matchTextCI(m.selector, selFilter)) {
            continue;
          }
          methods.push_back(xdec::json::Value::object({
              {"kind", std::string(1, m.kind)},
              {"selector", m.selector},
              {"type", m.typeEncoding},
              {"imp", std::format("0x{:x}", m.impVa)},
          }));
        }
        out.push_back(xdec::json::Value::object({
            {"name", cls.name},
            {"super", cls.superName},
            {"class_va", std::format("0x{:x}", cls.classVa)},
            {"instance_size", static_cast<uint64_t>(cls.instanceSize)},
            {"ivars", xdec::json::Value::array(std::move(ivars))},
            {"methods", xdec::json::Value::array(std::move(methods))},
        }));
        if (limit != 0 && out.size() >= limit) {
          break;
        }
      }
      return mcpTextResult(
          xdec::json::Value::object({
              {"matched", static_cast<uint64_t>(out.size())},
              {"classes", xdec::json::Value::array(std::move(out))},
          })
              .dump(2));
    }

    if (name == "xdec_functions") {
      const std::string grep = args.stringAt("grep").value_or("");
      const std::size_t limit =
          static_cast<std::size_t>(args.intAt("limit").value_or(100));
      std::vector<xdec::json::Value> items;
      std::unordered_set<uint64_t> seen;
      for (const auto& sym : image.symbols()) {
        if (sym.defined && sym.kind == xdec::binary::SymbolKind::Function &&
            matchTextCI(sym.name, grep) && seen.insert(sym.va).second) {
          items.push_back(xdec::json::Value::object({
              {"va", std::format("0x{:x}", sym.va)},
              {"name", sym.name},
              {"size", sym.size},
          }));
          if (limit != 0 && items.size() >= limit) {
            break;
          }
        }
      }
      if (machoMeta != nullptr && (limit == 0 || items.size() < limit)) {
        for (const auto& [impVa, info] : machoMeta->allObjcMethods(image)) {
          if ((matchTextCI(info.displayName, grep) || matchTextCI(info.cName, grep)) &&
              seen.insert(impVa).second) {
            items.push_back(xdec::json::Value::object({
                {"va", std::format("0x{:x}", impVa)},
                {"name", info.displayName},
                {"encoding", info.typeEncoding},
            }));
            if (limit != 0 && items.size() >= limit) {
              break;
            }
          }
        }
      }
      return mcpTextResult(
          xdec::json::Value::object({
              {"count", static_cast<uint64_t>(items.size())},
              {"functions", xdec::json::Value::array(std::move(items))},
          })
              .dump(2));
    }

    if (name == "xdec_strings") {
      xdec::analysis::StringScanOptions scanOpts;
      scanOpts.filter = args.stringAt("grep").value_or("");
      scanOpts.minLength = static_cast<std::size_t>(args.intAt("min_len").value_or(4));
      scanOpts.limit = static_cast<std::size_t>(args.intAt("limit").value_or(100));
      const bool withXrefs = args.boolAt("xrefs").value_or(false);
      auto hits = xdec::analysis::scanCStringSections(stringSpans, scanOpts);
      if (machoMeta != nullptr && (scanOpts.limit == 0 || hits.size() < scanOpts.limit)) {
        const auto& sec = machoMeta->sections().cfstring;
        for (uint64_t off = 0; off + 32 <= sec.size; off += 32) {
          const uint64_t va = sec.va + off;
          if (const auto cf = machoMeta->cfStringAt(image, va);
              cf && cf->size() >= scanOpts.minLength && matchTextCI(*cf, scanOpts.filter)) {
            hits.push_back(xdec::analysis::ImageStringHit{va, "__cfstring", *cf});
            if (scanOpts.limit != 0 && hits.size() >= scanOpts.limit) {
              break;
            }
          }
        }
      }
      std::unordered_map<uint64_t, std::vector<std::string>> xrefsByTarget;
      if (withXrefs && !hits.empty()) {
        std::vector<uint64_t> vas;
        vas.reserve(hits.size());
        for (const auto& h : hits) {
          vas.push_back(h.va);
        }
        xdec::analysis::XrefScanOptions xOpts;
        xOpts.followStubs = false;
        xOpts.limit = 0;
        for (const auto& xr :
             xdec::analysis::scanAarch64Xrefs(codeSpans, vas, rawReader, xOpts)) {
          auto& list = xrefsByTarget[xr.targetVa];
          if (list.size() < 5) {
            list.push_back(formatOwner(xr.insnVa));
          }
        }
      }
      std::vector<xdec::json::Value> items;
      for (const auto& h : hits) {
        std::vector<xdec::json::Value> refs;
        if (withXrefs) {
          if (const auto it = xrefsByTarget.find(h.va); it != xrefsByTarget.end()) {
            for (const auto& r : it->second) {
              refs.emplace_back(r);
            }
          }
        }
        items.push_back(xdec::json::Value::object({
            {"va", std::format("0x{:x}", h.va)},
            {"section", h.section},
            {"text", h.text},
            {"xrefs", xdec::json::Value::array(std::move(refs))},
        }));
      }
      return mcpTextResult(
          xdec::json::Value::object({
              {"count", static_cast<uint64_t>(items.size())},
              {"strings", xdec::json::Value::array(std::move(items))},
          })
              .dump(2));
    }

    if (name == "xdec_xrefs") {
      const std::string query = args.stringAt("query").value_or("");
      if (query.empty()) {
        return mcpTextResult("missing 'query'", true);
      }
      xdec::analysis::XrefScanOptions scanOpts;
      scanOpts.limit = static_cast<std::size_t>(args.intAt("limit").value_or(200));
      std::vector<uint64_t> targets;
      std::unordered_map<uint64_t, std::string> targetLabels;
      const auto addTarget = [&](uint64_t tVa, std::string label) {
        if (targetLabels.try_emplace(tVa, std::move(label)).second) {
          targets.push_back(tVa);
        }
      };

      uint64_t va = 0;
      if (parseNumber(query, va)) {
        std::string label = std::format("0x{:x}", va);
        if (const auto* sym = image.symbolAt(va)) {
          label = sym->name;
        } else if (const auto imp = image.importNameAt(va)) {
          label = std::format("ptr_{}", *imp);
        } else if (machoMeta != nullptr) {
          if (const auto m = machoMeta->objcMethodAt(image, va)) {
            label = m->displayName;
          } else if (const auto d = machoMeta->resolveDataSymbolAt(image, va)) {
            label = *d;
          }
        }
        addTarget(va, std::move(label));
        if (machoMeta != nullptr) {
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
                    addTarget(sr.va + off, std::format("sel:\"{}\"", *s));
                  }
                }
              }
            }
          }
        }
      } else if (query.starts_with("sel:") && machoMeta != nullptr) {
        const std::string_view needle = std::string_view{query}.substr(4);
        const auto& sr = machoMeta->sections().objcSelrefs;
        for (uint64_t off = 0; off + 8 <= sr.size; off += 8) {
          if (const auto p = image.readUnsigned(sr.va + off, 8)) {
            if (const auto s = image.readCString(*p, 160);
                s && (*s == needle || matchTextCI(*s, needle))) {
              addTarget(sr.va + off, std::format("sel:\"{}\"", *s));
            }
          }
        }
      } else if (query.starts_with("str:")) {
        const std::string_view strNeedle = std::string_view{query}.substr(4);
        xdec::analysis::StringScanOptions strOpts;
        strOpts.filter = std::string{strNeedle};
        strOpts.minLength = 1;
        strOpts.limit = 64;
        for (const auto& s : xdec::analysis::scanCStringSections(stringSpans, strOpts)) {
          addTarget(s.va, std::format("\"{}\"", s.text));
        }
        if (machoMeta != nullptr) {
          const auto& sec = machoMeta->sections().cfstring;
          for (uint64_t off = 0; off + 32 <= sec.size; off += 32) {
            const uint64_t cfVa = sec.va + off;
            if (const auto cf = machoMeta->cfStringAt(image, cfVa);
                cf && matchTextCI(*cf, strNeedle)) {
              addTarget(cfVa, std::format("@\"{}\"", *cf));
            }
          }
        }
      } else {
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
            if (sym.defined && matchTextCI(sym.name, query)) {
              addTarget(sym.va, sym.name);
              if (targets.size() >= 32) {
                break;
              }
            }
          }
          for (const auto& reloc : image.relocations()) {
            if (const auto imp = image.importNameAt(reloc.va); imp && matchTextCI(*imp, query)) {
              addTarget(reloc.va, std::string{*imp});
              if (targets.size() >= 64) {
                break;
              }
            }
          }
          if (machoMeta != nullptr && targets.size() < 64) {
            for (const auto& [impVa, info] : machoMeta->allObjcMethods(image)) {
              if (matchTextCI(info.displayName, query)) {
                addTarget(impVa, info.displayName);
                if (targets.size() >= 64) {
                  break;
                }
              }
            }
          }
        }
      }
      const auto hits =
          xdec::analysis::scanAarch64Xrefs(codeSpans, targets, rawReader, scanOpts);
      std::vector<xdec::json::Value> items;
      for (const auto& h : hits) {
        items.push_back(xdec::json::Value::object({
            {"va", std::format("0x{:x}", h.insnVa)},
            {"kind", std::string{xdec::analysis::toString(h.kind)}},
            {"target_va", std::format("0x{:x}", h.targetVa)},
            {"target_label", targetLabels[h.targetVa]},
            {"via_stub", h.viaStubVa != 0 ? std::format("0x{:x}", h.viaStubVa) : std::string{}},
            {"function", formatOwner(h.insnVa)},
        }));
      }
      return mcpTextResult(
          xdec::json::Value::object({
              {"query", query},
              {"count", static_cast<uint64_t>(items.size())},
              {"xrefs", xdec::json::Value::array(std::move(items))},
          })
              .dump(2));
    }

    if (name == "xdec_data") {
      const std::string addrStr = args.stringAt("address").value_or("");
      uint64_t va = 0;
      if (!parseNumber(addrStr, va)) {
        return mcpTextResult("invalid 'address'", true);
      }
      const std::size_t count =
          static_cast<std::size_t>(args.intAt("count").value_or(8));
      std::vector<xdec::json::Value> slots;
      for (std::size_t i = 0; i < std::min<std::size_t>(count, 128); ++i) {
        const uint64_t slotVa = va + i * 8;
        const auto val = image.readUnsigned(slotVa, 8);
        if (!val) {
          break;
        }
        std::string note;
        if (const auto imp = image.importNameAt(slotVa)) {
          note = std::format("import {}", *imp);
        } else if (machoMeta != nullptr) {
          if (const auto m = machoMeta->objcMethodAt(image, *val)) {
            note = m->displayName;
          } else if (const auto d = machoMeta->resolveDataSymbolAt(image, *val)) {
            note = *d;
          }
        }
        slots.push_back(xdec::json::Value::object({
            {"va", std::format("0x{:x}", slotVa)},
            {"value", std::format("0x{:x}", *val)},
            {"note", note},
        }));
      }
      return mcpTextResult(
          xdec::json::Value::object({
              {"address", std::format("0x{:x}", va)},
              {"cfstring", machoMeta != nullptr
                               ? machoMeta->cfStringAt(image, va).value_or("")
                               : std::string{}},
              {"slots", xdec::json::Value::array(std::move(slots))},
          })
              .dump(2));
    }

    if (name == "xdec_callers") {
      const std::string query = args.stringAt("query").value_or("");
      if (query.empty()) {
        return mcpTextResult("missing 'query'", true);
      }
      const unsigned maxDepth = static_cast<unsigned>(
          std::clamp<int64_t>(args.intAt("depth").value_or(1), 1, 4));
      const std::size_t limit =
          static_cast<std::size_t>(args.intAt("limit").value_or(100));

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
          if (sym.defined && sym.kind == xdec::binary::SymbolKind::Function &&
              sym.name == query) {
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
        return mcpTextResult(std::format("no target matching '{}'", query), true);
      }

      std::vector<xdec::json::Value> items;
      std::unordered_set<uint64_t> visited(frontier.begin(), frontier.end());
      for (unsigned d = 1; d <= maxDepth && !frontier.empty(); ++d) {
        std::vector<uint64_t> scanTargets = frontier;
        if (machoMeta != nullptr && !machoMeta->sections().objcSelrefs.empty()) {
          for (const uint64_t fVa : frontier) {
            if (const auto m = machoMeta->objcMethodAt(image, fVa)) {
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
        xdec::analysis::XrefScanOptions xOpts;
        xOpts.limit = 0;
        const auto hits =
            xdec::analysis::scanAarch64Xrefs(codeSpans, scanTargets, rawReader, xOpts);
        std::vector<uint64_t> nextFrontier;
        std::unordered_set<uint64_t> seenAtDepth;
        for (const auto& h : hits) {
          const auto [callerVa, callerName] = enclosingStartVa(h.insnVa);
          if (!seenAtDepth.insert(callerVa).second) {
            continue;
          }
          items.push_back(xdec::json::Value::object({
              {"depth", static_cast<uint64_t>(d)},
              {"call_va", std::format("0x{:x}", h.insnVa)},
              {"caller_va", std::format("0x{:x}", callerVa)},
              {"caller_name", callerName},
              {"target_va", std::format("0x{:x}", h.targetVa)},
          }));
          if (visited.insert(callerVa).second) {
            nextFrontier.push_back(callerVa);
          }
          if (limit != 0 && items.size() >= limit) {
            break;
          }
        }
        frontier = std::move(nextFrontier);
        if (limit != 0 && items.size() >= limit) {
          break;
        }
      }
      return mcpTextResult(
          xdec::json::Value::object({
              {"query", query},
              {"count", static_cast<uint64_t>(items.size())},
              {"callers", xdec::json::Value::array(std::move(items))},
          })
              .dump(2));
    }

    if (name == "xdec_callees") {
      uint64_t startVa = 0;
      const std::string addrStr = args.stringAt("address").value_or("");
      const std::string symStr = args.stringAt("symbol").value_or("");
      if (!addrStr.empty()) {
        if (!parseNumber(addrStr, startVa)) {
          return mcpTextResult("invalid 'address'", true);
        }
      } else if (!symStr.empty()) {
        bool found = false;
        for (const auto& sym : image.symbols()) {
          if (sym.defined && sym.kind == xdec::binary::SymbolKind::Function &&
              (sym.name == symStr || matchTextCI(sym.name, symStr))) {
            startVa = sym.va;
            found = true;
            if (sym.name == symStr) {
              break;
            }
          }
        }
        if (!found && machoMeta != nullptr) {
          for (const auto& [impVa, info] : machoMeta->allObjcMethods(image)) {
            if (info.displayName == symStr || info.cName == symStr ||
                matchTextCI(info.displayName, symStr)) {
              startVa = impVa;
              found = true;
              if (info.displayName == symStr) {
                break;
              }
            }
          }
        }
        if (!found) {
          return mcpTextResult(std::format("no function matching '{}'", symStr), true);
        }
      } else {
        return mcpTextResult("provide 'address' or 'symbol'", true);
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
        return mcpTextResult("address not backed by mapped code", true);
      }
      std::array<uint64_t, 31> adrpPage{};
      std::array<uint32_t, 31> adrpSeq{};
      std::array<std::string, 31> regSel{};
      std::array<std::string, 31> regCls{};
      uint32_t seq = 0;
      std::vector<xdec::json::Value> items;
      const std::size_t count = bytes.size() / 4;
      for (std::size_t i = 0; i < count; ++i) {
        ++seq;
        const uint64_t pc = startVa + i * 4;
        uint32_t w = 0;
        std::memcpy(&w, bytes.data() + i * 4, 4);
        if ((w & 0xffe0ffe0u) == 0xaa0003e0u) {
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
            adrpPage[rd] =
                (pc & ~uint64_t{0xfff}) + static_cast<uint64_t>(static_cast<int64_t>(sext) << 12);
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
              if (const auto sp = image.readUnsigned(slotVa, 8)) {
                if (const auto s = image.readCString(*sp, 160)) {
                  regSel[rt] = *s;
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
          continue;
        }
        std::string cName;
        if (const auto got = xdec::analysis::pltGotSlot(rawReader, dest)) {
          if (const auto imp = image.importNameAt(*got)) {
            cName = std::string{*imp};
          }
          if (const auto selSlot = xdec::analysis::objcStubSelectorSlot(rawReader, dest)) {
            if (const auto selPtr = image.readUnsigned(*selSlot, 8)) {
              if (const auto sel = image.readCString(*selPtr, 160)) {
                cName += std::format("${}", *sel);
              }
            }
          }
        }
        uint64_t cur = dest;
        bool chased = false;
        for (unsigned hop = 0; hop < 6; ++hop) {
          const auto wOpt = image.readUnsigned(cur, 4);
          if (!wOpt || (*wOpt & 0xfc000000u) != 0x14000000u) {
            break;
          }
          const uint32_t bImm26 = static_cast<uint32_t>(*wOpt) & 0x03ffffffu;
          const int32_t bSext = static_cast<int32_t>(bImm26 << 6) >> 6;
          cur = cur + static_cast<uint64_t>(static_cast<int64_t>(bSext) << 2);
          chased = true;
        }
        if (cName.empty()) {
          if (const auto* s = image.symbolAt(cur)) {
            cName = chased ? ("j_" + s->name) : s->name;
          } else if (machoMeta != nullptr) {
            if (const auto m = machoMeta->objcMethodAt(image, cur)) {
              cName = chased ? ("j_" + m->displayName) : m->displayName;
            }
          }
        }
        if (cName.empty()) {
          cName = chased ? std::format("j_sub_{:x}", cur) : std::format("sub_{:x}", dest);
        }
        if ((cName == "_objc_msgSend" || cName == "_objc_msgSendSuper2") &&
            !regSel[1].empty()) {
          cName += !regCls[0].empty() ? std::format(" [{} {}]", regCls[0], regSel[1])
                                      : std::format(" sel:\"{}\"", regSel[1]);
        }
        items.push_back(xdec::json::Value::object({
            {"site", std::format("0x{:x}", pc)},
            {"target", std::format("0x{:x}", chased ? cur : dest)},
            {"name", cName},
        }));
        for (unsigned r = 0; r <= 18; ++r) {
          adrpSeq[r] = 0;
          regSel[r].clear();
          regCls[r].clear();
        }
      }
      return mcpTextResult(
          xdec::json::Value::object({
              {"function", std::format("0x{:x}", startVa)},
              {"count", static_cast<uint64_t>(items.size())},
              {"callees", xdec::json::Value::array(std::move(items))},
          })
              .dump(2));
    }

    if (name == "xdec_disasm") {
      const std::string addrStr = args.stringAt("address").value_or("");
      uint64_t va = 0;
      if (!parseNumber(addrStr, va)) {
        return mcpTextResult("invalid 'address'", true);
      }
      const std::size_t count =
          static_cast<std::size_t>(std::clamp<int64_t>(args.intAt("count").value_or(32), 1, 512));
      const unsigned width = engine.program().insnWidth / 8;
      std::vector<std::byte> buf(count * width);
      if (auto rd = image.read(va, buf); !rd) {
        return mcpTextResult(rd.error().format(), true);
      }
      std::string out;
      for (std::size_t i = 0; i < count; ++i) {
        const uint64_t pc = va + i * width;
        const auto slice = std::span<const std::byte>{buf}.subspan(i * width, width);
        const auto insn = engine.decode(slice, pc);
        uint32_t rawWord = 0;
        if (width == 4) {
          std::memcpy(&rawWord, slice.data(), 4);
        }
        out += std::format("0x{:08x}  {:08x}  {}\n", pc, rawWord,
                           insn.valid ? engine.disassemble(insn) : std::string{".word"});
      }
      return mcpTextResult(std::move(out));
    }

    if (name == "xdec_read") {
      const std::string addrStr = args.stringAt("address").value_or("");
      uint64_t va = 0;
      if (!parseNumber(addrStr, va)) {
        return mcpTextResult("invalid 'address'", true);
      }
      const std::size_t size =
          static_cast<std::size_t>(std::clamp<int64_t>(args.intAt("size").value_or(64), 1, 4096));
      std::vector<std::byte> buf(size);
      if (auto rd = image.read(va, buf); !rd) {
        return mcpTextResult(rd.error().format(), true);
      }
      std::string out;
      for (std::size_t off = 0; off < size; off += 16) {
        std::string hex;
        std::string ascii;
        for (std::size_t col = 0; col < 16; ++col) {
          if (off + col >= size) {
            hex += "   ";
            continue;
          }
          const auto b = std::to_integer<uint8_t>(buf[off + col]);
          hex += std::format("{:02x} ", b);
          ascii += (b >= 0x20 && b < 0x7f) ? static_cast<char>(b) : '.';
        }
        out += std::format("0x{:08x}  {} |{}|\n", va + off, hex, ascii);
      }
      return mcpTextResult(std::move(out));
    }

    if (name == "xdec_decompile") {
      uint64_t address = 0;
      const std::string addrStr = args.stringAt("address").value_or("");
      const std::string symStr = args.stringAt("symbol").value_or("");
      if (!addrStr.empty()) {
        if (!parseNumber(addrStr, address)) {
          return mcpTextResult("invalid 'address'", true);
        }
      } else if (!symStr.empty()) {
        bool found = false;
        for (const auto& s : image.symbols()) {
          if (s.defined && s.kind == xdec::binary::SymbolKind::Function &&
              (s.name == symStr || matchTextCI(s.name, symStr))) {
            address = s.va;
            found = true;
            if (s.name == symStr) {
              break;
            }
          }
        }
        if (!found && machoMeta != nullptr) {
          for (const auto& [impVa, info] : machoMeta->allObjcMethods(image)) {
            if (info.displayName == symStr || info.cName == symStr ||
                matchTextCI(info.displayName, symStr)) {
              address = impVa;
              found = true;
              if (info.displayName == symStr) {
                break;
              }
            }
          }
        }
        if (!found) {
          return mcpTextResult(std::format("symbol '{}' not found", symStr), true);
        }
      } else {
        return mcpTextResult("provide 'address' or 'symbol'", true);
      }

      xdec::decompile::DecompileToCOptions opts;
      opts.driver = session->driverOptions();
      opts.driver.target = xdec::il::Maturity::Vars;
      opts.driver.sealUnresolvedBranches = args.boolAt("allow_unresolved").value_or(true);
      opts.profile = &session->profile;
      opts.structure.astTransforms = pluginAstTransforms;
      opts.emit.symbols = session->symbols();
      opts.emit.addresses = session->addresses();
      opts.emit.imageReader = reader;
      opts.emit.entryRegs =
          session->entryRegFacts().empty() ? nullptr : &session->entryRegFacts();
      opts.emit.verboseNotes = args.boolAt("verbose_notes").value_or(false);
      const std::string lang = args.stringAt("lang").value_or("c");
      opts.emit.language =
          (lang == "objc" || lang == "oc") ? xdec::emit::EmitLanguage::Objc
                                           : xdec::emit::EmitLanguage::C;
      opts.emit.hlTransforms = pluginHlTransforms;

      const auto unwindBounds =
          machoMeta != nullptr ? machoMeta->functionBoundsAt(image, address) : std::nullopt;
      const bool knownUnwind = unwindBounds.has_value() && unwindBounds->first == address;
      opts.driver.isTailCall = [&image, machoMeta, &reader, entryAddr = address,
                                unwindBounds, knownUnwind](uint64_t, uint64_t target) {
        if (target == entryAddr) {
          return false;
        }
        if (xdec::analysis::pltGotSlot(reader, target).has_value()) {
          return true;
        }
        if (const auto* s = image.symbolAt(target);
            s != nullptr && s->kind == xdec::binary::SymbolKind::Function) {
          return true;
        }
        if (machoMeta != nullptr) {
          if (machoMeta->objcMethodAt(image, target).has_value()) {
            return true;
          }
          if (const auto b = machoMeta->functionBoundsAt(image, target);
              b.has_value() && b->first == target) {
            return true;
          }
        }
        (void)unwindBounds;
        (void)knownUnwind;
        return false;
      };

      auto res = xdec::decompile::decompileToC(engine, reader, address, registry, opts);
      if (!res) {
        return mcpTextResult(res.error().format(), true);
      }
      return mcpTextResult(res->cSource);
    }

    return mcpTextResult(std::format("unknown tool '{}'", name), true);
  };

  const xdec::json::Value toolList = xdec::json::Value::array({
      mcpToolDef("xdec_info", "Summarize the loaded binary image.",
                 xdec::json::Value::object({{"type", "object"}})),
      mcpToolDef("xdec_objc", "Query Mach-O Objective-C classes, ivars, and methods.",
                 xdec::json::Value::object({
                     {"type", "object"},
                     {"properties", xdec::json::Value::object({
                                        {"class", xdec::json::Value::object({{"type", "string"}})},
                                        {"sel", xdec::json::Value::object({{"type", "string"}})},
                                        {"limit", xdec::json::Value::object({{"type", "integer"}})},
                                    })},
                 })),
      mcpToolDef("xdec_functions", "List defined functions and ObjC methods.",
                 xdec::json::Value::object({
                     {"type", "object"},
                     {"properties", xdec::json::Value::object({
                                        {"grep", xdec::json::Value::object({{"type", "string"}})},
                                        {"limit", xdec::json::Value::object({{"type", "integer"}})},
                                    })},
                 })),
      mcpToolDef("xdec_strings", "Scan CStrings and CFStrings with optional xrefs.",
                 xdec::json::Value::object({
                     {"type", "object"},
                     {"properties", xdec::json::Value::object({
                                        {"grep", xdec::json::Value::object({{"type", "string"}})},
                                        {"min_len", xdec::json::Value::object({{"type", "integer"}})},
                                        {"limit", xdec::json::Value::object({{"type", "integer"}})},
                                        {"xrefs", xdec::json::Value::object({{"type", "boolean"}})},
                                    })},
                 })),
      mcpToolDef("xdec_xrefs", "Find ARM64 callers and ADRP+ADD/LDR references.",
                 xdec::json::Value::object({
                     {"type", "object"},
                     {"properties", xdec::json::Value::object({
                                        {"query", xdec::json::Value::object({{"type", "string"}})},
                                        {"limit", xdec::json::Value::object({{"type", "integer"}})},
                                    })},
                     {"required", xdec::json::Value::array({"query"})},
                 })),
      mcpToolDef("xdec_data", "Inspect structured data/CFString/pointer slots at an address.",
                 xdec::json::Value::object({
                     {"type", "object"},
                     {"properties", xdec::json::Value::object({
                                        {"address", xdec::json::Value::object({{"type", "string"}})},
                                        {"count", xdec::json::Value::object({{"type", "integer"}})},
                                    })},
                     {"required", xdec::json::Value::array({"address"})},
                 })),
      mcpToolDef("xdec_callers", "Trace callers of an address, symbol, or ObjC method up to depth N.",
                 xdec::json::Value::object({
                     {"type", "object"},
                     {"properties", xdec::json::Value::object({
                                        {"query", xdec::json::Value::object({{"type", "string"}})},
                                        {"depth", xdec::json::Value::object({{"type", "integer"}})},
                                        {"limit", xdec::json::Value::object({{"type", "integer"}})},
                                    })},
                     {"required", xdec::json::Value::array({"query"})},
                 })),
      mcpToolDef("xdec_callees", "List direct, stub, branch-island, and ObjC callees inside a function.",
                 xdec::json::Value::object({
                     {"type", "object"},
                     {"properties", xdec::json::Value::object({
                                        {"address", xdec::json::Value::object({{"type", "string"}})},
                                        {"symbol", xdec::json::Value::object({{"type", "string"}})},
                                    })},
                 })),
      mcpToolDef("xdec_disasm", "Disassemble N AArch64 instructions at an address.",
                 xdec::json::Value::object({
                     {"type", "object"},
                     {"properties", xdec::json::Value::object({
                                        {"address", xdec::json::Value::object({{"type", "string"}})},
                                        {"count", xdec::json::Value::object({{"type", "integer"}})},
                                    })},
                     {"required", xdec::json::Value::array({"address"})},
                 })),
      mcpToolDef("xdec_read", "Hex + ASCII dump bytes from the unified relocated memory view.",
                 xdec::json::Value::object({
                     {"type", "object"},
                     {"properties", xdec::json::Value::object({
                                        {"address", xdec::json::Value::object({{"type", "string"}})},
                                        {"size", xdec::json::Value::object({{"type", "integer"}})},
                                    })},
                     {"required", xdec::json::Value::array({"address"})},
                 })),
      mcpToolDef("xdec_decompile", "Decompile a function by hex address or symbol name to C or Objective-C.",
                 xdec::json::Value::object({
                     {"type", "object"},
                     {"properties", xdec::json::Value::object({
                                        {"address", xdec::json::Value::object({{"type", "string"}})},
                                        {"symbol", xdec::json::Value::object({{"type", "string"}})},
                                        {"lang", xdec::json::Value::object({{"type", "string"}})},
                                        {"allow_unresolved", xdec::json::Value::object({{"type", "boolean"}})},
                                        {"verbose_notes", xdec::json::Value::object({{"type", "boolean"}})},
                                    })},
                 })),
  });

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) {
      continue;
    }
    auto parsed = xdec::json::parse(line);
    if (!parsed) {
      std::cout << jsonRpcErr(xdec::json::Value{nullptr}, -32700, parsed.error().format())
                       .dump(0)
                << '\n'
                << std::flush;
      continue;
    }
    const auto* idVal = parsed->find("id");
    const std::string method = parsed->stringAt("method").value_or("");
    if (idVal == nullptr) {
      // JSON-RPC notification (e.g. notifications/initialized): no reply.
      continue;
    }
    if (method == "initialize") {
      std::cout << jsonRpcOk(
                       *idVal,
                       xdec::json::Value::object({
                           {"protocolVersion", "2024-11-05"},
                           {"capabilities",
                            xdec::json::Value::object({
                                {"tools", xdec::json::Value::object({})},
                            })},
                           {"serverInfo",
                            xdec::json::Value::object({
                                {"name", "xdec"},
                                {"version", "0.1.0"},
                            })},
                       }))
                       .dump(0)
                << '\n'
                << std::flush;
    } else if (method == "ping") {
      std::cout << jsonRpcOk(*idVal, xdec::json::Value::object({})).dump(0) << '\n'
                << std::flush;
    } else if (method == "tools/list") {
      std::cout << jsonRpcOk(*idVal, xdec::json::Value::object({{"tools", toolList}})).dump(0)
                << '\n'
                << std::flush;
    } else if (method == "tools/call") {
      const auto* params = parsed->find("params");
      const std::string toolName =
          params != nullptr ? params->stringAt("name").value_or("") : std::string{};
      const xdec::json::Value* toolArgs =
          params != nullptr ? params->find("arguments") : nullptr;
      const xdec::json::Value emptyArgs = xdec::json::Value::object({});
      std::cout << jsonRpcOk(*idVal,
                             callTool(toolName, toolArgs != nullptr ? *toolArgs : emptyArgs))
                       .dump(0)
                << '\n'
                << std::flush;
    } else {
      std::cout << jsonRpcErr(*idVal, -32601, std::format("method '{}' not found", method))
                       .dump(0)
                << '\n'
                << std::flush;
    }
  }
  return 0;
}

}  // namespace xdec::cli
