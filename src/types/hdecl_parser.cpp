#include "xdec/types/parse.h"

#include <array>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>

#ifndef XDEC_TYPES_DIR
#define XDEC_TYPES_DIR "types"
#endif

namespace xdec::types {
namespace {

enum class Tok : uint8_t { End, Ident, Number, Punct };

struct Token {
  Tok kind = Tok::End;
  std::string text;
  int64_t number = 0;
  unsigned line = 0;

  [[nodiscard]] bool is(std::string_view spelling) const noexcept {
    return (kind == Tok::Punct || kind == Tok::Ident) && text == spelling;
  }
};

[[nodiscard]] bool isIdentStart(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}

[[nodiscard]] bool isIdentBody(char c) noexcept {
  return isIdentStart(c) || (c >= '0' && c <= '9');
}

/// Every C keyword this parser recognises as part of a type. Anything else
/// that appears where a type specifier belongs is either a typedef name (if
/// the database knows it) or a skipped declaration.
[[nodiscard]] bool isQualifier(std::string_view word) noexcept {
  static constexpr std::array kQualifiers = {
      "const",    "volatile", "restrict", "__restrict", "__restrict__",
      "_Atomic",  "static",   "extern",   "inline",     "__inline",
      "__inline__", "register", "_Noreturn", "__extension__", "auto",
      "_Nonnull", "_Nullable", "_Null_unspecified", "__unaligned"};
  for (const char* candidate : kQualifiers) {
    if (word == candidate) {
      return true;
    }
  }
  return false;
}

/// The tokeniser. Comments and preprocessor lines are handled here rather than
/// in a separate pass so that line numbers in warnings stay true to the file.
class Lexer {
 public:
  Lexer(std::string_view text, std::map<std::string, int64_t>& defines)
      : text_(text), defines_(&defines) {}

  [[nodiscard]] Result<std::vector<Token>> tokenize() {
    std::vector<Token> tokens;
    while (true) {
      XDEC_TRY_VOID(skipTrivia());
      if (pos_ >= text_.size()) {
        break;
      }
      Token token;
      token.line = line_;
      const char c = text_[pos_];
      if (isIdentStart(c)) {
        const std::size_t start = pos_;
        while (pos_ < text_.size() && isIdentBody(text_[pos_])) {
          ++pos_;
        }
        token.kind = Tok::Ident;
        token.text = std::string{text_.substr(start, pos_ - start)};
      } else if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
        token.kind = Tok::Number;
        token.number = lexNumber();
        token.text = "<number>";
      } else if (c == '\'' || c == '"') {
        // Character and string literals only appear inside things this parser
        // skips (attributes, asm labels); keeping them as one token stops a
        // quote from swallowing the rest of the file.
        token.kind = Tok::Punct;
        token.text = lexQuoted(c);
      } else {
        token.kind = Tok::Punct;
        token.text = lexPunct();
      }
      tokens.push_back(std::move(token));
    }
    Token end;
    end.line = line_;
    tokens.push_back(std::move(end));
    return tokens;
  }

 private:
  [[nodiscard]] Result<void> skipTrivia() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == '\n') {
        ++line_;
        ++pos_;
        atLineStart_ = true;
        continue;
      }
      if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
        ++pos_;
        continue;
      }
      if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '/') {
        while (pos_ < text_.size() && text_[pos_] != '\n') {
          ++pos_;
        }
        continue;
      }
      if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '*') {
        const unsigned openedAt = line_;
        pos_ += 2;
        while (true) {
          if (pos_ + 1 >= text_.size()) {
            return err(DiagCode::ParseError,
                       std::format("unterminated /* comment opened on line {}", openedAt));
          }
          if (text_[pos_] == '\n') {
            ++line_;
          }
          if (text_[pos_] == '*' && text_[pos_ + 1] == '/') {
            pos_ += 2;
            break;
          }
          ++pos_;
        }
        continue;
      }
      if (c == '#' && atLineStart_) {
        skipDirective();
        continue;
      }
      atLineStart_ = false;
      return ok();
    }
    return ok();
  }

  /// Preprocessor lines are consumed, not honoured, with one exception:
  /// `#define NAME <integer>` records a constant, because array lengths and
  /// enum values in real headers are written that way.
  void skipDirective() {
    const std::size_t start = pos_;
    while (pos_ < text_.size()) {
      if (text_[pos_] == '\\' && pos_ + 1 < text_.size() &&
          (text_[pos_ + 1] == '\n' || text_[pos_ + 1] == '\r')) {
        ++line_;
        pos_ += 2;
        continue;
      }
      if (text_[pos_] == '\n') {
        break;
      }
      ++pos_;
    }
    const std::string_view directive = text_.substr(start, pos_ - start);
    recordDefine(directive);
  }

  void recordDefine(std::string_view directive) {
    std::size_t cursor = 1;  // past '#'
    auto skipSpace = [&] {
      while (cursor < directive.size() &&
             (directive[cursor] == ' ' || directive[cursor] == '\t')) {
        ++cursor;
      }
    };
    skipSpace();
    if (directive.compare(cursor, 6, "define") != 0) {
      return;
    }
    cursor += 6;
    skipSpace();
    const std::size_t nameStart = cursor;
    while (cursor < directive.size() && isIdentBody(directive[cursor])) {
      ++cursor;
    }
    if (cursor == nameStart || (cursor < directive.size() && directive[cursor] == '(')) {
      return;  // no name, or function-like
    }
    const std::string name{directive.substr(nameStart, cursor - nameStart)};
    skipSpace();
    const std::string_view rest = directive.substr(cursor);
    std::string cleaned;
    for (const char c : rest) {
      if (c != 'u' && c != 'U' && c != 'l' && c != 'L') {
        cleaned.push_back(c);
      }
    }
    try {
      std::size_t consumed = 0;
      const int64_t value = std::stoll(cleaned, &consumed, 0);
      while (consumed < cleaned.size() &&
             (cleaned[consumed] == ' ' || cleaned[consumed] == '\t')) {
        ++consumed;
      }
      if (consumed == cleaned.size()) {
        (*defines_)[name] = value;
      }
    } catch (const std::exception&) {
      // Not an integer constant. Nothing to record, nothing to complain about.
    }
  }

  [[nodiscard]] int64_t lexNumber() {
    const std::size_t start = pos_;
    int base = 10;
    if (text_[pos_] == '0' && pos_ + 1 < text_.size() &&
        (text_[pos_ + 1] == 'x' || text_[pos_ + 1] == 'X')) {
      base = 16;
      pos_ += 2;
    } else if (text_[pos_] == '0' && pos_ + 1 < text_.size() &&
               (text_[pos_ + 1] == 'b' || text_[pos_ + 1] == 'B')) {
      base = 2;
      pos_ += 2;
    }
    while (pos_ < text_.size() && (isIdentBody(text_[pos_]) || text_[pos_] == '.')) {
      ++pos_;
    }
    std::string token{text_.substr(start, pos_ - start)};
    try {
      return std::stoll(token, nullptr, base == 10 ? 0 : base);
    } catch (const std::exception&) {
      return 0;
    }
  }

  [[nodiscard]] std::string lexQuoted(char quote) {
    const std::size_t start = pos_;
    ++pos_;
    while (pos_ < text_.size() && text_[pos_] != quote) {
      if (text_[pos_] == '\\' && pos_ + 1 < text_.size()) {
        ++pos_;
      }
      if (text_[pos_] == '\n') {
        ++line_;
      }
      ++pos_;
    }
    if (pos_ < text_.size()) {
      ++pos_;
    }
    return std::string{text_.substr(start, pos_ - start)};
  }

  [[nodiscard]] std::string lexPunct() {
    static constexpr std::array kTwoChar = {"<<", ">>", "::", "->", "&&", "||",
                                            "==", "!=", "<=", ">="};
    if (pos_ + 1 < text_.size()) {
      const std::string_view pair = text_.substr(pos_, 2);
      for (const char* candidate : kTwoChar) {
        if (pair == candidate) {
          pos_ += 2;
          return std::string{pair};
        }
      }
    }
    return std::string{text_.substr(pos_++, 1)};
  }

  std::string_view text_;
  std::map<std::string, int64_t>* defines_;
  std::size_t pos_ = 0;
  unsigned line_ = 1;
  bool atLineStart_ = true;
};

/// What a declarator says on top of the base type: the pointer/array/function
/// wrapping, and the name (empty for an abstract declarator).
struct Declarator {
  std::string name;
  /// Applied outermost-last: the base type is fed through these in order.
  struct Step {
    enum class Kind : uint8_t { Pointer, Array, Function } kind;
    uint64_t length = kUnknownArrayLength;
    std::vector<FunctionParam> params;
    bool variadic = false;
  };
  std::vector<Step> steps;
};

class Parser {
 public:
  Parser(std::vector<Token> tokens, std::map<std::string, int64_t> defines,
         TypeDatabase& database)
      : tokens_(std::move(tokens)), constants_(std::move(defines)), database_(&database) {}

  [[nodiscard]] ParseReport run() {
    while (!atEnd()) {
      const std::size_t before = pos_;
      // Stray separators, and the closing brace of an `extern "C"` group the
      // parser deliberately steps into rather than over.
      if (peek().is(";") || peek().is("}")) {
        advance();
        continue;
      }
      Result<bool> declaration = parseDeclaration();
      if (!declaration) {
        warn(peek().line, declaration.error().message());
        ++report_.skipped;
        recover();
      } else if (*declaration) {
        ++report_.accepted;
      }
      if (pos_ == before) {
        // A declaration that consumed nothing would spin forever; the honest
        // response is to report the token and step over it.
        warn(peek().line, std::format("unexpected token '{}'", peek().text));
        ++report_.skipped;
        recover();
      }
    }
    return std::move(report_);
  }

 private:
  // --- Token access ------------------------------------------------------

  [[nodiscard]] const Token& peek(std::size_t ahead = 0) const {
    const std::size_t index = pos_ + ahead;
    return index < tokens_.size() ? tokens_[index] : tokens_.back();
  }
  [[nodiscard]] bool atEnd() const { return peek().kind == Tok::End; }
  const Token& advance() {
    const Token& token = peek();
    if (pos_ + 1 < tokens_.size()) {
      ++pos_;
    }
    return token;
  }
  [[nodiscard]] bool accept(std::string_view spelling) {
    if (peek().is(spelling)) {
      advance();
      return true;
    }
    return false;
  }
  [[nodiscard]] Result<void> expect(std::string_view spelling) {
    if (accept(spelling)) {
      return ok();
    }
    return err(DiagCode::ParseError,
               std::format("expected '{}', found '{}'", spelling, peek().text));
  }

  void warn(unsigned line, std::string message) {
    report_.warnings.push_back(ParseWarning{line, std::move(message)});
  }

  /// Skips to just past the next top-level `;`, so one bad declaration costs
  /// one declaration.
  void recover() {
    int depth = 0;
    while (!atEnd()) {
      const Token& token = advance();
      if (token.is("{") || token.is("(") || token.is("[")) {
        ++depth;
      } else if (token.is("}") || token.is(")") || token.is("]")) {
        if (depth > 0) {
          --depth;
        }
      } else if (token.is(";") && depth == 0) {
        return;
      }
    }
  }

  /// `__attribute__((...))`, `__asm__("...")` and friends: balanced groups that
  /// carry nothing this parser models. Consuming them is what lets real NDK
  /// headers through.
  bool skipAttributes() {
    bool skipped = false;
    while (true) {
      const Token& token = peek();
      if (token.kind != Tok::Ident) {
        break;
      }
      const bool isAttribute = token.text == "__attribute__" || token.text == "__attribute" ||
                               token.text == "__asm" || token.text == "__asm__" ||
                               token.text == "asm" || token.text == "__declspec";
      if (!isAttribute) {
        break;
      }
      advance();
      if (peek().is("(")) {
        skipBalanced("(", ")");
      }
      skipped = true;
    }
    return skipped;
  }

  void skipBalanced(std::string_view open, std::string_view close) {
    if (!accept(open)) {
      return;
    }
    int depth = 1;
    while (!atEnd() && depth > 0) {
      const Token& token = advance();
      if (token.is(open)) {
        ++depth;
      } else if (token.is(close)) {
        --depth;
      }
    }
  }

  // --- Constant expressions ---------------------------------------------

  /// Enough of a constant expression to read array lengths and enum values:
  /// integers, named constants, the usual binary operators, and parentheses.
  /// Anything else fails, and the caller decides whether that is fatal.
  [[nodiscard]] Result<int64_t> parseConstant(int minPrecedence = 0) {
    XDEC_TRY(int64_t left, parseUnaryConstant());
    while (true) {
      const std::string& op = peek().text;
      const int precedence = binaryPrecedence(op);
      if (peek().kind != Tok::Punct || precedence < 0 || precedence < minPrecedence) {
        return left;
      }
      advance();
      XDEC_TRY(const int64_t right, parseConstant(precedence + 1));
      left = applyBinary(op, left, right);
    }
  }

  [[nodiscard]] static int binaryPrecedence(const std::string& op) {
    if (op == "*" || op == "/" || op == "%") return 5;
    if (op == "+" || op == "-") return 4;
    if (op == "<<" || op == ">>") return 3;
    if (op == "&") return 2;
    if (op == "^") return 1;
    if (op == "|") return 0;
    return -1;
  }

  [[nodiscard]] static int64_t applyBinary(const std::string& op, int64_t lhs, int64_t rhs) {
    const auto ulhs = static_cast<uint64_t>(lhs);
    const auto urhs = static_cast<uint64_t>(rhs);
    if (op == "*") return static_cast<int64_t>(ulhs * urhs);
    if (op == "/") return rhs == 0 ? 0 : lhs / rhs;
    if (op == "%") return rhs == 0 ? 0 : lhs % rhs;
    if (op == "+") return static_cast<int64_t>(ulhs + urhs);
    if (op == "-") return static_cast<int64_t>(ulhs - urhs);
    if (op == "<<") return static_cast<int64_t>(ulhs << (urhs & 63U));
    if (op == ">>") return static_cast<int64_t>(ulhs >> (urhs & 63U));
    if (op == "&") return static_cast<int64_t>(ulhs & urhs);
    if (op == "^") return static_cast<int64_t>(ulhs ^ urhs);
    if (op == "|") return static_cast<int64_t>(ulhs | urhs);
    return 0;
  }

  [[nodiscard]] Result<int64_t> parseUnaryConstant() {
    if (accept("-")) {
      XDEC_TRY(const int64_t value, parseUnaryConstant());
      return static_cast<int64_t>(0U - static_cast<uint64_t>(value));
    }
    if (accept("+")) {
      return parseUnaryConstant();
    }
    if (accept("~")) {
      XDEC_TRY(const int64_t value, parseUnaryConstant());
      return ~value;
    }
    if (accept("(")) {
      XDEC_TRY(const int64_t value, parseConstant());
      XDEC_TRY_VOID(expect(")"));
      return value;
    }
    if (peek().kind == Tok::Number) {
      return advance().number;
    }
    if (peek().kind == Tok::Ident) {
      const auto found = constants_.find(peek().text);
      if (found != constants_.end()) {
        advance();
        return found->second;
      }
      return err(DiagCode::ParseError,
                 std::format("unknown constant '{}'", peek().text));
    }
    return err(DiagCode::ParseError,
               std::format("expected a constant, found '{}'", peek().text));
  }

  // --- Type specifiers ---------------------------------------------------

  struct Specifiers {
    TypeId type;
    bool isTypedef = false;
  };

  /// Reads the leading run of storage classes, qualifiers and type keywords,
  /// resolving them to one TypeId.
  [[nodiscard]] Result<Specifiers> parseSpecifiers() {
    Specifiers result;
    std::vector<std::string> keywords;
    bool sawTypeName = false;

    while (!atEnd()) {
      if (skipAttributes()) {
        continue;
      }
      const Token& token = peek();
      if (token.kind != Tok::Ident) {
        break;
      }
      if (token.text == "typedef") {
        advance();
        result.isTypedef = true;
        continue;
      }
      if (isQualifier(token.text)) {
        advance();
        continue;
      }
      if (token.text == "struct" || token.text == "union" || token.text == "enum") {
        if (sawTypeName) {
          break;
        }
        XDEC_TRY(const TypeId aggregate, parseAggregateSpecifier(token.text));
        result.type = aggregate;
        sawTypeName = true;
        continue;
      }
      if (isBuiltinKeyword(token.text)) {
        keywords.push_back(token.text);
        advance();
        sawTypeName = true;
        continue;
      }
      if (!sawTypeName && keywords.empty()) {
        const TypeId named = database_->lookup(token.text);
        if (named.valid()) {
          advance();
          result.type = named;
          sawTypeName = true;
          continue;
        }
      }
      break;
    }

    if (!keywords.empty()) {
      XDEC_TRY(result.type, resolveBuiltin(keywords));
    }
    if (!result.type.valid()) {
      return err(DiagCode::ParseError,
                 std::format("no type specifier before '{}'", peek().text));
    }
    return result;
  }

  [[nodiscard]] static bool isBuiltinKeyword(std::string_view word) noexcept {
    static constexpr std::array kBuiltins = {"void",  "char",  "short",   "int",
                                             "long",  "float", "double",  "signed",
                                             "unsigned", "_Bool", "__int128"};
    for (const char* candidate : kBuiltins) {
      if (word == candidate) {
        return true;
      }
    }
    return false;
  }

  /// Maps a multiset of type keywords onto one entry, so `unsigned long int`
  /// and `long unsigned` land on the same 64-bit unsigned type.
  [[nodiscard]] Result<TypeId> resolveBuiltin(const std::vector<std::string>& keywords) {
    bool isUnsigned = false;
    bool isSignedExplicit = false;
    unsigned longCount = 0;
    bool isShort = false;
    bool isChar = false;
    bool isInt = false;
    bool isVoid = false;
    bool isFloat = false;
    bool isDouble = false;
    bool isBool = false;
    bool isInt128 = false;
    for (const std::string& word : keywords) {
      if (word == "unsigned") isUnsigned = true;
      else if (word == "signed") isSignedExplicit = true;
      else if (word == "long") ++longCount;
      else if (word == "short") isShort = true;
      else if (word == "char") isChar = true;
      else if (word == "int") isInt = true;
      else if (word == "void") isVoid = true;
      else if (word == "float") isFloat = true;
      else if (word == "double") isDouble = true;
      else if (word == "_Bool") isBool = true;
      else if (word == "__int128") isInt128 = true;
    }
    if (isVoid) {
      return database_->voidType();
    }
    if (isBool) {
      return database_->boolType();
    }
    if (isDouble) {
      return database_->floatType(64);
    }
    if (isFloat) {
      return database_->floatType(32);
    }
    if (isInt128) {
      return database_->intType(128, !isUnsigned);
    }
    uint32_t width = 32;
    if (isChar) {
      width = 8;
    } else if (isShort) {
      width = 16;
    } else if (longCount > 0) {
      width = 64;
    }
    if (!isChar && !isShort && longCount == 0 && !isInt && !isSignedExplicit && !isUnsigned) {
      return err(DiagCode::ParseError, "empty type specifier");
    }
    // Plain `char` is signed on AArch64 Linux only for the purpose of this
    // model; the distinction never affects a decompiled listing's meaning.
    return database_->intType(width, !isUnsigned);
  }

  [[nodiscard]] Result<TypeId> parseAggregateSpecifier(const std::string& keyword) {
    advance();  // struct / union / enum
    skipAttributes();
    const TypeKind kind = keyword == "struct"  ? TypeKind::Struct
                          : keyword == "union" ? TypeKind::Union
                                               : TypeKind::Enum;
    std::string tag;
    if (peek().kind == Tok::Ident && !peek().is("{")) {
      tag = advance().text;
    }
    skipAttributes();

    const bool hasBody = peek().is("{");
    TypeId id = tag.empty() ? database_->createAnonymousAggregate(kind)
                            : database_->declareTag(kind, tag);
    if (!hasBody) {
      if (tag.empty()) {
        return err(DiagCode::ParseError, std::format("{} with neither tag nor body", keyword));
      }
      return id;
    }

    // A second definition of the same tag is a redefinition; parse the body
    // anyway so recovery lands in the right place, then report it.
    const TypeEntry* existing = database_->get(id);
    const bool alreadyComplete = existing != nullptr && existing->complete;

    if (kind == TypeKind::Enum) {
      XDEC_TRY(std::vector<EnumConstant> constants, parseEnumBody());
      if (alreadyComplete) {
        return err(DiagCode::ParseError, std::format("redefinition of enum '{}'", tag));
      }
      XDEC_TRY_VOID(database_->defineEnum(id, std::move(constants), TypeId::invalid()));
      return id;
    }

    XDEC_TRY(std::vector<StructField> fields, parseStructBody());
    if (alreadyComplete) {
      return err(DiagCode::ParseError, std::format("redefinition of {} '{}'", keyword, tag));
    }
    XDEC_TRY_VOID(database_->defineAggregate(id, std::move(fields)));
    return id;
  }

  [[nodiscard]] Result<std::vector<EnumConstant>> parseEnumBody() {
    XDEC_TRY_VOID(expect("{"));
    std::vector<EnumConstant> constants;
    int64_t next = 0;
    while (!atEnd() && !peek().is("}")) {
      if (peek().kind != Tok::Ident) {
        return err(DiagCode::ParseError,
                   std::format("expected an enumerator, found '{}'", peek().text));
      }
      EnumConstant constant;
      constant.name = advance().text;
      skipAttributes();
      if (accept("=")) {
        XDEC_TRY(const int64_t value, parseConstant());
        next = value;
      }
      constant.value = next;
      ++next;
      // Enumerators are constants in their own right for later expressions.
      constants_[constant.name] = constant.value;
      constants.push_back(std::move(constant));
      if (!accept(",")) {
        break;
      }
    }
    XDEC_TRY_VOID(expect("}"));
    return constants;
  }

  [[nodiscard]] Result<std::vector<StructField>> parseStructBody() {
    XDEC_TRY_VOID(expect("{"));
    std::vector<StructField> fields;
    while (!atEnd() && !peek().is("}")) {
      if (accept(";")) {
        continue;
      }
      XDEC_TRY(const Specifiers specifiers, parseSpecifiers());
      if (accept(";")) {
        // An anonymous struct/union member. C11 makes its fields members of
        // the enclosing type; that flattening is not modelled, so the member
        // is dropped with a note rather than silently misplacing offsets.
        warn(peek().line, "anonymous struct/union member is not modelled");
        continue;
      }
      while (true) {
        XDEC_TRY(Declarator declarator, parseDeclarator());
        skipAttributes();
        StructField field;
        field.name = declarator.name;
        if (accept(":")) {
          XDEC_TRY(const int64_t bits, parseConstant());
          warn(peek().line,
               std::format("bit-field '{}':{} is modelled as a whole member; offsets after "
                           "it may be wrong",
                           field.name.empty() ? "<unnamed>" : field.name, bits));
        }
        XDEC_TRY(const TypeId type, applyDeclarator(specifiers.type, declarator));
        field.type = type;
        // `uint8_t data[]` at the end of a struct: no size, and the offset the
        // layout gives it is the struct's size.
        const TypeEntry* entry = database_->get(type);
        field.flexible = entry != nullptr && entry->kind == TypeKind::Array &&
                         entry->arrayLength == kUnknownArrayLength;
        if (field.flexible) {
          field.type = entry->element;
        }
        if (!field.name.empty()) {
          fields.push_back(std::move(field));
        }
        if (!accept(",")) {
          break;
        }
      }
      XDEC_TRY_VOID(expect(";"));
    }
    XDEC_TRY_VOID(expect("}"));
    return fields;
  }

  // --- Declarators -------------------------------------------------------

  [[nodiscard]] Result<Declarator> parseDeclarator() {
    Declarator declarator;
    XDEC_TRY_VOID(parseDeclaratorInto(declarator));
    return declarator;
  }

  /// C declarator syntax binds suffixes tighter than the pointers written to
  /// the left of the name, and a parenthesised declarator outranks both. The
  /// step list records that as an application order: pointers first, then the
  /// suffixes in reverse (so `int a[3][4]` is 3 arrays of 4, not the other way
  /// round), then whatever the parenthesised inner declarator adds. Reading
  /// `int *p[4]` as "pointer to array" instead of "array of pointers" is the
  /// classic way to get this wrong, so each case is spelled out in the tests.
  [[nodiscard]] Result<void> parseDeclaratorInto(Declarator& declarator) {
    std::vector<Declarator::Step> pointers;
    while (accept("*")) {
      while (peek().kind == Tok::Ident && isQualifier(peek().text)) {
        advance();
      }
      skipAttributes();
      pointers.push_back(Declarator::Step{Declarator::Step::Kind::Pointer, 0, {}, false});
    }
    skipAttributes();

    Declarator inner;
    bool hasInner = false;
    if (peek().is("(") && !startsParameterList()) {
      advance();
      XDEC_TRY_VOID(parseDeclaratorInto(inner));
      XDEC_TRY_VOID(expect(")"));
      hasInner = true;
    } else if (peek().kind == Tok::Ident && !isQualifier(peek().text)) {
      declarator.name = advance().text;
    }

    // Suffixes bind tighter than the pointers to the left of the name.
    std::vector<Declarator::Step> suffixes;
    while (!atEnd()) {
      if (peek().is("[")) {
        advance();
        Declarator::Step step{Declarator::Step::Kind::Array, kUnknownArrayLength, {}, false};
        if (!peek().is("]")) {
          Result<int64_t> length = parseConstant();
          if (length) {
            step.length = static_cast<uint64_t>(*length);
          } else {
            warn(peek().line, "array length is not a constant expression; treated as unsized");
            while (!atEnd() && !peek().is("]")) {
              advance();
            }
          }
        }
        XDEC_TRY_VOID(expect("]"));
        suffixes.push_back(std::move(step));
        continue;
      }
      if (peek().is("(")) {
        XDEC_TRY(Declarator::Step step, parseParameterList());
        suffixes.push_back(std::move(step));
        continue;
      }
      break;
    }
    skipAttributes();

    for (Declarator::Step& step : pointers) {
      declarator.steps.push_back(std::move(step));
    }
    for (auto it = suffixes.rbegin(); it != suffixes.rend(); ++it) {
      declarator.steps.push_back(std::move(*it));
    }
    if (hasInner) {
      declarator.name = inner.name;
      for (Declarator::Step& step : inner.steps) {
        declarator.steps.push_back(std::move(step));
      }
    }
    return ok();
  }

  /// Distinguishes `(*fp)(int)` — a parenthesised declarator — from `foo(int)`
  /// — a parameter list. The only way to tell is to look at what follows the
  /// paren, because C's grammar is ambiguous here without a symbol table.
  [[nodiscard]] bool startsParameterList() const {
    const Token& after = peek(1);
    if (after.is(")")) {
      return true;  // `f()`
    }
    if (after.is("*") || after.is("(") || after.is("[")) {
      return false;  // `(*p)`, `((x))`
    }
    if (after.kind != Tok::Ident) {
      return false;
    }
    if (isQualifier(after.text) || isBuiltinKeyword(after.text) || after.text == "struct" ||
        after.text == "union" || after.text == "enum" || after.text == "void") {
      return true;
    }
    // A known type name starts a parameter; anything else is the declared name
    // of an inner declarator.
    return database_->lookup(after.text).valid();
  }

  [[nodiscard]] Result<Declarator::Step> parseParameterList() {
    XDEC_TRY_VOID(expect("("));
    Declarator::Step step{Declarator::Step::Kind::Function, 0, {}, false};
    if (accept(")")) {
      // `f()` — an unprototyped declaration. Treated as taking no arguments,
      // which is what every header that means anything by it intends.
      return step;
    }
    if (peek().is("void") && peek(1).is(")")) {
      advance();
      advance();
      return step;
    }
    while (true) {
      if (peek().is(".") && peek(1).is(".") && peek(2).is(".")) {
        advance();
        advance();
        advance();
        step.variadic = true;
        break;
      }
      XDEC_TRY(const Specifiers specifiers, parseSpecifiers());
      XDEC_TRY(Declarator inner, parseDeclarator());
      XDEC_TRY(TypeId type, applyDeclarator(specifiers.type, inner));
      // C decays array and function parameters to pointers; a decompiled
      // signature that said `int a[4]` would be claiming something the ABI
      // does not carry.
      const TypeEntry* entry = database_->get(type);
      if (entry != nullptr &&
          (entry->kind == TypeKind::Array || entry->kind == TypeKind::Function)) {
        type = database_->pointerTo(entry->kind == TypeKind::Array ? entry->element : type);
      }
      step.params.push_back(FunctionParam{inner.name, type});
      if (!accept(",")) {
        break;
      }
    }
    XDEC_TRY_VOID(expect(")"));
    return step;
  }

  [[nodiscard]] Result<TypeId> applyDeclarator(TypeId base, const Declarator& declarator) {
    TypeId type = base;
    for (const Declarator::Step& step : declarator.steps) {
      switch (step.kind) {
        case Declarator::Step::Kind::Pointer:
          type = database_->pointerTo(type);
          break;
        case Declarator::Step::Kind::Array:
          type = database_->arrayOf(type, step.length);
          break;
        case Declarator::Step::Kind::Function:
          type = database_->functionType(type, step.params, step.variadic);
          break;
      }
    }
    return type;
  }

  // --- Top-level declarations -------------------------------------------

  /// Returns whether anything was imported. A declaration that is only a type
  /// definition (`struct foo { ... };`) counts.
  [[nodiscard]] Result<bool> parseDeclaration() {
    skipAttributes();
    if (accept("extern") && peek().is("\"C\"")) {
      // `extern "C" { ... }` — a C++ header's wrapper. The brace group is
      // transparent, so step into it rather than over it.
      advance();
      (void)accept("{");
      return false;
    }
    XDEC_TRY(const Specifiers specifiers, parseSpecifiers());
    if (accept(";")) {
      return true;  // a bare struct/enum definition
    }

    bool imported = false;
    while (true) {
      XDEC_TRY(Declarator declarator, parseDeclarator());
      skipAttributes();
      if (accept("=")) {
        // An initialiser tells us nothing about the type; skip it.
        while (!atEnd() && !peek().is(",") && !peek().is(";")) {
          if (peek().is("{")) {
            skipBalanced("{", "}");
            continue;
          }
          advance();
        }
      }
      XDEC_TRY(const TypeId type, applyDeclarator(specifiers.type, declarator));

      if (declarator.name.empty()) {
        return err(DiagCode::ParseError, "declaration without a name");
      }
      if (specifiers.isTypedef) {
        Result<TypeId> alias = database_->addTypedef(declarator.name, type);
        if (!alias) {
          return std::move(alias).takeUnexpected();
        }
        imported = true;
      } else {
        const TypeEntry* entry = database_->get(type);
        if (entry != nullptr && entry->kind == TypeKind::Function) {
          XDEC_TRY_VOID(database_->declareFunction(declarator.name, type));
        } else {
          XDEC_TRY_VOID(database_->declareGlobal(declarator.name, type));
        }
        imported = true;
      }
      if (!accept(",")) {
        break;
      }
    }

    if (peek().is("{")) {
      // A function body in an imported header. The prototype is already
      // recorded; the body is not this parser's business.
      skipBalanced("{", "}");
      return imported;
    }
    XDEC_TRY_VOID(expect(";"));
    return imported;
  }

  std::vector<Token> tokens_;
  std::map<std::string, int64_t> constants_;
  TypeDatabase* database_;
  std::size_t pos_ = 0;
  ParseReport report_;
};

}  // namespace

std::string ParseReport::format(std::string_view origin) const {
  std::string out = std::format("{}: {} declaration(s) imported", origin, accepted);
  if (skipped != 0) {
    out += std::format(", {} skipped", skipped);
  }
  for (const ParseWarning& warning : warnings) {
    out += std::format("\n  {}:{}: {}", origin, warning.line, warning.message);
  }
  return out;
}

Result<ParseReport> parseHeader(std::string_view text, TypeDatabase& database) {
  std::map<std::string, int64_t> defines;
  Lexer lexer{text, defines};
  XDEC_TRY(std::vector<Token> tokens, lexer.tokenize());
  Parser parser{std::move(tokens), std::move(defines), database};
  return parser.run();
}

Result<ParseReport> parseHeaderFile(const std::string& path, TypeDatabase& database) {
  std::ifstream stream{path, std::ios::binary};
  if (!stream) {
    return err(DiagCode::IoError, std::format("cannot open header '{}'", path));
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  const std::string text = buffer.str();
  Result<ParseReport> report = parseHeader(text, database);
  if (!report) {
    return err(Diag{report.error()}.note(std::format("while reading '{}'", path)));
  }
  return report;
}

Result<std::string> resolveHeaderPath(std::string_view nameOrPath) {
  namespace fs = std::filesystem;
  const fs::path given{nameOrPath};
  if (fs::exists(given)) {
    return given.string();
  }
  if (given.has_parent_path() || given.has_extension()) {
    return err(DiagCode::IoError, std::format("no such header '{}'", nameOrPath));
  }
  const fs::path preset =
      fs::path{XDEC_TYPES_DIR} / "presets" / (std::string{nameOrPath} + ".hdecl");
  if (fs::exists(preset)) {
    return preset.string();
  }
  return err(DiagCode::IoError,
             std::format("no header or preset named '{}' (looked in '{}')", nameOrPath,
                         preset.string()));
}

}  // namespace xdec::types
