// objc_libcxx plugin: specialized Apple Mach-O Objective-C & libc++ recovery.
//
// Capabilities:
//   1. `objc-arity-trim` (IL Pass at Ssa):
//      Identifies `_objc_msgSend` / `_objc_msgSendSuper2` calls (including
//      Apple `__objc_stubs` selector trampolines) whose selector resolves
//      through Mach-O `__objc_selrefs` -> `__objc_methname`, trims trailing
//      unused argument registers (`x{arity}..x7`) based on `2 + count(':')`,
//      and attaches a human-readable selector note.
//   2. `objc-retain-elide` (IL Pass at Ssa, disabled when XDEC_OBJC_KEEP_ARC=1):
//      Elides redundant ARC retain/release/autoreleasedReturnValue calls so
//      Objective-C methods read like source code rather than retain-count
//      bookkeeping.
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "xdec/analysis/call_abi.h"
#include "xdec/analysis/call_target.h"
#include "xdec/analysis/plt_stub.h"
#include "xdec/il/function.h"
#include "xdec/plugin/abi.h"
#include "xdec/support/reader.h"

namespace {

constexpr std::size_t kCallTargetOperand = 0;
constexpr std::size_t kSelfArgOperand = 1;
constexpr std::size_t kSelectorArgOperand = 2;

[[nodiscard]] bool isObjcMsgSendName(std::string_view name) noexcept {
  return name == "_objc_msgSend" || name == "objc_msgSend" ||
         name == "_objc_msgSendSuper2" || name == "objc_msgSendSuper2";
}

[[nodiscard]] bool isObjcRetainPassThroughName(std::string_view name) noexcept {
  return name == "_objc_retain" || name == "objc_retain" ||
         name == "_objc_retainAutoreleasedReturnValue" ||
         name == "objc_retainAutoreleasedReturnValue" ||
         name == "_objc_unsafeClaimAutoreleasedReturnValue" ||
         name == "objc_unsafeClaimAutoreleasedReturnValue" ||
         name == "_objc_autoreleaseReturnValue" ||
         name == "objc_autoreleaseReturnValue";
}

[[nodiscard]] bool isObjcReleaseName(std::string_view name) noexcept {
  return name == "_objc_release" || name == "objc_release" ||
         name == "_objc_autorelease" || name == "objc_autorelease";
}

[[nodiscard]] std::string callSymbolName(const xdec::pass::Context& context,
                                         const xdec::il::Function& function,
                                         xdec::il::ExprId target) {
  uint64_t direct = 0;
  if (function.asConstant(target, direct)) {
    return context.nameAt(direct).name;
  }
  const xdec::analysis::CallTargetShape shape =
      xdec::analysis::describeCallTarget(function, target);
  if (shape.hasTableBase && shape.terms.empty()) {
    return context.memoryFacts().loaderValueAt(shape.tableBase).importName;
  }
  return {};
}

[[nodiscard]] xdec::il::ExprId unwrapCasts(const xdec::il::Function& function,
                                           xdec::il::ExprId id) {
  while (id.valid()) {
    const xdec::il::Expr& e = function.expr(id);
    if ((e.op == xdec::il::ExprOp::ZExt || e.op == xdec::il::ExprOp::SExt ||
         e.op == xdec::il::ExprOp::Trunc || e.op == xdec::il::ExprOp::Bitcast) &&
        e.operandCount == 1) {
      id = e.operands[0];
      continue;
    }
    break;
  }
  return id;
}

[[nodiscard]] std::optional<uint64_t> readU64Le(const xdec::ByteReader& reader,
                                                uint64_t va) {
  std::array<std::byte, 8> buf{};
  if (!reader(va, std::span<std::byte>{buf})) {
    return std::nullopt;
  }
  uint64_t out = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    out |= static_cast<uint64_t>(static_cast<uint8_t>(buf[i])) << (i * 8);
  }
  return out;
}

[[nodiscard]] std::optional<std::string> readSelectorString(
    const xdec::ByteReader& reader, const xdec::MemoryFacts& facts, uint64_t strVa) {
  if (strVa == 0 || !facts.isImmutable(strVa, 1)) {
    return std::nullopt;
  }
  std::string out;
  out.reserve(48);
  for (std::size_t offset = 0; offset < 256; ++offset) {
    std::byte b{};
    if (!reader(strVa + offset, std::span<std::byte>{&b, 1})) {
      return std::nullopt;
    }
    const auto ch = static_cast<unsigned char>(b);
    if (ch == 0) {
      return out.empty() ? std::nullopt : std::optional<std::string>{std::move(out)};
    }
    if (ch < 0x20 || ch > 0x7e) {
      return std::nullopt;
    }
    out.push_back(static_cast<char>(ch));
  }
  return std::nullopt;
}

[[nodiscard]] bool looksLikeObjcSelector(std::string_view s) noexcept {
  if (s.empty()) {
    return false;
  }
  for (const char raw : s) {
    const auto ch = static_cast<unsigned char>(raw);
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || ch == '_' || ch == ':' ||
                    ch == '$';
    if (!ok) {
      return false;
    }
  }
  return true;
}

/// Resolves the selector string for `selExpr` (the `x1` operand passed to
/// `_objc_msgSend`), following direct Load(slotVa) or single-step local
/// Load/Phi-of-Load references to Mach-O `__objc_selrefs` slots.
[[nodiscard]] std::optional<std::string> resolveSelector(
    const xdec::il::Function& function, const xdec::ByteReader& reader,
    const xdec::MemoryFacts& facts, xdec::il::ExprId selExpr, unsigned depth = 0) {
  if (!selExpr.valid() || depth > 4) {
    return std::nullopt;
  }
  selExpr = unwrapCasts(function, selExpr);
  uint64_t directVa = 0;
  if (function.asConstant(selExpr, directVa)) {
    if (auto s = readSelectorString(reader, facts, directVa);
        s && looksLikeObjcSelector(*s)) {
      return s;
    }
    return std::nullopt;
  }
  const xdec::il::Expr& expr = function.expr(selExpr);
  if (expr.op != xdec::il::ExprOp::Value) {
    return std::nullopt;
  }
  const xdec::il::ValueId val{static_cast<uint32_t>(expr.immediate)};
  if (!function.hasValue(val)) {
    return std::nullopt;
  }
  const xdec::il::OpId defId = function.value(val).definition;
  if (!defId.valid() || !function.hasOp(defId)) {
    return std::nullopt;
  }
  const xdec::il::Op& def = function.op(defId);
  if (def.code == xdec::il::OpCode::Load) {
    const auto loadOps = function.operands(def);
    if (loadOps.empty()) {
      return std::nullopt;
    }
    uint64_t slotVa = 0;
    if (!function.asConstant(unwrapCasts(function, loadOps[0]), slotVa)) {
      return std::nullopt;
    }
    const auto strVa = readU64Le(reader, slotVa);
    if (!strVa.has_value()) {
      return std::nullopt;
    }
    if (auto s = readSelectorString(reader, facts, *strVa);
        s && looksLikeObjcSelector(*s)) {
      return s;
    }
    return std::nullopt;
  }
  if (def.code == xdec::il::OpCode::Phi) {
    std::optional<std::string> agreed;
    for (const xdec::il::ExprId inOp : function.operands(def)) {
      if (function.expr(inOp).op == xdec::il::ExprOp::Value &&
          xdec::il::ValueId{static_cast<uint32_t>(function.expr(inOp).immediate)} == val) {
        continue;
      }
      auto s = resolveSelector(function, reader, facts, inOp, depth + 1);
      if (!s.has_value()) {
        return std::nullopt;
      }
      if (!agreed.has_value()) {
        agreed = std::move(s);
      } else if (*agreed != *s) {
        return std::nullopt;
      }
    }
    return agreed;
  }
  return std::nullopt;
}

class ObjcArityTrimPass final : public xdec::pass::FunctionPass {
 public:
  ObjcArityTrimPass()
      : FunctionPass([] {
          xdec::pass::PassInfo info;
          info.name = "objc-arity-trim";
          info.level = xdec::il::Maturity::Ssa;
          info.produces = xdec::il::Maturity::Ssa;
          info.requirements = {"resolve-call"};
          return info;
        }()) {}

  xdec::Result<bool> run(xdec::pass::Context& context) override {
    const xdec::ByteReader* reader = context.image();
    if (reader == nullptr) {
      return false;
    }
    xdec::il::Function& function = context.function();
    const xdec::MemoryFacts& facts = context.memoryFacts();

    bool changed = false;
    for (const xdec::il::BlockId blockId : function.blockHandles()) {
      for (const xdec::il::OpId opId : function.block(blockId).ops) {
        const xdec::il::Op& op = function.op(opId);
        if (op.code != xdec::il::OpCode::Call) {
          continue;
        }
        const auto operands = function.operands(op);
        if (operands.size() <= kSelectorArgOperand) {
          continue;
        }

        const std::string symName = callSymbolName(context, function, operands[kCallTargetOperand]);
        if (symName.empty() || !isObjcMsgSendName(symName)) {
          continue;
        }

        uint64_t direct = 0;
        (void)function.asConstant(operands[kCallTargetOperand], direct);
        std::optional<std::string> selector;
        if (direct != 0) {
          if (const auto stubSlot = xdec::analysis::objcStubSelectorSlot(*reader, direct)) {
            if (const auto strVa = readU64Le(*reader, *stubSlot)) {
              if (auto s = readSelectorString(*reader, facts, *strVa);
                  s && looksLikeObjcSelector(*s)) {
                selector = std::move(s);
              }
            }
          }
        }
        if (!selector.has_value()) {
          selector = resolveSelector(function, *reader, facts, operands[kSelectorArgOperand]);
        }
        if (!selector.has_value()) {
          continue;
        }

        const std::size_t colons = static_cast<std::size_t>(
            std::count(selector->begin(), selector->end(), ':'));
        const std::size_t argCount = 2 + colons;  // self + _cmd + colon params
        const std::size_t keep = kSelfArgOperand + argCount;
        const std::string note =
            std::format("objc selector: \"{}\" ({} arg(s))", *selector, argCount);
        if (function.noteOn(opId).find(note) == std::string_view::npos) {
          function.appendNote(opId, note);
        }
        // Recover scalar floating-point arguments (`d0..d7` / `s0..s7`) for
        // selectors whose trailing parameter(s) went into FP registers and left
        // integer slot(s) `Undef` or untouched `EntryReg` (e.g.
        // `safePerformSelector:withObject:afterDelay:`).
        std::vector<xdec::il::ExprId> updated(operands.begin(), operands.end());
        bool fpFilled = false;
        unsigned nextFpSlot = 0;
        const std::size_t fillEnd = std::min(updated.size(), keep);
        for (std::size_t idx = kSelfArgOperand + 2; idx < fillEnd; ++idx) {
          const xdec::il::Expr& slotExpr = function.expr(updated[idx]);
          bool missingIntArg = slotExpr.op == xdec::il::ExprOp::Undef;
          if (!missingIntArg && slotExpr.op == xdec::il::ExprOp::EntryReg && idx >= kSelfArgOperand + 3) {
            const xdec::il::RegId root{static_cast<uint32_t>(slotExpr.immediate)};
            missingIntArg =
                function.registers().nameOf(root) ==
                std::format("x{}", idx - kSelfArgOperand);
          }
          if (missingIntArg) {
            if (const auto fp = function.callFpArg(opId, nextFpSlot); fp.has_value()) {
              updated[idx] = function.cast(
                  xdec::il::ExprOp::Bitcast,
                  xdec::il::Type::floating(fp->bits == 32 ? 32 : 64), fp->value);
              ++nextFpSlot;
              fpFilled = true;
            }
          }
        }
        if (updated.size() > keep) {
          updated.resize(keep);
          function.setOperands(opId, updated);
          changed = true;
        } else if (fpFilled) {
          function.setOperands(opId, updated);
          changed = true;
        }
      }
    }
    return changed;
  }
};

class ObjcArcElidePass final : public xdec::pass::FunctionPass {
 public:
  ObjcArcElidePass()
      : FunctionPass([] {
          xdec::pass::PassInfo info;
          info.name = "objc-retain-elide";
          info.level = xdec::il::Maturity::Ssa;
          info.produces = xdec::il::Maturity::Ssa;
          info.requirements = {"resolve-call", "objc-arity-trim"};
          return info;
        }()) {}

  xdec::Result<bool> run(xdec::pass::Context& context) override {
    if (const char* keep = std::getenv("XDEC_OBJC_KEEP_ARC");
        keep != nullptr && std::string_view{keep} == "1") {
      return false;
    }

    xdec::il::Function& function = context.function();
    std::unordered_map<uint32_t, xdec::il::ExprId> passThroughRepl;
    struct DeadCall {
      xdec::il::BlockId block;
      xdec::il::OpId op;
    };
    std::vector<DeadCall> deadCalls;
    std::unordered_set<uint32_t> deadReleaseResults;

    for (const xdec::il::BlockId blockId : function.blockHandles()) {
      for (const xdec::il::OpId opId : function.block(blockId).ops) {
        const xdec::il::Op& op = function.op(opId);
        if (op.code != xdec::il::OpCode::Call) {
          continue;
        }
        const auto operands = function.operands(op);
        if (operands.size() < 2) {
          continue;
        }
        const std::string symName = callSymbolName(context, function, operands[kCallTargetOperand]);
        if (symName.empty()) {
          continue;
        }
        if (isObjcRetainPassThroughName(symName)) {
          if (op.result.valid()) {
            passThroughRepl.emplace(op.result.index(), operands[kSelfArgOperand]);
          }
          deadCalls.push_back({blockId, opId});
        } else if (isObjcReleaseName(symName)) {
          if (op.result.valid()) {
            deadReleaseResults.insert(op.result.index());
            passThroughRepl.emplace(op.result.index(), function.undefined(op.type));
          }
          deadCalls.push_back({blockId, opId});
        }
      }
    }

    if (deadCalls.empty()) {
      return false;
    }

    std::unordered_map<xdec::il::ExprId, xdec::il::ExprId> memo;
    auto resolveExpr = [&](auto& self, xdec::il::ExprId id) -> xdec::il::ExprId {
      if (!id.valid()) {
        return id;
      }
      if (const auto cached = memo.find(id); cached != memo.end()) {
        return cached->second;
      }
      const xdec::il::Expr expr = function.expr(id);
      xdec::il::ExprId out = id;
      if (expr.op == xdec::il::ExprOp::Value) {
        const auto vIdx = static_cast<uint32_t>(expr.immediate);
        if (const auto found = passThroughRepl.find(vIdx); found != passThroughRepl.end()) {
          out = self(self, found->second);
        }
      } else if (expr.operandCount > 0) {
        xdec::il::Expr rebuilt = expr;
        bool changed = false;
        for (unsigned i = 0; i < expr.operandCount; ++i) {
          const xdec::il::ExprId next = self(self, expr.operands[i]);
          changed |= next != expr.operands[i];
          rebuilt.operands[i] = next;
        }
        if (changed) {
          out = function.intern(rebuilt);
        }
      }
      memo.emplace(id, out);
      return out;
    };

    for (const xdec::il::BlockId blockId : function.blockHandles()) {
      for (const xdec::il::OpId opId : function.block(blockId).ops) {
        const xdec::il::Op& op = function.op(opId);
        const auto operands = function.operands(op);
        if (operands.empty()) {
          continue;
        }
        if (op.code == xdec::il::OpCode::Return && operands.size() == 1) {
          const xdec::il::Expr& retExpr = function.expr(operands[0]);
          if (retExpr.op == xdec::il::ExprOp::Value &&
              deadReleaseResults.contains(static_cast<uint32_t>(retExpr.immediate))) {
            function.setOperands(opId, std::vector<xdec::il::ExprId>{});
            continue;
          }
        }
        std::vector<xdec::il::ExprId> rewritten(operands.begin(), operands.end());
        bool touched = false;
        for (xdec::il::ExprId& operand : rewritten) {
          const xdec::il::ExprId next = resolveExpr(resolveExpr, operand);
          touched |= next != operand;
          operand = next;
        }
        if (touched) {
          function.setOperands(opId, rewritten);
        }
      }
    }

    for (const DeadCall& dead : deadCalls) {
      if (function.hasOp(dead.op)) {
        function.removeOp(dead.block, dead.op);
      }
    }

    // Sweep pure Loads/Phis that became unreferenced once their ARC call users
    // were removed (e.g. GOT loads for `_objc_release` or block capture reloads).
    for (bool swept = true; swept;) {
      swept = false;
      std::vector<uint8_t> used(function.valueCount(), 0);
      std::vector<uint8_t> visitedExpr(function.exprCount(), 0);
      std::vector<xdec::il::ExprId> stack;
      const auto markExpr = [&](xdec::il::ExprId root) {
        if (!root.valid()) {
          return;
        }
        stack.clear();
        stack.push_back(root);
        while (!stack.empty()) {
          const xdec::il::ExprId cur = stack.back();
          stack.pop_back();
          if (cur.index() < visitedExpr.size()) {
            if (visitedExpr[cur.index()] != 0) {
              continue;
            }
            visitedExpr[cur.index()] = 1;
          }
          const xdec::il::Expr& e = function.expr(cur);
          if (e.op == xdec::il::ExprOp::Value) {
            const auto vIdx = static_cast<uint32_t>(e.immediate);
            if (vIdx < used.size()) {
              used[vIdx] = 1;
            }
          } else {
            for (unsigned i = 0; i < e.operandCount; ++i) {
              stack.push_back(e.operands[i]);
            }
          }
        }
      };
      for (const xdec::il::BlockId b : function.blockHandles()) {
        for (const xdec::il::OpId o : function.block(b).ops) {
          for (const xdec::il::ExprId operand : function.operands(function.op(o))) {
            markExpr(operand);
          }
        }
      }
      for (const xdec::il::BlockId b : function.blockHandles()) {
        const auto ops = function.block(b).ops;
        for (const xdec::il::OpId o : ops) {
          const xdec::il::Op& op = function.op(o);
          if ((op.code == xdec::il::OpCode::Load || op.code == xdec::il::OpCode::Phi) &&
              op.result.valid() && op.result.index() < used.size() &&
              used[op.result.index()] == 0) {
            function.removeOp(b, o);
            swept = true;
          }
        }
      }
    }
    return true;
  }
};

// ---------------------------------------------------------------------------
// Step 3: High-level AST transform for Objective-C output (`--lang objc`)
// ---------------------------------------------------------------------------

[[nodiscard]] std::optional<std::string> parseObjcSelectorComment(std::string_view comment) {
  static constexpr std::string_view kPrefix = "objc selector: \"";
  if (!comment.starts_with(kPrefix)) {
    return std::nullopt;
  }
  const std::size_t end = comment.find('"', kPrefix.size());
  if (end == std::string_view::npos) {
    return std::nullopt;
  }
  return std::string{comment.substr(kPrefix.size(), end - kPrefix.size())};
}

[[nodiscard]] std::string cleanObjcClassRefName(std::string_view raw) {
  // Recognise `(*(uint64_t*)(clsRef_Foo))`, `(*(uint64_t*)(ptr__OBJC_CLASS_$_Bar))`,
  // `(*(uint64_t*)(superRef_Baz))`, or bare `clsRef_Foo`.
  auto stripPrefix = [&](std::string_view s, std::string_view prefix) -> std::optional<std::string> {
    if (s.starts_with(prefix)) {
      return std::string{s.substr(prefix.size())};
    }
    return std::nullopt;
  };
  std::string_view inner = raw;
  static constexpr std::string_view kDeref = "(*(uint64_t*)(";
  if (inner.starts_with(kDeref) && inner.ends_with("))")) {
    inner = inner.substr(kDeref.size(), inner.size() - kDeref.size() - 2);
  }
  if (auto n = stripPrefix(inner, "clsRef_")) {
    return *n;
  }
  if (auto n = stripPrefix(inner, "ptr__OBJC_CLASS_$_")) {
    return *n;
  }
  if (auto n = stripPrefix(inner, "superRef_")) {
    return "super";
  }
  return {};
}

[[nodiscard]] std::optional<std::string> extractIvarField(std::string_view ivarSym) {
  // `ivar_<Class>_<field>` (e.g. `ivar_SessionManager_devToken` or `ivar_CookieWriter__webView`)
  if (!ivarSym.starts_with("ivar_")) {
    return std::nullopt;
  }
  const std::size_t sep = ivarSym.find('_', 5);
  if (sep == std::string_view::npos || sep + 1 >= ivarSym.size()) {
    return std::nullopt;
  }
  return std::string{ivarSym.substr(sep + 1)};
}

struct ObjcScopeMaps {
  std::unordered_map<std::string, std::string> clsTempToName;
  std::unordered_map<std::string, std::string> ivarTempToField;
  std::unordered_map<std::string, std::string> selTempToSelector;
};

[[nodiscard]] std::string selectorFromSelRefSym(std::string_view sym) {
  static constexpr std::string_view kSelPrefix = "selRef_";
  if (!sym.starts_with(kSelPrefix)) {
    return {};
  }
  std::string out{sym.substr(kSelPrefix.size())};
  if (!out.empty() && out.back() == '_') {
    for (std::size_t i = 0; i < out.size(); ++i) {
      if (out[i] == '_' && i > 0) {
        out[i] = ':';
      }
    }
  }
  return out;
}

[[nodiscard]] std::optional<std::string> parseSelRefDeref(std::string_view raw) {
  static constexpr std::string_view kPrefix = "(*(uint64_t*)(selRef_";
  if (raw.starts_with(kPrefix) && raw.ends_with("))")) {
    const std::size_t symStart = raw.find("selRef_");
    const std::string_view sym = raw.substr(symStart, raw.size() - 2 - symStart);
    if (std::string s = selectorFromSelRefSym(sym); !s.empty()) {
      return s;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string> parseProtoRefDeref(std::string_view raw) {
  static constexpr std::string_view kPrefix = "(*(uint64_t*)(protoRef_";
  if (raw.starts_with(kPrefix) && raw.ends_with("))")) {
    const std::size_t symStart = raw.find("protoRef_") + 9;
    return std::string{raw.substr(symStart, raw.size() - 2 - symStart)};
  }
  return std::nullopt;
}

[[nodiscard]] std::string rewriteCFStringsInRaw(std::string text) {
  std::size_t pos = 0;
  while ((pos = text.find("cfstr_", pos)) != std::string::npos) {
    const std::size_t commentStart = text.find(" /* @\"", pos);
    if (commentStart == std::string::npos || commentStart - pos > 64) {
      pos += 6;
      continue;
    }
    const std::size_t litStart = commentStart + 4;  // points at `@"`
    std::size_t cur = litStart + 2;
    while (cur < text.size()) {
      if (text[cur] == '\\' && cur + 1 < text.size()) {
        cur += 2;
        continue;
      }
      if (text[cur] == '"') {
        ++cur;
        break;
      }
      ++cur;
    }
    if (cur + 3 <= text.size() && std::string_view{text}.substr(cur, 3) == " */") {
      const std::string lit = text.substr(litStart, cur - litStart);
      text.replace(pos, (cur + 3) - pos, lit);
      pos += lit.size();
      continue;
    }
    pos += 6;
  }
  return text;
}

[[nodiscard]] bool isIdentChar(char c) noexcept {
  const auto uc = static_cast<unsigned char>(c);
  return std::isalnum(uc) != 0 || c == '_';
}

[[nodiscard]] std::string replaceBasePlusOffset(std::string text, std::string_view offsetPattern,
                                                std::string_view fieldName) {
  std::size_t pos = 0;
  while ((pos = text.find(offsetPattern, pos)) != std::string::npos) {
    // Expect `(<baseIdent> + <offsetPattern>)` or `((<cast>)(<baseIdent>) + <offsetPattern>)`
    if (pos >= 3 && std::string_view{text}.substr(pos - 3, 3) == " + ") {
      const std::size_t after = pos + offsetPattern.size();
      if (after < text.size() && text[after] == ')') {
        std::size_t lhsEnd = pos - 3;
        std::string baseIdent;
        std::size_t openParen = std::string::npos;
        if (lhsEnd > 0 && text[lhsEnd - 1] == ')') {
          // Check `(<cast>)(<baseIdent>)`
          const std::size_t idClose = lhsEnd - 1;
          std::size_t idStart = idClose;
          while (idStart > 0 && isIdentChar(text[idStart - 1])) {
            --idStart;
          }
          if (idStart > 1 && idStart < idClose && text[idStart - 1] == '(' &&
              text[idStart - 2] == ')') {
            const std::size_t castStart = text.rfind('(', idStart - 2);
            if (castStart != std::string::npos && castStart > 0 &&
                text[castStart - 1] == '(') {
              baseIdent = text.substr(idStart, idClose - idStart);
              openParen = castStart - 1;
            }
          }
        } else {
          std::size_t baseStart = lhsEnd;
          while (baseStart > 0 && isIdentChar(text[baseStart - 1])) {
            --baseStart;
          }
          if (baseStart > 0 && baseStart < lhsEnd && text[baseStart - 1] == '(') {
            baseIdent = text.substr(baseStart, lhsEnd - baseStart);
            openParen = baseStart - 1;
          }
        }
        if (!baseIdent.empty() && openParen != std::string::npos) {
          const std::string repl = std::format("(&{}->{})", baseIdent, fieldName);
          text.replace(openParen, (after + 1) - openParen, repl);
          pos = openParen + repl.size();
          continue;
        }
      }
    }
    pos += offsetPattern.size();
  }
  return text;
}

[[nodiscard]] std::string rewriteObjcRawText(std::string text, const ObjcScopeMaps& maps,
                                             bool replaceSelRefToAtSelector) {
  for (const auto& [t, cls] : maps.clsTempToName) {
    if (text == t) {
      return cls;
    }
  }
  text = rewriteCFStringsInRaw(std::move(text));
  if (const auto proto = parseProtoRefDeref(text)) {
    return std::format("@protocol({})", *proto);
  }
  if (replaceSelRefToAtSelector) {
    if (const auto sel = parseSelRefDeref(text)) {
      return std::format("@selector({})", *sel);
    }
    if (const auto it = maps.selTempToSelector.find(text);
        it != maps.selTempToSelector.end()) {
      return std::format("@selector({})", it->second);
    }
  }
  for (const auto& [t, ivarField] : maps.ivarTempToField) {
    text = replaceBasePlusOffset(std::move(text),
                                 std::format("((int64_t)(int32_t)({}))", t), ivarField);
    text = replaceBasePlusOffset(std::move(text), t, ivarField);
  }
  std::size_t ivPos = 0;
  while ((ivPos = text.find("((int64_t)(int32_t)((*(uint32_t*)(ivar_", ivPos)) !=
         std::string::npos) {
    const std::size_t symStart =
        ivPos + std::string_view{"((int64_t)(int32_t)((*(uint32_t*)("}.size();
    const std::size_t symEnd = text.find("))))", symStart);
    if (symEnd == std::string::npos) {
      break;
    }
    if (const auto field = extractIvarField(
            std::string_view{text}.substr(symStart, symEnd - symStart))) {
      const std::string_view pat =
          std::string_view{text}.substr(ivPos, (symEnd + 4) - ivPos);
      const std::string updated = replaceBasePlusOffset(text, pat, *field);
      if (updated != text) {
        text = updated;
        ivPos = 0;
        continue;
      }
    }
    ivPos = symEnd;
  }
  return text;
}

void transformObjcExpr(xdec::emit::HlExpr& expr,
                       const std::optional<std::string>& pendingSelector,
                       ObjcScopeMaps& maps, bool isSelectorArg = false) {
  if (expr.kind == xdec::emit::HlExprKind::Call && expr.lhs &&
      expr.lhs->kind == xdec::emit::HlExprKind::Var &&
      isObjcMsgSendName(expr.lhs->text) && expr.args.size() >= 2) {
    if (expr.args[0]) {
      transformObjcExpr(*expr.args[0], std::nullopt, maps, false);
    }
    std::string selector = pendingSelector.value_or("");
    const std::string rawSelArg =
        (expr.args[1] && expr.args[1]->kind == xdec::emit::HlExprKind::Raw)
            ? expr.args[1]->text
            : std::string{};
    if (!selector.empty() && !rawSelArg.empty()) {
      // If args[1] was a temp variable (e.g. `t2`), learn its exact selector.
      if (maps.selTempToSelector.contains(rawSelArg)) {
        maps.selTempToSelector[rawSelArg] = selector;
      }
    }
    if (selector.empty() && !rawSelArg.empty()) {
      if (const auto it = maps.selTempToSelector.find(rawSelArg);
          it != maps.selTempToSelector.end()) {
        selector = it->second;
      } else if (const auto parsed = parseSelRefDeref(rawSelArg)) {
        selector = *parsed;
      }
    }
    for (std::size_t i = 2; i < expr.args.size(); ++i) {
      if (expr.args[i]) {
        transformObjcExpr(*expr.args[i], std::nullopt, maps, true);
      }
    }
    if (!selector.empty()) {
      xdec::emit::HlExprPtr recv = std::move(expr.args[0]);
      if (recv->kind == xdec::emit::HlExprKind::Raw) {
        if (const std::string cls = cleanObjcClassRefName(recv->text); !cls.empty()) {
          recv->text = cls;
        }
      }
      std::vector<xdec::emit::HlExprPtr> msgArgs;
      msgArgs.reserve(expr.args.size() - 2);
      for (std::size_t i = 2; i < expr.args.size(); ++i) {
        msgArgs.push_back(std::move(expr.args[i]));
      }
      expr.kind = xdec::emit::HlExprKind::ObjcMsg;
      expr.text = std::move(selector);
      expr.lhs = std::move(recv);
      expr.args = std::move(msgArgs);
      if (expr.isTailCall && expr.callNote.empty()) {
        expr.callNote = " /* tailcall */";
      }
      return;
    }
  }

  if (expr.lhs) {
    transformObjcExpr(*expr.lhs, std::nullopt, maps, false);
  }
  if (expr.rhs) {
    transformObjcExpr(*expr.rhs, std::nullopt, maps, false);
  }
  for (auto& a : expr.args) {
    if (a) {
      transformObjcExpr(*a, std::nullopt, maps, true);
    }
  }
  if (expr.kind == xdec::emit::HlExprKind::Raw) {
    if (const std::string cleaned = cleanObjcClassRefName(expr.text); !cleaned.empty()) {
      expr.text = cleaned;
    } else {
      expr.text = rewriteObjcRawText(std::move(expr.text), maps, isSelectorArg);
    }
  }
}

void transformObjcStmt(xdec::emit::HlStmt& stmt, ObjcScopeMaps maps) {
  if (stmt.kind == xdec::emit::HlStmtKind::Sequence ||
      stmt.kind == xdec::emit::HlStmtKind::Scope) {
    std::optional<std::string> pendingSelector;

    for (std::size_t i = 0; i < stmt.items.size(); ++i) {
      auto& item = stmt.items[i];
      if (!item) {
        continue;
      }
      if (item->kind == xdec::emit::HlStmtKind::Comment) {
        if (auto sel = parseObjcSelectorComment(item->text)) {
          pendingSelector = std::move(sel);
          item.reset();  // consumed by the following ObjcMsg
          continue;
        }
      }
      if (item->kind == xdec::emit::HlStmtKind::Assign && item->lhs &&
          item->lhs->kind == xdec::emit::HlExprKind::Var && item->rhs &&
          item->rhs->kind == xdec::emit::HlExprKind::Raw) {
        const std::string& dst = item->lhs->text;
        const std::string& rhsText = item->rhs->text;
        if (const std::string cls = cleanObjcClassRefName(rhsText); !cls.empty()) {
          maps.clsTempToName[dst] = cls;
        }
        static constexpr std::string_view kIvarLoad = "(*(uint32_t*)(ivar_";
        if (rhsText.starts_with(kIvarLoad) && rhsText.ends_with("))")) {
          const std::size_t symStart = rhsText.find("ivar_");
          const std::string_view ivarSym =
              std::string_view{rhsText}.substr(symStart, rhsText.size() - 2 - symStart);
          if (const auto field = extractIvarField(ivarSym)) {
            maps.ivarTempToField[dst] = *field;
          }
        }
        if (const auto sel = parseSelRefDeref(rhsText)) {
          maps.selTempToSelector[dst] = *sel;
        }
        static constexpr std::string_view kSextPrefix = "((int64_t)(int32_t)(";
        if (rhsText.starts_with(kSextPrefix) && rhsText.ends_with("))")) {
          const std::string innerVar{
              rhsText.substr(kSextPrefix.size(), rhsText.size() - kSextPrefix.size() - 2)};
          if (const auto it = maps.ivarTempToField.find(innerVar);
              it != maps.ivarTempToField.end()) {
            maps.ivarTempToField[dst] = it->second;
          }
        }
      }

      if (item->cond) {
        transformObjcExpr(*item->cond, std::nullopt, maps, false);
      }
      if (item->lhs) {
        transformObjcExpr(*item->lhs,
                          item->kind == xdec::emit::HlStmtKind::ExprStmt ? pendingSelector
                                                                         : std::nullopt,
                          maps, false);
      }
      if (item->rhs) {
        transformObjcExpr(*item->rhs,
                          item->kind == xdec::emit::HlStmtKind::Assign ? pendingSelector
                                                                       : std::nullopt,
                          maps, item->kind == xdec::emit::HlStmtKind::Assign);
      }
      if (item->kind != xdec::emit::HlStmtKind::Comment) {
        pendingSelector.reset();
      }
      transformObjcStmt(*item, maps);
    }
    stmt.items.erase(
        std::remove_if(stmt.items.begin(), stmt.items.end(),
                       [](const xdec::emit::HlStmtPtr& p) { return p == nullptr; }),
        stmt.items.end());
    return;
  }

  if (stmt.thenArm) {
    transformObjcStmt(*stmt.thenArm, maps);
  }
  if (stmt.elseArm) {
    transformObjcStmt(*stmt.elseArm, maps);
  }
  for (auto& c : stmt.cases) {
    if (c.body) {
      transformObjcStmt(*c.body, maps);
    }
  }
}

void collectIdentUsesInExpr(const xdec::emit::HlExpr& expr,
                            std::unordered_set<std::string>& used) {
  if (expr.kind == xdec::emit::HlExprKind::Var) {
    used.insert(expr.text);
  } else if (expr.kind == xdec::emit::HlExprKind::Raw) {
    const std::string_view s = expr.text;
    std::size_t i = 0;
    while (i < s.size()) {
      const auto uc = static_cast<unsigned char>(s[i]);
      if (std::isalpha(uc) != 0 || s[i] == '_') {
        const std::size_t start = i;
        while (i < s.size() &&
               (std::isalnum(static_cast<unsigned char>(s[i])) != 0 || s[i] == '_' ||
                s[i] == '$')) {
          ++i;
        }
        used.emplace(s.substr(start, i - start));
      } else {
        ++i;
      }
    }
  }
  if (expr.lhs) {
    collectIdentUsesInExpr(*expr.lhs, used);
  }
  if (expr.rhs) {
    collectIdentUsesInExpr(*expr.rhs, used);
  }
  for (const auto& a : expr.args) {
    if (a) {
      collectIdentUsesInExpr(*a, used);
    }
  }
}

void collectIdentUsesInStmt(const xdec::emit::HlStmt& stmt,
                            std::unordered_set<std::string>& used) {
  if (stmt.cond) {
    collectIdentUsesInExpr(*stmt.cond, used);
  }
  if (stmt.kind == xdec::emit::HlStmtKind::Assign) {
    if (stmt.lhs && stmt.lhs->kind != xdec::emit::HlExprKind::Var) {
      collectIdentUsesInExpr(*stmt.lhs, used);
    }
    if (stmt.rhs) {
      collectIdentUsesInExpr(*stmt.rhs, used);
    }
  } else {
    if (stmt.lhs) {
      collectIdentUsesInExpr(*stmt.lhs, used);
    }
    if (stmt.rhs) {
      collectIdentUsesInExpr(*stmt.rhs, used);
    }
  }
  for (const auto& item : stmt.items) {
    if (item) {
      collectIdentUsesInStmt(*item, used);
    }
  }
  if (stmt.thenArm) {
    collectIdentUsesInStmt(*stmt.thenArm, used);
  }
  if (stmt.elseArm) {
    collectIdentUsesInStmt(*stmt.elseArm, used);
  }
  for (const auto& c : stmt.cases) {
    if (c.body) {
      collectIdentUsesInStmt(*c.body, used);
    }
  }
}

[[nodiscard]] bool isTempVarName(std::string_view name) noexcept {
  if (name.size() >= 2 && name[0] == 't' &&
      std::all_of(name.begin() + 1, name.end(),
                  [](unsigned char c) { return std::isdigit(c) != 0; })) {
    return true;
  }
  if (name.size() >= 5 && name.starts_with("_cse") &&
      std::all_of(name.begin() + 4, name.end(),
                  [](unsigned char c) { return std::isdigit(c) != 0; })) {
    return true;
  }
  return false;
}

[[nodiscard]] bool isPureObjcMetaAssign(const xdec::emit::HlStmt& stmt) {
  if (stmt.kind != xdec::emit::HlStmtKind::Assign || !stmt.lhs ||
      stmt.lhs->kind != xdec::emit::HlExprKind::Var || !stmt.rhs) {
    return false;
  }
  if (!isTempVarName(stmt.lhs->text)) {
    return false;
  }
  if (stmt.rhs->kind == xdec::emit::HlExprKind::Var ||
      stmt.rhs->kind == xdec::emit::HlExprKind::Const ||
      stmt.rhs->kind == xdec::emit::HlExprKind::StringLit ||
      stmt.rhs->kind == xdec::emit::HlExprKind::CFStringLit) {
    return true;
  }
  if (stmt.rhs->kind != xdec::emit::HlExprKind::Raw) {
    return false;
  }
  const std::string_view r = stmt.rhs->text;
  if (r.starts_with("@selector(") || r.starts_with("@protocol(") ||
      r.starts_with("@\"") || r.starts_with("(*(uint64_t*)(selRef_") ||
      r.starts_with("(*(uint64_t*)(clsRef_") || r.starts_with("(*(uint32_t*)(ivar_") ||
      r.starts_with("(*(uint64_t*)(protoRef_") ||
      r.starts_with("(*(uint64_t*)(ptr__OBJC_CLASS_$_")) {
    return true;
  }
  // Pure cast of an identifier (e.g. `((int64_t)(int32_t)(t36))`).
  if (r.starts_with("((") && r.ends_with("))")) {
    const std::size_t open = r.rfind('(');
    if (open != std::string_view::npos && open > 0 && r[open - 1] == ')') {
      const std::string_view inner = r.substr(open + 1, r.size() - open - 3);
      if (!inner.empty() && std::all_of(inner.begin(), inner.end(), [](unsigned char c) {
            return std::isalnum(c) != 0 || c == '_';
          })) {
        return true;
      }
    }
  }
  // Pure identifier (e.g. `iConsole`, `NSData`, `t45`), no dereference or call.
  return !r.empty() && std::all_of(r.begin(), r.end(), [](unsigned char c) {
           return std::isalnum(c) != 0 || c == '_';
         });
}

[[nodiscard]] bool isEmptyStmtTree(const xdec::emit::HlStmt& stmt) {
  if (stmt.kind == xdec::emit::HlStmtKind::Sequence ||
      stmt.kind == xdec::emit::HlStmtKind::Scope) {
    return std::all_of(stmt.items.begin(), stmt.items.end(),
                       [](const xdec::emit::HlStmtPtr& p) {
                         return p == nullptr || isEmptyStmtTree(*p);
                       });
  }
  return false;
}

bool removeUnusedMetaAssigns(xdec::emit::HlStmt& stmt,
                             const std::unordered_set<std::string>& used) {
  bool removed = false;
  if (stmt.kind == xdec::emit::HlStmtKind::Sequence ||
      stmt.kind == xdec::emit::HlStmtKind::Scope) {
    for (auto& item : stmt.items) {
      if (!item) {
        continue;
      }
      if (isPureObjcMetaAssign(*item) && !used.contains(item->lhs->text)) {
        item.reset();
        removed = true;
        continue;
      }
      if (item->kind == xdec::emit::HlStmtKind::MultiAssign && item->rhs &&
          item->rhs->kind == xdec::emit::HlExprKind::Const) {
        const std::size_t before = item->names.size();
        item->names.erase(
            std::remove_if(item->names.begin(), item->names.end(),
                           [&used](const std::string& n) {
                             return isTempVarName(n) && !used.contains(n);
                           }),
            item->names.end());
        if (item->names.size() != before) {
          removed = true;
        }
        if (item->names.empty()) {
          item.reset();
          removed = true;
          continue;
        }
      }
      removed |= removeUnusedMetaAssigns(*item, used);
    }
    stmt.items.erase(
        std::remove_if(stmt.items.begin(), stmt.items.end(),
                       [](const xdec::emit::HlStmtPtr& p) { return p == nullptr; }),
        stmt.items.end());
  }
  if (stmt.thenArm) {
    removed |= removeUnusedMetaAssigns(*stmt.thenArm, used);
  }
  if (stmt.elseArm) {
    removed |= removeUnusedMetaAssigns(*stmt.elseArm, used);
    if (isEmptyStmtTree(*stmt.elseArm)) {
      stmt.elseArm.reset();
      removed = true;
    }
  }
  for (auto& c : stmt.cases) {
    if (c.body) {
      removed |= removeUnusedMetaAssigns(*c.body, used);
    }
  }
  return removed;
}

void objcHlAstTransform(const xdec::il::Function& /*function*/, xdec::emit::HlFunction& hl) {
  if (hl.language != xdec::emit::EmitLanguage::Objc || !hl.body) {
    return;
  }
  transformObjcStmt(*hl.body, ObjcScopeMaps{});
  for (bool changed = true; changed;) {
    std::unordered_set<std::string> used;
    collectIdentUsesInStmt(*hl.body, used);
    changed = removeUnusedMetaAssigns(*hl.body, used);
  }
}

}  // namespace

extern "C" XDEC_EXPORT std::uint32_t xdec_plugin_abi_version() {
  return XDEC_PLUGIN_ABI_VERSION;
}

extern "C" XDEC_EXPORT XdecPluginInitResult
xdec_plugin_init(const XdecPluginHost* host) {
  if (host == nullptr || host->passes == nullptr) {
    return {false, "null host or registry"};
  }
  if (!host->passes->add(std::make_unique<ObjcArityTrimPass>())) {
    return {false, "failed to register objc-arity-trim"};
  }
  if (!host->passes->add(std::make_unique<ObjcArcElidePass>())) {
    return {false, "failed to register objc-retain-elide"};
  }
  if (host->hlTransforms != nullptr) {
    host->hlTransforms->push_back(objcHlAstTransform);
  }
  return {true, nullptr};
}
