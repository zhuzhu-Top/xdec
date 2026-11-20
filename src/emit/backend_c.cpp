#include "backend_c.h"

#include <format>
#include <string>
#include <string_view>

#include "c_context.h"
#include "xdec/analysis/image_literals.h"

namespace xdec::emit {

namespace {

class CBackend {
 public:
  explicit CBackend(unsigned baseIndent) : indent_(baseIndent) {}

  [[nodiscard]] std::string render(const HlStmt& root) {
    std::string out;
    emitStmt(root, out);
    return out;
  }

 private:
  void line(std::string& out, std::string_view text) const {
    appendLine(out, indent_, text);
  }

  [[nodiscard]] std::string renderExpr(const HlExpr& expr,
                                       bool parenthesizeBinary = false) const {
    switch (expr.kind) {
      case HlExprKind::Raw:
      case HlExprKind::Var:
      case HlExprKind::FloatLit:
        return expr.text;
      case HlExprKind::Const:
        return std::format("0x{:x}", expr.constant);
      case HlExprKind::StringLit:
        return analysis::quoteCString(expr.text);
      case HlExprKind::CFStringLit:
        return std::format("{} /* @{} */", expr.extra, analysis::quoteCString(expr.text));
      case HlExprKind::Unary:
        return std::format("{}{}", expr.text, renderExpr(*expr.lhs, true));
      case HlExprKind::Binary: {
        const std::string inner = std::format("{} {} {}", renderExpr(*expr.lhs, true),
                                              expr.text, renderExpr(*expr.rhs, true));
        return parenthesizeBinary ? std::format("({})", inner) : inner;
      }
      case HlExprKind::Cast:
        return std::format("({})({})", expr.text, renderExpr(*expr.lhs, true));
      case HlExprKind::Deref:
        if (!expr.extra.empty()) {
          return std::format("(*({}*)({}))", expr.extra, renderExpr(*expr.lhs, true));
        }
        return std::format("(*{})", renderExpr(*expr.lhs, true));
      case HlExprKind::AddressOf:
        return std::format("&{}", renderExpr(*expr.lhs, true));
      case HlExprKind::Select: {
        const std::string inner =
            std::format("{} ? {} : {}", renderExpr(*expr.cond, true),
                        renderExpr(*expr.lhs, true), renderExpr(*expr.rhs, true));
        return parenthesizeBinary ? std::format("({})", inner) : inner;
      }
      case HlExprKind::Call: {
        std::string args;
        for (std::size_t i = 0; i < expr.args.size(); ++i) {
          args += i == 0 ? "" : ", ";
          args += renderExpr(*expr.args[i]);
        }
        if (expr.isDirectCall) {
          return std::format("{}({}){}", renderExpr(*expr.lhs, true), args, expr.callSuffix);
        }
        return std::format("(({}){})({}){}", expr.extra, renderExpr(*expr.lhs, true), args,
                           expr.callSuffix);
      }
      case HlExprKind::ObjcMsg: {
        std::string args;
        for (std::size_t i = 0; i < expr.args.size(); ++i) {
          args += i == 0 ? "" : ", ";
          args += renderExpr(*expr.args[i]);
        }
        return std::format("_objc_msgSend({}, {}{}{})", renderExpr(*expr.lhs), expr.text,
                           args.empty() ? "" : ", ", args);
      }
    }
    return {};
  }

  [[nodiscard]] std::string callTrailingNote(const HlExpr& expr) const {
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

}  // namespace

std::string renderCBody(const HlStmt& root, unsigned baseIndent) {
  return CBackend(baseIndent).render(root);
}

}  // namespace xdec::emit
