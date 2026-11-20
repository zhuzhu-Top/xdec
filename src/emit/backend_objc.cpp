#include "backend_objc.h"

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "c_context.h"
#include "xdec/analysis/image_literals.h"

namespace xdec::emit {

namespace {

[[nodiscard]] std::vector<std::string_view> splitColons(std::string_view selector) {
  std::vector<std::string_view> parts;
  std::size_t pos = 0;
  while (pos < selector.size()) {
    const std::size_t colon = selector.find(':', pos);
    if (colon == std::string_view::npos) {
      parts.push_back(selector.substr(pos));
      break;
    }
    parts.push_back(selector.substr(pos, colon - pos));
    pos = colon + 1;
  }
  return parts;
}

class ObjcBackend {
 public:
  explicit ObjcBackend(unsigned baseIndent) : indent_(baseIndent) {}

  [[nodiscard]] std::string render(const HlStmt& root) {
    std::string out;
    emitStmt(root, out);
    return out;
  }

 private:
  void line(std::string& out, std::string_view text) const {
    appendLine(out, indent_, text);
  }

  [[nodiscard]] std::string renderExpr(const HlExpr& expr) const {
    switch (expr.kind) {
      case HlExprKind::Raw:
      case HlExprKind::Var:
        return expr.text;
      case HlExprKind::Const:
        return std::format("0x{:x}", expr.constant);
      case HlExprKind::StringLit:
        return analysis::quoteCString(expr.text);
      case HlExprKind::CFStringLit:
        return std::format("@{}", analysis::quoteCString(expr.text));
      case HlExprKind::Unary:
        return std::format("{}{}", expr.text, renderExpr(*expr.lhs));
      case HlExprKind::Binary:
        return std::format("{} {} {}", renderExpr(*expr.lhs), expr.text,
                           renderExpr(*expr.rhs));
      case HlExprKind::Cast:
        return std::format("({})({})", expr.text, renderExpr(*expr.lhs));
      case HlExprKind::Deref:
        return std::format("(*{})", renderExpr(*expr.lhs));
      case HlExprKind::AddressOf:
        return std::format("&{}", renderExpr(*expr.lhs));
      case HlExprKind::ObjcMsg: {
        const std::string recv = renderExpr(*expr.lhs);
        const std::vector<std::string_view> parts = splitColons(expr.text);
        if (expr.args.empty() || expr.text.find(':') == std::string::npos) {
          return std::format("[{} {}]", recv, expr.text);
        }
        std::string msg = std::format("[{}", recv);
        const std::size_t named = std::min(parts.size(), expr.args.size());
        for (std::size_t i = 0; i < named; ++i) {
          msg += std::format(" {}:", parts[i]);
          msg += renderExpr(*expr.args[i]);
        }
        for (std::size_t i = named; i < expr.args.size(); ++i) {
          msg += std::format(", {}", renderExpr(*expr.args[i]));
        }
        msg += ']';
        return msg;
      }
      case HlExprKind::Call: {
        std::string args;
        for (std::size_t i = 0; i < expr.args.size(); ++i) {
          args += i == 0 ? "" : ", ";
          args += renderExpr(*expr.args[i]);
        }
        if (expr.isDirectCall) {
          return std::format("{}({}){}", renderExpr(*expr.lhs), args, expr.callSuffix);
        }
        return std::format("(({}){})({}){}", expr.extra, renderExpr(*expr.lhs), args,
                           expr.callSuffix);
      }
    }
    return {};
  }

  [[nodiscard]] std::string callTrailingNote(const HlExpr& expr) const {
    if (expr.kind == HlExprKind::ObjcMsg) {
      return expr.callNote;
    }
    if (expr.kind != HlExprKind::Call) {
      return {};
    }
    if (expr.isDirectCall) {
      return expr.isNoreturn ? " /* does not return */" : "";
    }
    return expr.callNote;
  }

  void emitStmt(const HlStmt& stmt, std::string& out) {
    switch (stmt.kind) {
      case HlStmtKind::Sequence:
        for (const HlStmtPtr& item : stmt.items) {
          if (item) {
            emitStmt(*item, out);
          }
        }
        break;
      case HlStmtKind::Scope:
        line(out, "{");
        ++indent_;
        for (const HlStmtPtr& item : stmt.items) {
          if (item) {
            emitStmt(*item, out);
          }
        }
        --indent_;
        line(out, "}");
        break;
      case HlStmtKind::Label:
        out += std::format("{}:\n", stmt.text);
        break;
      case HlStmtKind::Comment:
        if (stmt.lineComment) {
          line(out, std::format("// {}", stmt.text));
        } else {
          line(out, std::format("/* {} */", stmt.text));
        }
        break;
      case HlStmtKind::VarDecl:
        line(out, std::format("{}{} {} = {};", stmt.isConst ? "const " : "",
                              stmt.typeText, stmt.text, renderExpr(*stmt.lhs)));
        break;
      case HlStmtKind::Assign: {
        const std::string note = callTrailingNote(*stmt.rhs);
        const std::string trail =
            stmt.text.empty() ? note : std::format(" /* {} */", stmt.text);
        line(out, std::format("{} = {};{}", renderExpr(*stmt.lhs),
                              renderExpr(*stmt.rhs), trail));
        break;
      }
      case HlStmtKind::MultiAssign: {
        std::string assignees;
        for (const std::string& n : stmt.names) {
          assignees += n + " = ";
        }
        const std::string trail =
            stmt.text.empty() ? "" : std::format(" /* {} */", stmt.text);
        line(out, std::format("{}{};{}", assignees, renderExpr(*stmt.rhs), trail));
        break;
      }
      case HlStmtKind::ExprStmt: {
        const std::string note = callTrailingNote(*stmt.lhs);
        line(out, std::format("{};{}", renderExpr(*stmt.lhs), note));
        break;
      }
      case HlStmtKind::If:
        emitIfChain(stmt, out, /*isElseIf=*/false);
        break;
      case HlStmtKind::While:
        if (!stmt.cond) {
          line(out, "while (true) {");
        } else {
          line(out, std::format("while ({}) {{", renderExpr(*stmt.cond)));
        }
        ++indent_;
        if (stmt.thenArm) {
          emitStmt(*stmt.thenArm, out);
        }
        --indent_;
        line(out, "}");
        break;
      case HlStmtKind::DoWhile:
        line(out, "do {");
        ++indent_;
        if (stmt.thenArm) {
          emitStmt(*stmt.thenArm, out);
        }
        --indent_;
        line(out, std::format("}} while ({});", renderExpr(*stmt.cond)));
        break;
      case HlStmtKind::Switch:
        line(out, std::format("switch ({}) {{", renderExpr(*stmt.cond)));
        ++indent_;
        for (const HlSwitchCase& c : stmt.cases) {
          if (c.isDefault) {
            line(out, "default:");
          } else if (c.comment.empty()) {
            line(out, std::format("case 0x{:x}:", c.value));
          } else {
            line(out, std::format("case 0x{:x}: /* {} */", c.value, c.comment));
          }
          ++indent_;
          if (c.body) {
            emitStmt(*c.body, out);
          }
          --indent_;
        }
        --indent_;
        line(out, "}");
        if (stmt.elseArm) {
          emitStmt(*stmt.elseArm, out);
        }
        break;
      case HlStmtKind::Return:
        if (!stmt.lhs) {
          line(out, "return;");
        } else {
          line(out, std::format("return {};", renderExpr(*stmt.lhs)));
        }
        break;
      case HlStmtKind::Goto:
        line(out, std::format("goto {};", stmt.text));
        break;
      case HlStmtKind::Break:
        line(out, "break;");
        break;
      case HlStmtKind::Continue:
        line(out, "continue;");
        break;
      case HlStmtKind::Unimplemented:
        line(out, std::format("__xdec_unimplemented(\"{}\");", stmt.text));
        break;
      case HlStmtKind::Unreachable:
        line(out, "__builtin_unreachable();");
        break;
    }
  }

  void emitIfChain(const HlStmt& stmt, std::string& out, bool isElseIf) {
    line(out, std::format("{}if ({}) {{", isElseIf ? "} else " : "", renderExpr(*stmt.cond)));
    ++indent_;
    if (stmt.thenArm) {
      emitStmt(*stmt.thenArm, out);
    }
    --indent_;
    if (stmt.elseArm) {
      if (stmt.elseArm->kind == HlStmtKind::If) {
        emitIfChain(*stmt.elseArm, out, /*isElseIf=*/true);
        return;
      }
      line(out, "} else {");
      ++indent_;
      emitStmt(*stmt.elseArm, out);
      --indent_;
    }
    line(out, "}");
  }

  unsigned indent_ = 1;
};

[[nodiscard]] std::vector<std::string> parseObjcEncodeTypes(std::string_view enc) {
  std::vector<std::string> types;
  std::size_t i = 0;
  auto skipDigits = [&]() {
    while (i < enc.size() && enc[i] >= '0' && enc[i] <= '9') {
      ++i;
    }
  };
  auto skipBalanced = [&](char open, char close) {
    int depth = 0;
    while (i < enc.size()) {
      if (enc[i] == open) {
        ++depth;
      } else if (enc[i] == close && --depth <= 0) {
        ++i;
        break;
      }
      ++i;
    }
  };
  auto parseOne = [&](auto& self) -> std::string {
    while (i < enc.size() && (enc[i] == 'r' || enc[i] == 'n' || enc[i] == 'N' ||
                              enc[i] == 'o' || enc[i] == 'O' || enc[i] == 'R' ||
                              enc[i] == 'V')) {
      ++i;
    }
    if (i >= enc.size()) {
      return {};
    }
    const char ch = enc[i++];
    switch (ch) {
      case 'v': return "void";
      case '@':
        if (i < enc.size() && enc[i] == '?') {
          ++i;
          return "id";
        }
        if (i < enc.size() && enc[i] == '"') {
          const std::size_t close = enc.find('"', i + 1);
          if (close != std::string_view::npos) {
            const std::string_view cls = enc.substr(i + 1, close - i - 1);
            i = close + 1;
            if (!cls.empty() && cls.find('<') == std::string_view::npos) {
              return std::format("{}*", cls);
            }
          }
        }
        return "id";
      case '#': return "Class";
      case ':': return "SEL";
      case 'B': return "BOOL";
      case 'c': return "int8_t";
      case 'C': return "uint8_t";
      case 's': return "int16_t";
      case 'S': return "uint16_t";
      case 'i': return "int32_t";
      case 'I': return "uint32_t";
      case 'l':
      case 'q': return "int64_t";
      case 'L':
      case 'Q': return "uint64_t";
      case 'f': return "float";
      case 'd': return "double";
      case '*': return "char*";
      case '^': {
        std::string inner = self(self);
        if (inner.empty() || inner == "void") {
          return "void*";
        }
        return inner + "*";
      }
      case '{':
        --i;
        skipBalanced('{', '}');
        return "struct_t";
      case '[':
        --i;
        skipBalanced('[', ']');
        return "void*";
      case '(':
        --i;
        skipBalanced('(', ')');
        return "union_t";
      default:
        return "uint64_t";
    }
  };
  while (i < enc.size()) {
    std::string t = parseOne(parseOne);
    if (t.empty()) {
      break;
    }
    skipDigits();
    types.push_back(std::move(t));
  }
  return types;
}

}  // namespace

std::string renderObjcBody(const HlStmt& root, unsigned baseIndent) {
  return ObjcBackend(baseIndent).render(root);
}

std::string formatObjcMethodSignature(
    std::string_view displayName, std::string_view typeEncoding, std::string_view returnType,
    const std::vector<std::pair<std::string, std::string>>& params) {
  if (displayName.size() < 5 ||
      (displayName[0] != '-' && displayName[0] != '+') ||
      displayName[1] != '[') {
    return {};
  }
  const char kindChar = displayName[0];
  const std::size_t space = displayName.find(' ');
  const std::size_t close = displayName.rfind(']');
  if (space == std::string_view::npos || close == std::string_view::npos ||
      close <= space + 1) {
    return {};
  }
  const std::string_view className = displayName.substr(2, space - 2);
  const std::string_view selector = displayName.substr(space + 1, close - space - 1);
  const std::vector<std::string> encTypes = parseObjcEncodeTypes(typeEncoding);
  const std::string ret =
      !encTypes.empty()
          ? encTypes[0]
          : (returnType == "uint64_t" ? std::string{"id"} : std::string{returnType});

  // Skip implicit `self`/`cls` (index 0) and `_cmd` (index 1) in method signature.
  const std::vector<std::string_view> parts = splitColons(selector);
  if (selector.find(':') == std::string_view::npos) {
    return std::format("// @implementation {}\n{} ({}){}", className, kindChar, ret, selector);
  }
  std::string out = std::format("// @implementation {}\n{} ({})", className, kindChar, ret);
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const std::size_t paramIdx = i + 2;
    const std::size_t encIdx = i + 3;  // encTypes: [0]=ret, [1]=self, [2]=_cmd, [3+]=args
    std::string pType = "id";
    std::string pName = std::format("arg{}", paramIdx + 1);
    if (encIdx < encTypes.size() && !encTypes[encIdx].empty()) {
      pType = encTypes[encIdx];
    } else if (paramIdx < params.size() && !params[paramIdx].first.empty()) {
      pType = params[paramIdx].first == "uint64_t" ? std::string{"id"}
                                                   : params[paramIdx].first;
    }
    if (paramIdx < params.size() && !params[paramIdx].second.empty()) {
      pName = params[paramIdx].second;
    }
    out += std::format("{}{}:({}){}", i == 0 ? "" : " ", parts[i], pType, pName);
  }
  return out;
}

}  // namespace xdec::emit
