#include "xdec/spec/parse.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <system_error>

#include "lexer.h"

namespace xdec::spec {
namespace {

/// Binding power for the binary operators, low to high. Zero means "not a
/// binary operator", which is how the precedence climb knows to stop.
[[nodiscard]] unsigned precedenceOf(TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::OrOr:
      return 1;
    case TokenKind::AndAnd:
      return 2;
    case TokenKind::Pipe:
      return 3;
    case TokenKind::Caret:
      return 4;
    case TokenKind::Ampersand:
      return 5;
    case TokenKind::Equal:
    case TokenKind::NotEqual:
      return 6;
    case TokenKind::Less:
    case TokenKind::LessEqual:
    case TokenKind::Greater:
    case TokenKind::GreaterEqual:
    case TokenKind::LessU:
    case TokenKind::LessEqualU:
    case TokenKind::GreaterU:
    case TokenKind::GreaterEqualU:
      return 7;
    case TokenKind::Shl:
    case TokenKind::Shr:
    case TokenKind::ShrS:
      return 8;
    case TokenKind::Plus:
    case TokenKind::Minus:
      return 9;
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::SlashS:
    case TokenKind::Percent:
    case TokenKind::PercentS:
      return 10;
    default:
      return 0;
  }
}

[[nodiscard]] BinaryOp binaryOpOf(TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::Plus:
      return BinaryOp::Add;
    case TokenKind::Minus:
      return BinaryOp::Sub;
    case TokenKind::Star:
      return BinaryOp::Mul;
    case TokenKind::Slash:
      return BinaryOp::DivU;
    case TokenKind::SlashS:
      return BinaryOp::DivS;
    case TokenKind::Percent:
      return BinaryOp::RemU;
    case TokenKind::PercentS:
      return BinaryOp::RemS;
    case TokenKind::Ampersand:
      return BinaryOp::And;
    case TokenKind::Pipe:
      return BinaryOp::Or;
    case TokenKind::Caret:
      return BinaryOp::Xor;
    case TokenKind::Shl:
      return BinaryOp::Shl;
    case TokenKind::Shr:
      return BinaryOp::ShrU;
    case TokenKind::ShrS:
      return BinaryOp::ShrS;
    case TokenKind::Equal:
      return BinaryOp::Equal;
    case TokenKind::NotEqual:
      return BinaryOp::NotEqual;
    case TokenKind::Less:
      return BinaryOp::LessS;
    case TokenKind::LessEqual:
      return BinaryOp::LessEqualS;
    case TokenKind::Greater:
      return BinaryOp::GreaterS;
    case TokenKind::GreaterEqual:
      return BinaryOp::GreaterEqualS;
    case TokenKind::LessU:
      return BinaryOp::LessU;
    case TokenKind::LessEqualU:
      return BinaryOp::LessEqualU;
    case TokenKind::GreaterU:
      return BinaryOp::GreaterU;
    case TokenKind::GreaterEqualU:
      return BinaryOp::GreaterEqualU;
    case TokenKind::AndAnd:
      return BinaryOp::LogicalAnd;
    case TokenKind::OrOr:
      return BinaryOp::LogicalOr;
    default:
      return BinaryOp::Add;
  }
}

class Parser {
 public:
  Parser(std::vector<Token> tokens, std::string_view sourceName)
      : tokens_(std::move(tokens)), sourceName_(sourceName) {}

  Result<std::unique_ptr<Module>> parseModule() {
    auto module = std::make_unique<Module>();
    module->sourceName = std::string{sourceName_};
    XDEC_TRY_VOID(parseArch(module->arch));
    XDEC_TRY_VOID(parseDeclarations(*module));
    return module;
  }

  /// An included file: declarations without an `arch` block, since there is one
  /// architecture per spec and the root file is where it is described.
  Result<std::unique_ptr<Module>> parseFragment() {
    auto module = std::make_unique<Module>();
    module->sourceName = std::string{sourceName_};
    if (atKeyword("arch")) {
      return fail(peek().loc,
                  "an included file cannot declare an arch block; the root spec declares it");
    }
    XDEC_TRY_VOID(parseDeclarations(*module));
    return module;
  }

  Result<void> parseDeclarations(Module& module) {
    while (!at(TokenKind::End)) {
      if (atKeyword("include")) {
        const SourceLoc loc = peek().loc;
        advance();
        XDEC_TRY(const Token* token, expect(TokenKind::String));
        module.includes.push_back(IncludeDecl{loc, token->text});
      } else if (atKeyword("fn")) {
        XDEC_TRY(FnDecl function, parseFn());
        module.functions.push_back(std::move(function));
      } else if (atKeyword("insn")) {
        XDEC_TRY(InsnDecl insn, parseInsn());
        module.instructions.push_back(std::move(insn));
      } else {
        return fail(peek().loc, std::format("expected 'include', 'fn' or 'insn' but found {}",
                                            describe(peek())));
      }
    }
    return ok();
  }

  Result<ExprPtr> parseStandaloneExpression() {
    XDEC_TRY(ExprPtr expr, parseExpr());
    if (!at(TokenKind::End)) {
      return fail(peek().loc, std::format("unexpected {} after expression", describe(peek())));
    }
    return expr;
  }

 private:
  // -- token helpers --------------------------------------------------------

  [[nodiscard]] const Token& peek(std::size_t ahead = 0) const {
    const std::size_t index = position_ + ahead;
    return index < tokens_.size() ? tokens_[index] : tokens_.back();
  }

  [[nodiscard]] bool at(TokenKind kind) const { return peek().kind == kind; }

  [[nodiscard]] bool atKeyword(std::string_view word) const {
    return peek().kind == TokenKind::Identifier && peek().text == word;
  }

  const Token& advance() {
    const Token& token = peek();
    if (position_ < tokens_.size() - 1) {
      ++position_;
    }
    return token;
  }

  bool accept(TokenKind kind) {
    if (!at(kind)) {
      return false;
    }
    advance();
    return true;
  }

  bool acceptKeyword(std::string_view word) {
    if (!atKeyword(word)) {
      return false;
    }
    advance();
    return true;
  }

  [[nodiscard]] static std::string describe(const Token& token) {
    if (token.kind == TokenKind::Identifier) {
      return std::format("'{}'", token.text);
    }
    if (token.kind == TokenKind::Integer) {
      return std::format("integer {}", token.integer);
    }
    return std::string{toString(token.kind)};
  }

  [[nodiscard]] Unexpected fail(SourceLoc loc, std::string message) const {
    return err(Diag{DiagCode::ParseError, std::move(message)}.note(
        std::format("{}:{}", sourceName_, loc.toString())));
  }

  Result<const Token*> expect(TokenKind kind) {
    if (!at(kind)) {
      return fail(peek().loc,
                  std::format("expected {} but found {}", toString(kind), describe(peek())));
    }
    return &advance();
  }

  Result<std::string> expectIdentifier(std::string_view what) {
    if (!at(TokenKind::Identifier)) {
      return fail(peek().loc,
                  std::format("expected {} but found {}", what, describe(peek())));
    }
    return advance().text;
  }

  Result<uint64_t> expectInteger(std::string_view what) {
    if (!at(TokenKind::Integer)) {
      return fail(peek().loc,
                  std::format("expected {} but found {}", what, describe(peek())));
    }
    return advance().integer;
  }

  Result<void> expectKeyword(std::string_view word) {
    if (!acceptKeyword(word)) {
      return fail(peek().loc,
                  std::format("expected '{}' but found {}", word, describe(peek())));
    }
    return ok();
  }

  // -- types ----------------------------------------------------------------

  Result<TypeExpr> parseType() {
    TypeExpr type;
    type.loc = peek().loc;
    XDEC_TRY(const std::string name, expectIdentifier("a type"));

    if (name == "int") {
      type.kind = TypeKind::Int;
      if (accept(TokenKind::LParen)) {
        XDEC_TRY(type.rangeLow, expectInteger("a range lower bound"));
        XDEC_TRY_VOID(expect(TokenKind::DotDot));
        XDEC_TRY(type.rangeHigh, expectInteger("a range upper bound"));
        XDEC_TRY_VOID(expect(TokenKind::RParen));
        if (type.rangeLow > type.rangeHigh) {
          return fail(type.loc, "range lower bound is above its upper bound");
        }
        type.hasRange = true;
      }
      return type;
    }
    if (name == "flags") {
      type.kind = TypeKind::Flags;
      return type;
    }
    if (name == "void") {
      type.kind = TypeKind::Void;
      return type;
    }
    if (name == "bits" || name == "float") {
      type.kind = name == "bits" ? TypeKind::Bits : TypeKind::Float;
      XDEC_TRY_VOID(expect(TokenKind::LParen));
      XDEC_TRY(type.width, parseExpr());
      XDEC_TRY_VOID(expect(TokenKind::RParen));
      return type;
    }
    return fail(type.loc, std::format("unknown type '{}'", name));
  }

  // -- architecture ---------------------------------------------------------

  Result<void> parseArch(ArchDecl& arch) {
    arch.loc = peek().loc;
    XDEC_TRY_VOID(expectKeyword("arch"));
    XDEC_TRY(arch.name, expectIdentifier("an architecture name"));
    // Qualified: the member function of the same name hides the one in xdec.
    if (!xdec::parseArch(arch.name, arch.arch)) {
      return fail(arch.loc, std::format("unknown architecture '{}'", arch.name));
    }
    XDEC_TRY_VOID(expect(TokenKind::LBrace));

    bool sawWidth = false;
    while (!accept(TokenKind::RBrace)) {
      if (at(TokenKind::End)) {
        return fail(peek().loc, "unterminated arch block");
      }
      const SourceLoc loc = peek().loc;
      XDEC_TRY(const std::string keyword, expectIdentifier("an arch property"));

      if (keyword == "endian") {
        XDEC_TRY(const std::string value, expectIdentifier("'little' or 'big'"));
        if (value == "little") {
          arch.endian = Endian::Little;
        } else if (value == "big") {
          arch.endian = Endian::Big;
        } else {
          return fail(loc, "endian must be 'little' or 'big'");
        }
      } else if (keyword == "insnwidth") {
        XDEC_TRY(const uint64_t bits, expectInteger("an instruction width in bits"));
        // Variable-length encodings need a different decoder shape than the
        // fixed-width decision tree, so v1 says so rather than half-working.
        if (bits == 0 || bits > 64 || bits % 8 != 0) {
          return fail(loc, "insnwidth must be a whole number of bytes, at most 64 bits");
        }
        arch.insnWidth = static_cast<unsigned>(bits);
        sawWidth = true;
      } else if (keyword == "pointer") {
        XDEC_TRY(const uint64_t bits, expectInteger("a pointer width in bits"));
        arch.pointerBits = static_cast<unsigned>(bits);
      } else if (keyword == "regfile") {
        XDEC_TRY(RegFileDecl file, parseRegFile(loc));
        arch.regFiles.push_back(std::move(file));
      } else if (keyword == "reg") {
        XDEC_TRY(RegDecl reg, parseReg(loc));
        arch.regs.push_back(std::move(reg));
      } else {
        return fail(loc, std::format("unknown arch property '{}'", keyword));
      }
    }

    if (!sawWidth) {
      return fail(arch.loc, "arch block does not declare insnwidth");
    }
    if (arch.pointerBits == 0) {
      arch.pointerBits = pointerBits(arch.arch);
    }
    return ok();
  }

  Result<RegFileDecl> parseRegFile(SourceLoc loc) {
    RegFileDecl file;
    file.loc = loc;
    XDEC_TRY(file.name, expectIdentifier("a register file name"));
    XDEC_TRY_VOID(expect(TokenKind::Colon));
    XDEC_TRY(const TypeExpr type, parseType());
    XDEC_TRY(file.bits, constantWidth(type, "register file element"));
    XDEC_TRY_VOID(expect(TokenKind::LBracket));
    XDEC_TRY(const uint64_t count, expectInteger("a register count"));
    XDEC_TRY_VOID(expect(TokenKind::RBracket));
    if (count == 0 || count > 1024) {
      return fail(loc, "register count must be between 1 and 1024");
    }
    file.count = static_cast<unsigned>(count);
    XDEC_TRY_VOID(expect(TokenKind::LBrace));

    while (!accept(TokenKind::RBrace)) {
      if (at(TokenKind::End)) {
        return fail(peek().loc, "unterminated regfile block");
      }
      const SourceLoc itemLoc = peek().loc;
      XDEC_TRY(const std::string keyword, expectIdentifier("a regfile property"));

      if (keyword == "prefix") {
        XDEC_TRY(const Token* token, expect(TokenKind::String));
        file.prefix = token->text;
      } else if (keyword == "role") {
        XDEC_TRY(const std::string role, expectIdentifier("a register role"));
        if (!parseRegRole(role, file.role)) {
          return fail(itemLoc, std::format("unknown register role '{}'", role));
        }
        if (file.role == RegRole::Flags) {
          return fail(itemLoc, "a register file cannot hold flags");
        }
      } else if (keyword == "zero") {
        XDEC_TRY(const uint64_t index, expectInteger("a register index"));
        if (index >= file.count) {
          return fail(itemLoc, "zero register index is outside the file");
        }
        file.zeroIndex = static_cast<unsigned>(index);
        XDEC_TRY_VOID(expectKeyword("as"));
        XDEC_TRY(const Token* token, expect(TokenKind::String));
        file.zeroName = token->text;
      } else if (keyword == "view") {
        XDEC_TRY(RegViewDecl view, parseRegView(itemLoc, file));
        file.views.push_back(std::move(view));
      } else {
        return fail(itemLoc, std::format("unknown regfile property '{}'", keyword));
      }
    }

    if (file.prefix.empty()) {
      return fail(loc, "regfile does not declare a name prefix");
    }
    return file;
  }

  /// Whether the file needs a zero name for this view depends on a property that
  /// may be declared after it, so the checker enforces that rather than this.
  Result<RegViewDecl> parseRegView(SourceLoc loc, const RegFileDecl& file) {
    RegViewDecl view;
    view.loc = loc;
    XDEC_TRY(view.name, expectIdentifier("a view name"));
    XDEC_TRY_VOID(expect(TokenKind::Colon));
    XDEC_TRY(const TypeExpr type, parseType());
    XDEC_TRY(view.bits, constantWidth(type, "register view"));
    XDEC_TRY_VOID(expect(TokenKind::Assign));
    XDEC_TRY_VOID(expectKeyword("low"));
    XDEC_TRY(const uint64_t offset, expectInteger("a bit offset"));
    view.offset = static_cast<unsigned>(offset);
    if (view.offset + view.bits > file.bits) {
      return fail(loc, "register view reaches past the register it views");
    }
    while (at(TokenKind::Identifier)) {
      if (acceptKeyword("zeroext")) {
        view.zeroExtends = true;
      } else if (acceptKeyword("prefix")) {
        XDEC_TRY(const Token* token, expect(TokenKind::String));
        view.prefix = token->text;
      } else if (acceptKeyword("zero")) {
        XDEC_TRY(const Token* token, expect(TokenKind::String));
        view.zeroName = token->text;
      } else {
        break;
      }
    }
    if (view.prefix.empty()) {
      return fail(loc, "register view does not declare a name prefix");
    }
    return view;
  }

  Result<RegDecl> parseReg(SourceLoc loc) {
    RegDecl reg;
    reg.loc = loc;
    XDEC_TRY(reg.name, expectIdentifier("a register name"));
    XDEC_TRY_VOID(expect(TokenKind::Colon));
    XDEC_TRY(const TypeExpr type, parseType());
    if (type.kind == TypeKind::Flags) {
      reg.bits = 0;
      reg.role = RegRole::Flags;
    } else {
      XDEC_TRY(reg.bits, constantWidth(type, "register"));
    }
    if (acceptKeyword("role")) {
      XDEC_TRY(const std::string role, expectIdentifier("a register role"));
      if (!parseRegRole(role, reg.role)) {
        return fail(loc, std::format("unknown register role '{}'", role));
      }
    }
    if (type.kind == TypeKind::Flags && reg.role != RegRole::Flags) {
      return fail(loc, "a flags register must have role 'flags'");
    }
    return reg;
  }

  /// Register widths must be known when the spec is compiled: the register file
  /// is fixed hardware, not something an instruction field can size.
  Result<unsigned> constantWidth(const TypeExpr& type, std::string_view what) {
    if (type.kind != TypeKind::Bits && type.kind != TypeKind::Float) {
      return fail(type.loc, std::format("{} must be declared with bits(N) or float(N)", what));
    }
    if (type.width == nullptr || type.width->kind != ExprKind::Integer) {
      return fail(type.loc, std::format("{} width must be a literal", what));
    }
    const uint64_t bits = type.width->integer;
    if (bits == 0 || bits > 2048) {
      return fail(type.loc, std::format("{} width is out of range", what));
    }
    return static_cast<unsigned>(bits);
  }

  // -- functions ------------------------------------------------------------

  Result<FnDecl> parseFn() {
    FnDecl function;
    function.loc = peek().loc;
    XDEC_TRY_VOID(expectKeyword("fn"));
    XDEC_TRY(function.name, expectIdentifier("a function name"));
    XDEC_TRY_VOID(expect(TokenKind::LParen));

    while (!accept(TokenKind::RParen)) {
      ParamDecl param;
      param.loc = peek().loc;
      XDEC_TRY(param.name, expectIdentifier("a parameter name"));
      XDEC_TRY_VOID(expect(TokenKind::Colon));
      XDEC_TRY(param.type, parseType());
      function.params.push_back(std::move(param));
      if (!at(TokenKind::RParen)) {
        XDEC_TRY_VOID(expect(TokenKind::Comma));
      }
    }

    if (accept(TokenKind::Arrow)) {
      XDEC_TRY(function.result, parseType());
    } else {
      function.result.kind = TypeKind::Void;
      function.result.loc = function.loc;
    }
    XDEC_TRY(function.body, parseBlock());
    return function;
  }

  // -- instructions ---------------------------------------------------------

  Result<InsnDecl> parseInsn() {
    InsnDecl insn;
    insn.loc = peek().loc;
    XDEC_TRY_VOID(expectKeyword("insn"));
    XDEC_TRY(insn.name, expectIdentifier("an instruction name"));
    XDEC_TRY_VOID(expect(TokenKind::LBrace));

    bool sawEncoding = false;
    bool sawSemantics = false;

    while (!accept(TokenKind::RBrace)) {
      if (at(TokenKind::End)) {
        return fail(peek().loc, "unterminated insn block");
      }
      const SourceLoc loc = peek().loc;
      XDEC_TRY(const std::string keyword, expectIdentifier("an insn property"));

      if (keyword == "encoding") {
        if (sawEncoding) {
          return fail(loc, "insn declares more than one encoding");
        }
        XDEC_TRY(insn.encoding, parseEncoding(loc));
        sawEncoding = true;
      } else if (keyword == "asm") {
        XDEC_TRY(const Token* token, expect(TokenKind::String));
        XDEC_TRY(AsmTemplate tmpl, parseAsmTemplate(token->loc, token->text));
        insn.asmTemplate = std::move(tmpl);
      } else if (keyword == "require") {
        XDEC_TRY(ExprPtr condition, parseExpr());
        XDEC_TRY_VOID(expect(TokenKind::Semicolon));
        insn.requires_.push_back(std::move(condition));
      } else if (keyword == "priority") {
        bool negative = accept(TokenKind::Minus);
        XDEC_TRY(const uint64_t value, expectInteger("a priority"));
        insn.priority = negative ? -static_cast<int>(value) : static_cast<int>(value);
      } else if (keyword == "semantics") {
        XDEC_TRY(insn.semantics, parseBlock());
        sawSemantics = true;
      } else {
        return fail(loc, std::format("unknown insn property '{}'", keyword));
      }
    }

    if (!sawEncoding) {
      return fail(insn.loc, std::format("insn '{}' has no encoding", insn.name));
    }
    if (!sawSemantics) {
      return fail(insn.loc, std::format("insn '{}' has no semantics", insn.name));
    }
    return insn;
  }

  /// `encoding` runs until the next insn property. An item is either a quoted
  /// bit pattern or `name:width`, and no property keyword is followed by a
  /// colon, so two tokens of lookahead separate them unambiguously.
  Result<EncodingDecl> parseEncoding(SourceLoc loc) {
    EncodingDecl encoding;
    encoding.loc = loc;

    while (true) {
      if (at(TokenKind::String)) {
        const Token& token = advance();
        EncodingItem item;
        item.loc = token.loc;
        item.isLiteral = true;
        for (const char c : token.text) {
          if (c != '0' && c != '1') {
            return fail(item.loc, "a literal encoding field may only contain 0 and 1");
          }
          item.literal = (item.literal << 1) | static_cast<uint64_t>(c - '0');
          ++item.bits;
        }
        if (item.bits == 0) {
          return fail(item.loc, "empty literal encoding field");
        }
        encoding.items.push_back(std::move(item));
        continue;
      }

      if (at(TokenKind::Identifier) && peek(1).kind == TokenKind::Colon) {
        const Token& token = advance();
        advance();  // colon
        EncodingItem item;
        item.loc = token.loc;
        item.field = token.text;
        item.isWildcard = token.text == "_";
        XDEC_TRY(const uint64_t bits, expectInteger("a field width in bits"));
        if (bits == 0 || bits > 64) {
          return fail(item.loc, "field width must be between 1 and 64 bits");
        }
        item.bits = static_cast<unsigned>(bits);
        encoding.items.push_back(std::move(item));
        continue;
      }

      break;
    }

    if (encoding.items.empty()) {
      return fail(loc, "encoding declares no fields");
    }

    // Items are written most significant first, which matches how every
    // architecture manual draws them.
    unsigned width = 0;
    for (const EncodingItem& item : encoding.items) {
      width += item.bits;
    }
    if (width > 64) {
      return fail(loc, "encoding is wider than 64 bits");
    }
    encoding.width = width;

    unsigned offset = width;
    for (const EncodingItem& item : encoding.items) {
      offset -= item.bits;
      if (item.isLiteral) {
        const uint64_t fieldMask = item.bits == 64 ? ~uint64_t{0} : ((uint64_t{1} << item.bits) - 1);
        encoding.mask |= fieldMask << offset;
        encoding.value |= (item.literal & fieldMask) << offset;
      }
    }
    return encoding;
  }

  // -- asm templates --------------------------------------------------------

  /// The template is a string with `{expr}` substitutions and `[...]` groups
  /// that are printed only when their contents are non-default. Braces and
  /// brackets are escaped with a backslash.
  Result<AsmTemplate> parseAsmTemplate(SourceLoc loc, std::string_view text) {
    AsmTemplate tmpl;
    tmpl.loc = loc;
    std::size_t position = 0;
    XDEC_TRY(tmpl.pieces, parseAsmPieces(loc, text, position, /*insideGroup=*/false));
    return tmpl;
  }

  Result<std::vector<AsmPiecePtr>> parseAsmPieces(SourceLoc loc, std::string_view text,
                                                  std::size_t& position, bool insideGroup) {
    std::vector<AsmPiecePtr> pieces;
    std::string literal;

    const auto flush = [&] {
      if (literal.empty()) {
        return;
      }
      auto piece = std::make_unique<AsmPiece>();
      piece->kind = AsmPieceKind::Text;
      piece->loc = loc;
      piece->text = std::move(literal);
      literal.clear();
      pieces.push_back(std::move(piece));
    };

    while (position < text.size()) {
      const char c = text[position];
      if (c == '\\' && position + 1 < text.size()) {
        literal.push_back(text[position + 1]);
        position += 2;
        continue;
      }
      if (c == ']') {
        if (!insideGroup) {
          return fail(loc, "unmatched ']' in an asm template");
        }
        ++position;
        flush();
        return pieces;
      }
      if (c == '[') {
        ++position;
        flush();
        auto piece = std::make_unique<AsmPiece>();
        piece->kind = AsmPieceKind::OptionalGroup;
        piece->loc = loc;
        XDEC_TRY(piece->group, parseAsmPieces(loc, text, position, /*insideGroup=*/true));
        pieces.push_back(std::move(piece));
        continue;
      }
      if (c == '{') {
        const std::size_t close = text.find('}', position);
        if (close == std::string_view::npos) {
          return fail(loc, "unterminated '{' in an asm template");
        }
        flush();
        XDEC_TRY(AsmPiecePtr piece,
                 parseAsmSubstitution(loc, text.substr(position + 1, close - position - 1)));
        pieces.push_back(std::move(piece));
        position = close + 1;
        continue;
      }
      literal.push_back(c);
      ++position;
    }

    if (insideGroup) {
      return fail(loc, "unterminated '[' in an asm template");
    }
    flush();
    return pieces;
  }

  /// `Rd`, `Rd:gpr`, or `Rd:gpr(sf)`.
  Result<AsmPiecePtr> parseAsmSubstitution(SourceLoc loc, std::string_view body) {
    auto piece = std::make_unique<AsmPiece>();
    piece->kind = AsmPieceKind::Substitution;
    piece->loc = loc;

    // The style follows a colon, but a colon is not enough to find it: both the
    // value and the style's argument may contain a `?:` conditional. So the
    // separator is the one colon outside any parentheses that no `?` claimed,
    // which is unambiguous and lets either side hold a conditional.
    std::string_view valueText = body;
    std::string_view styleText;
    unsigned depth = 0;
    unsigned pendingConditionals = 0;
    for (std::size_t index = 0; index < body.size(); ++index) {
      switch (body[index]) {
        case '(':
          ++depth;
          break;
        case ')':
          if (depth != 0) {
            --depth;
          }
          break;
        case '?':
          if (depth == 0) {
            ++pendingConditionals;
          }
          break;
        case ':':
          if (depth != 0) {
            break;
          }
          if (pendingConditionals != 0) {
            --pendingConditionals;
            break;
          }
          valueText = body.substr(0, index);
          styleText = body.substr(index + 1);
          index = body.size();
          break;
        default:
          break;
      }
    }
    if (valueText.empty()) {
      return fail(loc, "empty substitution in an asm template");
    }

    XDEC_TRY(piece->value, parseSubExpression(loc, valueText));

    if (!styleText.empty()) {
      const std::size_t paren = styleText.find('(');
      if (paren == std::string_view::npos) {
        piece->style = std::string{styleText};
      } else {
        if (styleText.back() != ')') {
          return fail(loc, "unterminated style argument in an asm template");
        }
        piece->style = std::string{styleText.substr(0, paren)};
        const std::string_view argument =
            styleText.substr(paren + 1, styleText.size() - paren - 2);
        XDEC_TRY(piece->styleArgument, parseSubExpression(loc, argument));
      }
    }
    return piece;
  }

  Result<ExprPtr> parseSubExpression(SourceLoc loc, std::string_view text) {
    XDEC_TRY(std::vector<Token> tokens, tokenize(text, sourceName_));
    // Report against the template's location; a column inside the string would
    // be more precise but the string's own offsets are not tracked.
    for (Token& token : tokens) {
      token.loc = loc;
    }
    Parser sub{std::move(tokens), sourceName_};
    return sub.parseStandaloneExpression();
  }

  // -- statements -----------------------------------------------------------

  Result<std::vector<StmtPtr>> parseBlock() {
    XDEC_TRY_VOID(expect(TokenKind::LBrace));
    std::vector<StmtPtr> body;
    while (!accept(TokenKind::RBrace)) {
      if (at(TokenKind::End)) {
        return fail(peek().loc, "unterminated block");
      }
      XDEC_TRY(StmtPtr statement, parseStmt());
      body.push_back(std::move(statement));
    }
    return body;
  }

  Result<StmtPtr> parseStmt() {
    const SourceLoc loc = peek().loc;

    if (acceptKeyword("let")) {
      auto statement = std::make_unique<Stmt>();
      statement->kind = StmtKind::Let;
      statement->loc = loc;
      XDEC_TRY(statement->name, expectIdentifier("a binding name"));
      XDEC_TRY_VOID(expect(TokenKind::Assign));
      XDEC_TRY(statement->value, parseExpr());
      XDEC_TRY_VOID(expect(TokenKind::Semicolon));
      return statement;
    }

    if (acceptKeyword("if")) {
      auto statement = std::make_unique<Stmt>();
      statement->kind = StmtKind::If;
      statement->loc = loc;
      XDEC_TRY(statement->value, parseExpr());
      XDEC_TRY(statement->thenBody, parseBlock());
      if (acceptKeyword("else")) {
        if (atKeyword("if")) {
          XDEC_TRY(StmtPtr nested, parseStmt());
          statement->elseBody.push_back(std::move(nested));
        } else {
          XDEC_TRY(statement->elseBody, parseBlock());
        }
      }
      return statement;
    }

    if (acceptKeyword("return")) {
      auto statement = std::make_unique<Stmt>();
      statement->kind = StmtKind::Return;
      statement->loc = loc;
      if (!at(TokenKind::Semicolon)) {
        XDEC_TRY(statement->value, parseExpr());
      }
      XDEC_TRY_VOID(expect(TokenKind::Semicolon));
      return statement;
    }

    XDEC_TRY(ExprPtr first, parseExpr());

    if (accept(TokenKind::Assign)) {
      auto statement = std::make_unique<Stmt>();
      statement->kind = StmtKind::Assign;
      statement->loc = loc;
      statement->target = std::move(first);
      XDEC_TRY(statement->value, parseExpr());
      XDEC_TRY_VOID(expect(TokenKind::Semicolon));
      return statement;
    }

    auto statement = std::make_unique<Stmt>();
    statement->kind = StmtKind::Effect;
    statement->loc = loc;
    statement->value = std::move(first);
    XDEC_TRY_VOID(expect(TokenKind::Semicolon));
    return statement;
  }

  // -- expressions ----------------------------------------------------------

  Result<ExprPtr> parseExpr() { return parseConditional(); }

  Result<ExprPtr> parseConditional() {
    XDEC_TRY(ExprPtr condition, parseBinary(1));
    if (!accept(TokenKind::Question)) {
      return condition;
    }
    auto expr = std::make_unique<Expr>();
    expr->kind = ExprKind::Conditional;
    expr->loc = condition->loc;
    expr->args.push_back(std::move(condition));
    XDEC_TRY(ExprPtr ifTrue, parseExpr());
    XDEC_TRY_VOID(expect(TokenKind::Colon));
    XDEC_TRY(ExprPtr ifFalse, parseExpr());
    expr->args.push_back(std::move(ifTrue));
    expr->args.push_back(std::move(ifFalse));
    return expr;
  }

  Result<ExprPtr> parseBinary(unsigned minPrecedence) {
    XDEC_TRY(ExprPtr left, parseUnary());
    while (true) {
      const unsigned precedence = precedenceOf(peek().kind);
      if (precedence == 0 || precedence < minPrecedence) {
        return left;
      }
      const Token& token = advance();
      XDEC_TRY(ExprPtr right, parseBinary(precedence + 1));
      auto expr = std::make_unique<Expr>();
      expr->kind = ExprKind::Binary;
      expr->loc = token.loc;
      expr->binaryOp = binaryOpOf(token.kind);
      expr->args.push_back(std::move(left));
      expr->args.push_back(std::move(right));
      left = std::move(expr);
    }
  }

  Result<ExprPtr> parseUnary() {
    const SourceLoc loc = peek().loc;
    UnaryOp op = UnaryOp::Negate;
    bool isUnary = true;
    if (accept(TokenKind::Minus)) {
      op = UnaryOp::Negate;
    } else if (accept(TokenKind::Tilde)) {
      op = UnaryOp::BitNot;
    } else if (accept(TokenKind::Bang)) {
      op = UnaryOp::LogicalNot;
    } else {
      isUnary = false;
    }

    if (!isUnary) {
      return parsePostfix();
    }

    XDEC_TRY(ExprPtr operand, parseUnary());
    auto expr = std::make_unique<Expr>();
    expr->kind = ExprKind::Unary;
    expr->loc = loc;
    expr->unaryOp = op;
    expr->args.push_back(std::move(operand));
    return expr;
  }

  Result<ExprPtr> parsePostfix() {
    XDEC_TRY(ExprPtr expr, parsePrimary());
    while (true) {
      const SourceLoc loc = peek().loc;
      if (accept(TokenKind::LParen)) {
        if (expr->kind != ExprKind::Name) {
          return fail(loc, "only a named function can be called");
        }
        auto call = std::make_unique<Expr>();
        call->kind = ExprKind::Call;
        call->loc = expr->loc;
        call->name = expr->name;
        while (!accept(TokenKind::RParen)) {
          XDEC_TRY(ExprPtr argument, parseExpr());
          call->args.push_back(std::move(argument));
          if (!at(TokenKind::RParen)) {
            XDEC_TRY_VOID(expect(TokenKind::Comma));
          }
        }
        expr = std::move(call);
        continue;
      }
      if (accept(TokenKind::LBracket)) {
        auto index = std::make_unique<Expr>();
        index->kind = ExprKind::Index;
        index->loc = loc;
        index->args.push_back(std::move(expr));
        XDEC_TRY(ExprPtr subscript, parseExpr());
        index->args.push_back(std::move(subscript));
        XDEC_TRY_VOID(expect(TokenKind::RBracket));
        expr = std::move(index);
        continue;
      }
      if (accept(TokenKind::Dot)) {
        auto member = std::make_unique<Expr>();
        member->kind = ExprKind::Member;
        member->loc = loc;
        XDEC_TRY(member->name, expectIdentifier("a member name"));
        member->args.push_back(std::move(expr));
        expr = std::move(member);
        continue;
      }
      return expr;
    }
  }

  Result<ExprPtr> parsePrimary() {
    const Token& token = peek();
    if (token.kind == TokenKind::Integer) {
      advance();
      return Expr::makeInteger(token.loc, token.integer);
    }
    if (token.kind == TokenKind::Identifier) {
      advance();
      return Expr::makeName(token.loc, token.text);
    }
    if (token.kind == TokenKind::String) {
      advance();
      auto expr = std::make_unique<Expr>();
      expr->kind = ExprKind::String;
      expr->loc = token.loc;
      expr->name = token.text;
      return expr;
    }
    if (accept(TokenKind::LParen)) {
      XDEC_TRY(ExprPtr expr, parseExpr());
      XDEC_TRY_VOID(expect(TokenKind::RParen));
      return expr;
    }
    return fail(token.loc, std::format("expected an expression but found {}", describe(token)));
  }

  std::vector<Token> tokens_;
  std::string_view sourceName_;
  std::size_t position_ = 0;
};

}  // namespace

Result<std::unique_ptr<Module>> parseModule(std::string_view text, std::string_view sourceName) {
  XDEC_TRY(std::vector<Token> tokens, tokenize(text, sourceName));
  Parser parser{std::move(tokens), sourceName};
  return parser.parseModule();
}

Result<std::unique_ptr<Module>> parseFragment(std::string_view text,
                                             std::string_view sourceName) {
  XDEC_TRY(std::vector<Token> tokens, tokenize(text, sourceName));
  Parser parser{std::move(tokens), sourceName};
  return parser.parseFragment();
}

Result<ExprPtr> parseExpression(std::string_view text, std::string_view sourceName) {
  XDEC_TRY(std::vector<Token> tokens, tokenize(text, sourceName));
  Parser parser{std::move(tokens), sourceName};
  return parser.parseStandaloneExpression();
}

namespace {

Result<std::string> readWholeFile(const std::filesystem::path& path) {
  std::ifstream file{path, std::ios::binary};
  if (!file) {
    return err(Diag{DiagCode::IoError, std::format("cannot open spec '{}'", path.string())});
  }
  std::ostringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

/// Reads one file and everything below it into `out`.
///
/// `visiting` is the include stack, so a cycle is reported as a cycle rather
/// than running until the stack is gone. `seen` makes a file included twice --
/// two groups sharing a helper file, say -- read once instead of colliding with
/// itself on every duplicate declaration.
Result<void> loadInto(const std::filesystem::path& path, bool isRoot, Module& out,
                      std::vector<std::filesystem::path>& visiting,
                      std::set<std::filesystem::path>& seen) {
  std::error_code ec;
  std::filesystem::path key = std::filesystem::weakly_canonical(path, ec);
  if (ec) {
    key = path;
  }
  if (std::ranges::find(visiting, key) != visiting.end()) {
    return err(Diag{DiagCode::ParseError,
                    std::format("spec '{}' includes itself", path.filename().string())});
  }
  if (!seen.insert(key).second) {
    return ok();
  }
  visiting.push_back(key);

  XDEC_TRY(const std::string text, readWholeFile(path));
  const std::string name = path.filename().string();
  XDEC_TRY(std::unique_ptr<Module> parsed,
           isRoot ? parseModule(text, name) : parseFragment(text, name));

  if (isRoot) {
    out.sourceName = parsed->sourceName;
    out.arch = std::move(parsed->arch);
  }
  for (const IncludeDecl& include : parsed->includes) {
    XDEC_TRY_VOID(loadInto(path.parent_path() / include.path, /*isRoot=*/false, out, visiting,
                           seen));
  }
  std::ranges::move(parsed->functions, std::back_inserter(out.functions));
  std::ranges::move(parsed->instructions, std::back_inserter(out.instructions));

  visiting.pop_back();
  return ok();
}

}  // namespace

Result<std::unique_ptr<Module>> parseSpecFile(const std::filesystem::path& path) {
  auto module = std::make_unique<Module>();
  std::vector<std::filesystem::path> visiting;
  std::set<std::filesystem::path> seen;
  XDEC_TRY_VOID(loadInto(path, /*isRoot=*/true, *module, visiting, seen));
  return module;
}

}  // namespace xdec::spec
