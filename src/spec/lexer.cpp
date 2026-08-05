#include "lexer.h"

#include <format>

namespace xdec::spec {
namespace {

[[nodiscard]] bool isIdentStart(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

[[nodiscard]] bool isIdentChar(char c) noexcept {
  return isIdentStart(c) || (c >= '0' && c <= '9');
}

[[nodiscard]] bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] bool isHexDigit(char c) noexcept {
  return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

[[nodiscard]] unsigned hexValue(char c) noexcept {
  if (c <= '9') {
    return static_cast<unsigned>(c - '0');
  }
  return static_cast<unsigned>((c | 0x20) - 'a') + 10;
}

class Lexer {
 public:
  Lexer(std::string_view text, std::string_view sourceName)
      : text_(text), sourceName_(sourceName) {}

  Result<std::vector<Token>> run() {
    while (true) {
      skipTrivia();
      if (position_ >= text_.size()) {
        break;
      }
      XDEC_TRY_VOID(scanToken());
    }
    tokens_.push_back(Token{TokenKind::End, here(), {}, 0, 0});
    return std::move(tokens_);
  }

 private:
  [[nodiscard]] SourceLoc here() const noexcept { return SourceLoc{line_, column_}; }

  [[nodiscard]] Unexpected fail(SourceLoc loc, std::string message) const {
    return err(Diag{DiagCode::ParseError, std::move(message)}.note(
        std::format("{}:{}", sourceName_, loc.toString())));
  }

  [[nodiscard]] char peek(std::size_t ahead = 0) const noexcept {
    const std::size_t index = position_ + ahead;
    return index < text_.size() ? text_[index] : '\0';
  }

  void bump() noexcept {
    if (position_ >= text_.size()) {
      return;
    }
    if (text_[position_] == '\n') {
      ++line_;
      column_ = 1;
    } else {
      ++column_;
    }
    ++position_;
  }

  void bump(std::size_t count) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
      bump();
    }
  }

  void skipTrivia() noexcept {
    while (position_ < text_.size()) {
      const char c = peek();
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        bump();
      } else if (c == '/' && peek(1) == '/') {
        while (position_ < text_.size() && peek() != '\n') {
          bump();
        }
      } else if (c == '/' && peek(1) == '*') {
        bump(2);
        while (position_ < text_.size() && !(peek() == '*' && peek(1) == '/')) {
          bump();
        }
        bump(2);
      } else {
        return;
      }
    }
  }

  void push(TokenKind kind, SourceLoc loc, std::size_t length) {
    bump(length);
    tokens_.push_back(Token{kind, loc, {}, 0, 0});
  }

  Result<void> scanToken() {
    const SourceLoc loc = here();
    const char c = peek();

    if (isIdentStart(c)) {
      const std::size_t start = position_;
      while (position_ < text_.size() && isIdentChar(peek())) {
        bump();
      }
      Token token{TokenKind::Identifier, loc, {}, 0, 0};
      token.text = std::string{text_.substr(start, position_ - start)};
      tokens_.push_back(std::move(token));
      return ok();
    }

    if (isDigit(c)) {
      return scanNumber();
    }

    if (c == '"') {
      return scanString();
    }

    // Multi-character operators first, longest match.
    struct Operator {
      std::string_view text;
      TokenKind kind;
    };
    static constexpr Operator kOperators[] = {
        {"<=u", TokenKind::LessEqualU}, {">=u", TokenKind::GreaterEqualU},
        {">>>", TokenKind::ShrS},       {"<u", TokenKind::LessU},
        {">u", TokenKind::GreaterU},    {"==", TokenKind::Equal},
        {"!=", TokenKind::NotEqual},    {"<=", TokenKind::LessEqual},
        {">=", TokenKind::GreaterEqual},{"<<", TokenKind::Shl},
        {">>", TokenKind::Shr},         {"&&", TokenKind::AndAnd},
        {"||", TokenKind::OrOr},        {"->", TokenKind::Arrow},
        {"..", TokenKind::DotDot},      {"/s", TokenKind::SlashS},
        {"%s", TokenKind::PercentS},    {"{", TokenKind::LBrace},
        {"}", TokenKind::RBrace},       {"(", TokenKind::LParen},
        {")", TokenKind::RParen},       {"[", TokenKind::LBracket},
        {"]", TokenKind::RBracket},     {",", TokenKind::Comma},
        {";", TokenKind::Semicolon},    {":", TokenKind::Colon},
        {".", TokenKind::Dot},          {"?", TokenKind::Question},
        {"=", TokenKind::Assign},       {"<", TokenKind::Less},
        {">", TokenKind::Greater},      {"+", TokenKind::Plus},
        {"-", TokenKind::Minus},        {"*", TokenKind::Star},
        {"/", TokenKind::Slash},        {"%", TokenKind::Percent},
        {"&", TokenKind::Ampersand},    {"|", TokenKind::Pipe},
        {"^", TokenKind::Caret},        {"~", TokenKind::Tilde},
        {"!", TokenKind::Bang},
    };

    for (const Operator& op : kOperators) {
      if (text_.compare(position_, op.text.size(), op.text) == 0) {
        push(op.kind, loc, op.text.size());
        return ok();
      }
    }

    return fail(loc, std::format("unexpected character '{}'", c));
  }

  Result<void> scanNumber() {
    const SourceLoc loc = here();
    uint64_t value = 0;
    unsigned digits = 0;

    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
      bump(2);
      if (!isHexDigit(peek())) {
        return fail(loc, "hexadecimal literal has no digits");
      }
      while (isHexDigit(peek()) || peek() == '_') {
        if (peek() != '_') {
          value = value * 16 + hexValue(peek());
          ++digits;
        }
        bump();
      }
      digits *= 4;
    } else if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
      bump(2);
      if (peek() != '0' && peek() != '1') {
        return fail(loc, "binary literal has no digits");
      }
      while (peek() == '0' || peek() == '1' || peek() == '_') {
        if (peek() != '_') {
          value = value * 2 + static_cast<uint64_t>(peek() - '0');
          ++digits;
        }
        bump();
      }
    } else {
      while (isDigit(peek()) || peek() == '_') {
        if (peek() != '_') {
          value = value * 10 + static_cast<uint64_t>(peek() - '0');
          ++digits;
        }
        bump();
      }
    }

    // A trailing identifier character means something like `12abc`, which is
    // almost certainly a typo rather than an intentional juxtaposition.
    if (isIdentChar(peek())) {
      return fail(here(), "unexpected character after a numeric literal");
    }

    Token token{TokenKind::Integer, loc, {}, value, digits};
    tokens_.push_back(std::move(token));
    return ok();
  }

  Result<void> scanString() {
    const SourceLoc loc = here();
    bump();  // opening quote
    std::string contents;
    while (true) {
      if (position_ >= text_.size() || peek() == '\n') {
        return fail(loc, "unterminated string");
      }
      const char c = peek();
      if (c == '"') {
        bump();
        break;
      }
      if (c == '\\') {
        bump();
        switch (peek()) {
          case 'n':
            contents.push_back('\n');
            break;
          case 't':
            contents.push_back('\t');
            break;
          case '\\':
          case '"':
            contents.push_back(peek());
            break;
          case '{':
          case '}':
          case '[':
          case ']':
            // Braces and brackets are the asm template's own syntax, so the
            // backslash is left in place for the template parser to consume. A
            // plain string has no use for them either way.
            contents.push_back('\\');
            contents.push_back(peek());
            break;
          default:
            return fail(here(), std::format("unknown escape '\\{}'", peek()));
        }
        bump();
        continue;
      }
      contents.push_back(c);
      bump();
    }

    Token token{TokenKind::String, loc, {}, 0, 0};
    token.text = std::move(contents);
    tokens_.push_back(std::move(token));
    return ok();
  }

  std::string_view text_;
  std::string_view sourceName_;
  std::size_t position_ = 0;
  uint32_t line_ = 1;
  uint32_t column_ = 1;
  std::vector<Token> tokens_;
};

}  // namespace

std::string_view toString(TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::End:
      return "end of file";
    case TokenKind::Identifier:
      return "identifier";
    case TokenKind::Integer:
      return "integer";
    case TokenKind::String:
      return "string";
    case TokenKind::LBrace:
      return "'{'";
    case TokenKind::RBrace:
      return "'}'";
    case TokenKind::LParen:
      return "'('";
    case TokenKind::RParen:
      return "')'";
    case TokenKind::LBracket:
      return "'['";
    case TokenKind::RBracket:
      return "']'";
    case TokenKind::Comma:
      return "','";
    case TokenKind::Semicolon:
      return "';'";
    case TokenKind::Colon:
      return "':'";
    case TokenKind::Dot:
      return "'.'";
    case TokenKind::DotDot:
      return "'..'";
    case TokenKind::Question:
      return "'?'";
    case TokenKind::Arrow:
      return "'->'";
    case TokenKind::Assign:
      return "'='";
    case TokenKind::Equal:
      return "'=='";
    case TokenKind::NotEqual:
      return "'!='";
    case TokenKind::Less:
      return "'<'";
    case TokenKind::LessEqual:
      return "'<='";
    case TokenKind::Greater:
      return "'>'";
    case TokenKind::GreaterEqual:
      return "'>='";
    case TokenKind::LessU:
      return "'<u'";
    case TokenKind::LessEqualU:
      return "'<=u'";
    case TokenKind::GreaterU:
      return "'>u'";
    case TokenKind::GreaterEqualU:
      return "'>=u'";
    case TokenKind::Plus:
      return "'+'";
    case TokenKind::Minus:
      return "'-'";
    case TokenKind::Star:
      return "'*'";
    case TokenKind::Slash:
      return "'/'";
    case TokenKind::SlashS:
      return "'/s'";
    case TokenKind::Percent:
      return "'%'";
    case TokenKind::PercentS:
      return "'%s'";
    case TokenKind::Ampersand:
      return "'&'";
    case TokenKind::Pipe:
      return "'|'";
    case TokenKind::Caret:
      return "'^'";
    case TokenKind::Tilde:
      return "'~'";
    case TokenKind::Bang:
      return "'!'";
    case TokenKind::Shl:
      return "'<<'";
    case TokenKind::Shr:
      return "'>>'";
    case TokenKind::ShrS:
      return "'>>>'";
    case TokenKind::AndAnd:
      return "'&&'";
    case TokenKind::OrOr:
      return "'||'";
  }
  return "token";
}

Result<std::vector<Token>> tokenize(std::string_view text, std::string_view sourceName) {
  Lexer lexer{text, sourceName};
  return lexer.run();
}

}  // namespace xdec::spec
