#include "xdec/il/parser.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <unordered_map>
#include <vector>

#include "xdec/support/bits.h"

namespace xdec::il {
namespace {

[[nodiscard]] bool isIdentChar(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '.';
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

/// One logical line of IL, with its source line number for diagnostics.
struct Line {
  std::string_view text;
  unsigned number = 0;
};

/// Where a line's comment begins, or its end if it has none. `;` starts a
/// comment — that is what lets a dump carry disassembly the parser need not
/// understand — but only outside a quoted string, because a note or a mnemonic
/// is free to contain one and truncating there would silently corrupt it.
std::size_t commentStart(std::string_view line) {
  bool quoted = false;
  for (std::size_t at = 0; at < line.size(); ++at) {
    if (quoted && line[at] == '\\') {
      ++at;  // an escaped character, quote included, is never a delimiter
      continue;
    }
    if (line[at] == '"') {
      quoted = !quoted;
    } else if (line[at] == ';' && !quoted) {
      return at;
    }
  }
  return line.size();
}

std::vector<Line> splitLines(std::string_view text) {
  std::vector<Line> lines;
  unsigned number = 0;
  std::size_t position = 0;
  while (position <= text.size()) {
    const std::size_t newline = text.find('\n', position);
    const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
    ++number;
    std::string_view line = trim(text.substr(position, end - position));
    line = trim(line.substr(0, commentStart(line)));
    if (!line.empty()) {
      lines.push_back(Line{line, number});
    }
    if (newline == std::string_view::npos) {
      break;
    }
    position = newline + 1;
  }
  return lines;
}

/// A cursor over one line, used for the fiddly parts (expressions, operands).
class Cursor {
 public:
  explicit Cursor(std::string_view text) noexcept : text_(text) {}

  void skipSpace() noexcept {
    while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\t')) {
      ++position_;
    }
  }

  [[nodiscard]] bool atEnd() noexcept {
    skipSpace();
    return position_ >= text_.size();
  }

  [[nodiscard]] char peek() const noexcept {
    return position_ < text_.size() ? text_[position_] : '\0';
  }

  bool consume(char expected) noexcept {
    skipSpace();
    if (peek() != expected) {
      return false;
    }
    ++position_;
    return true;
  }

  bool consumeWord(std::string_view word) noexcept {
    skipSpace();
    if (text_.compare(position_, word.size(), word) != 0) {
      return false;
    }
    const std::size_t after = position_ + word.size();
    if (after < text_.size() && isIdentChar(text_[after])) {
      return false;
    }
    position_ = after;
    return true;
  }

  std::string_view takeIdent() noexcept {
    skipSpace();
    const std::size_t start = position_;
    while (position_ < text_.size() && isIdentChar(text_[position_])) {
      ++position_;
    }
    return text_.substr(start, position_ - start);
  }

  /// Identifier stopping before `:` or `(`; used for expression op names, which
  /// contain dots but never colons.
  std::string_view takeOpName() noexcept {
    skipSpace();
    const std::size_t start = position_;
    while (position_ < text_.size() && isIdentChar(text_[position_])) {
      ++position_;
    }
    return text_.substr(start, position_ - start);
  }

  std::string_view takeQuoted() noexcept {
    skipSpace();
    if (peek() != '"') {
      return {};
    }
    ++position_;
    const std::size_t start = position_;
    while (position_ < text_.size() && text_[position_] != '"') {
      ++position_;
    }
    const std::string_view result = text_.substr(start, position_ - start);
    if (position_ < text_.size()) {
      ++position_;
    }
    return result;
  }

  /// A quoted string with backslash escapes, for text the printer had to escape
  /// (annotation notes). Unlike takeQuoted, which serves interned names that
  /// cannot contain a quote, this one has to survive arbitrary prose.
  bool takeEscapedString(std::string& out) {
    skipSpace();
    if (peek() != '"') {
      return false;
    }
    ++position_;
    while (position_ < text_.size() && text_[position_] != '"') {
      if (text_[position_] == '\\' && position_ + 1 < text_.size()) {
        ++position_;
      }
      out += text_[position_];
      ++position_;
    }
    if (position_ >= text_.size()) {
      return false;  // unterminated: the line ended inside the string
    }
    ++position_;
    return true;
  }

  /// Signed or unsigned, decimal or 0x-prefixed. Negative values are returned
  /// as their two's complement, matching how the printer renders them.
  bool takeInteger(uint64_t& out) noexcept {
    skipSpace();
    bool negative = false;
    if (peek() == '-') {
      negative = true;
      ++position_;
    }
    int base = 10;
    if (text_.compare(position_, 2, "0x") == 0 || text_.compare(position_, 2, "0X") == 0) {
      position_ += 2;
      base = 16;
    }
    uint64_t magnitude = 0;
    const char* begin = text_.data() + position_;
    const char* end = text_.data() + text_.size();
    const auto result = std::from_chars(begin, end, magnitude, base);
    if (result.ec != std::errc{}) {
      return false;
    }
    position_ = static_cast<std::size_t>(result.ptr - text_.data());
    out = negative ? ~magnitude + 1 : magnitude;
    return true;
  }

  [[nodiscard]] std::string_view rest() const noexcept { return text_.substr(position_); }
  [[nodiscard]] std::size_t position() const noexcept { return position_; }

  void advance(std::size_t count) noexcept {
    position_ = std::min(position_ + count, text_.size());
  }

 private:
  std::string_view text_;
  std::size_t position_ = 0;
};

class Parser {
 public:
  Parser(std::string_view text, const RegisterFile& registers)
      : lines_(splitLines(text)), registers_(&registers) {}

  Result<std::unique_ptr<Function>> run() {
    XDEC_TRY_VOID(parseHeader());
    collectBlocks();
    XDEC_TRY_VOID(parseBodies());
    XDEC_TRY_VOID(resolveDeferredOperands());
    function_->rebuildEdges();
    return std::move(function_);
  }

 private:
  [[nodiscard]] Unexpected fail(unsigned line, std::string message) const {
    return err(Diag{DiagCode::ParseError, std::move(message)}.note(std::format("line {}", line)));
  }

  Result<void> parseHeader() {
    if (lines_.empty()) {
      return err(DiagCode::ParseError, "empty IL text");
    }
    Cursor cursor{lines_[0].text};
    if (!cursor.consumeWord("function")) {
      return fail(lines_[0].number, "expected 'function'");
    }
    if (!cursor.consume('@')) {
      return fail(lines_[0].number, "expected '@' before the entry address");
    }
    uint64_t entryVa = 0;
    if (!cursor.takeInteger(entryVa)) {
      return fail(lines_[0].number, "expected an entry address");
    }

    std::string name;
    Arch arch = Arch::Unknown;
    Maturity maturity = Maturity::Lifted;

    while (!cursor.atEnd()) {
      if (cursor.consume('{')) {
        break;
      }
      const std::string_view key = cursor.takeIdent();
      if (key.empty()) {
        return fail(lines_[0].number,
                    std::format("unexpected text in function header: '{}'", cursor.rest()));
      }
      if (!cursor.consume('=')) {
        return fail(lines_[0].number, std::format("expected '=' after '{}'", key));
      }
      if (key == "name") {
        name = std::string{cursor.takeQuoted()};
      } else if (key == "arch") {
        if (!parseArch(cursor.takeIdent(), arch)) {
          return fail(lines_[0].number, "unknown architecture");
        }
      } else if (key == "maturity") {
        if (!parseMaturity(cursor.takeIdent(), maturity)) {
          return fail(lines_[0].number, "unknown maturity level");
        }
      } else {
        return fail(lines_[0].number, std::format("unknown function attribute '{}'", key));
      }
    }

    function_ = std::make_unique<Function>(arch, *registers_, entryVa);
    function_->setName(std::move(name));
    pendingMaturity_ = maturity;
    return ok();
  }

  /// Creates every block before any body is parsed, because branches routinely
  /// refer forward.
  void collectBlocks() {
    struct Header {
      uint32_t index;
      uint64_t va;
      uint64_t endVa;
      bool isEntry;
      bool isExternal;
    };
    std::vector<Header> headers;

    for (const Line& line : lines_) {
      Cursor cursor{line.text};
      if (!cursor.consumeWord("block")) {
        continue;
      }
      if (!cursor.consume('b')) {
        continue;
      }
      uint64_t index = 0;
      if (!cursor.takeInteger(index)) {
        continue;
      }
      uint64_t va = 0;
      uint64_t endVa = 0;
      if (cursor.consume('@')) {
        (void)cursor.takeInteger(va);
        if (cursor.consume('.') && cursor.consume('.')) {
          (void)cursor.takeInteger(endVa);
        }
      }
      const bool isEntry = line.text.find(" entry") != std::string_view::npos;
      const bool isExternal = line.text.find(" external") != std::string_view::npos;
      headers.push_back(Header{static_cast<uint32_t>(index), va, endVa, isEntry, isExternal});
    }

    // Create in index order so that the textual label bN matches BlockId N,
    // which keeps dumps stable across a print/parse cycle.
    std::sort(headers.begin(), headers.end(),
              [](const Header& a, const Header& b) { return a.index < b.index; });

    for (const Header& header : headers) {
      const BlockId id = function_->createBlock(header.va);
      function_->block(id).endVa = header.endVa;
      function_->block(id).external = header.isExternal;
      blockByLabel_.emplace(header.index, id);
      if (header.isEntry) {
        function_->setEntryBlock(id);
      }
    }
  }

  Result<void> parseBodies() {
    BlockId current;
    uint64_t currentAddress = kNoOpAddress;

    for (std::size_t index = 1; index < lines_.size(); ++index) {
      const Line& line = lines_[index];
      Cursor cursor{line.text};

      if (cursor.consumeWord("block")) {
        if (!cursor.consume('b')) {
          return fail(line.number, "expected a block label");
        }
        uint64_t label = 0;
        if (!cursor.takeInteger(label)) {
          return fail(line.number, "expected a block index");
        }
        const auto found = blockByLabel_.find(static_cast<uint32_t>(label));
        if (found == blockByLabel_.end()) {
          return fail(line.number, std::format("unknown block b{}", label));
        }
        current = found->second;
        currentAddress = kNoOpAddress;
        continue;
      }

      if (line.text == "}") {
        current = BlockId::invalid();
        continue;
      }

      if (cursor.peek() == '@') {
        (void)cursor.consume('@');
        if (cursor.consumeWord("none")) {
          currentAddress = kNoOpAddress;
        } else if (!cursor.takeInteger(currentAddress)) {
          return fail(line.number, "expected an address after '@'");
        }
        continue;
      }

      if (!current.valid()) {
        return fail(line.number, "operation outside any block");
      }
      XDEC_TRY_VOID(parseOp(line, cursor, current, currentAddress));
    }

    function_->setMaturity(pendingMaturity_);
    return ok();
  }

  Result<void> parseOp(const Line& line, Cursor& cursor, BlockId block, uint64_t address) {
    // An optional `%N =` result binding.
    bool hasResultLabel = false;
    uint64_t resultLabel = 0;
    if (cursor.peek() == '%') {
      (void)cursor.consume('%');
      if (!cursor.takeInteger(resultLabel)) {
        return fail(line.number, "expected a value index after '%'");
      }
      if (!cursor.consume('=')) {
        return fail(line.number, "expected '=' after a result label");
      }
      hasResultLabel = true;
    }

    const std::string_view mnemonic = cursor.takeOpName();
    OpCode code = OpCode::Nop;
    if (!parseOpCode(mnemonic, code)) {
      return fail(line.number, std::format("unknown operation '{}'", mnemonic));
    }

    Type type = Type::voidType();
    if (cursor.peek() == ':') {
      (void)cursor.consume(':');
      const std::string_view spelling = cursor.takeIdent();
      if (!Type::parse(spelling, type)) {
        return fail(line.number, std::format("unknown type '{}'", spelling));
      }
    }

    ValueId defined;
    OpId opId;

    switch (code) {
      case OpCode::ReadReg: {
        XDEC_TRY(const RegId reg, parseRegister(line, cursor));
        defined = function_->appendReadReg(block, address, reg);
        opId = function_->value(defined).definition;
        break;
      }
      case OpCode::WriteReg: {
        XDEC_TRY(const RegId reg, parseRegister(line, cursor));
        if (!cursor.consume(',')) {
          return fail(line.number, "expected ',' after the register name");
        }
        XDEC_TRY(const ExprId value, parseExpr(line, cursor));
        opId = function_->appendWriteReg(block, address, reg, value);
        break;
      }
      case OpCode::Load: {
        XDEC_TRY(const ExprId addressExpr, parseExpr(line, cursor));
        defined = function_->appendLoad(block, address, type, addressExpr);
        opId = function_->value(defined).definition;
        break;
      }
      case OpCode::Store: {
        XDEC_TRY(const ExprId addressExpr, parseExpr(line, cursor));
        if (!cursor.consume(',')) {
          return fail(line.number, "expected ',' after the store address");
        }
        XDEC_TRY(const ExprId value, parseExpr(line, cursor));
        opId = function_->appendStore(block, address, type, addressExpr, value);
        break;
      }
      case OpCode::Branch: {
        XDEC_TRY(const BlockId target, parseBlockRef(line, cursor));
        opId = function_->appendBranch(block, address, target);
        break;
      }
      case OpCode::CondBranch: {
        XDEC_TRY(const ExprId condition, parseExpr(line, cursor));
        if (!cursor.consume(',')) {
          return fail(line.number, "expected ',' after the branch condition");
        }
        XDEC_TRY(const BlockId ifTrue, parseBlockRef(line, cursor));
        if (!cursor.consume(',')) {
          return fail(line.number, "expected ',' between branch targets");
        }
        XDEC_TRY(const BlockId ifFalse, parseBlockRef(line, cursor));
        opId = function_->appendCondBranch(block, address, condition, ifTrue, ifFalse);
        break;
      }
      case OpCode::IndirectBranch: {
        XDEC_TRY(const ExprId target, parseExpr(line, cursor));
        opId = function_->appendIndirectBranch(block, address, target);
        // `-> unresolved` and `-> [b1, b2]` are both meaningful; the first says
        // the destination is genuinely not known yet.
        if (cursor.consume('-')) {
          if (!cursor.consume('>')) {
            return fail(line.number, "expected '->' after an indirect branch target");
          }
          if (!cursor.consumeWord("unresolved")) {
            if (!cursor.consume('[')) {
              return fail(line.number, "expected '[' or 'unresolved'");
            }
            std::vector<BlockId> resolved;
            if (!cursor.consume(']')) {
              while (true) {
                XDEC_TRY(const BlockId target2, parseBlockRef(line, cursor));
                resolved.push_back(target2);
                if (cursor.consume(']')) {
                  break;
                }
                if (!cursor.consume(',')) {
                  return fail(line.number, "expected ',' or ']' in the target list");
                }
              }
            }
            function_->setTargets(opId, resolved);
          }
        }
        break;
      }
      case OpCode::Call: {
        XDEC_TRY(const ExprId target, parseExpr(line, cursor));
        // `call:i64` carries the result-register type; a bare `call` defines
        // nothing (the pre-SSA shape, and text from before results existed).
        opId = function_->appendCall(block, address, target, type);
        defined = function_->op(opId).result;
        // Optional SSA-level annotation: `call target(arg, ...)`.
        if (cursor.consume('(')) {
          std::vector<il::ExprId> operands{target};
          if (!cursor.consume(')')) {
            while (true) {
              XDEC_TRY(const ExprId argument, parseExpr(line, cursor));
              operands.push_back(argument);
              if (cursor.consume(')')) {
                break;
              }
              if (!cursor.consume(',')) {
                return fail(line.number, "expected ',' or ')' in a call's arguments");
              }
            }
          }
          function_->setOperands(opId, operands);
        }
        break;
      }
      case OpCode::Return: {
        opId = function_->appendReturn(block, address);
        // Optional SSA-level annotation: `return value`.
        if (!cursor.atEnd()) {
          XDEC_TRY(const ExprId value, parseExpr(line, cursor));
          function_->setOperands(opId, std::span<const ExprId>{&value, 1});
        }
        break;
      }
      case OpCode::Nop:
        opId = function_->appendNop(block, address);
        break;
      case OpCode::Unreachable:
        opId = function_->appendUnreachable(block, address);
        break;
      case OpCode::Unimplemented:
        opId = function_->appendUnimplemented(block, address, cursor.takeQuoted());
        break;
      case OpCode::Intrinsic: {
        const std::string_view name = cursor.takeQuoted();
        if (!cursor.consume('(')) {
          return fail(line.number, "expected '(' after an intrinsic name");
        }
        std::vector<ExprId> arguments;
        if (!cursor.consume(')')) {
          while (true) {
            XDEC_TRY(const ExprId argument, parseExpr(line, cursor));
            arguments.push_back(argument);
            if (cursor.consume(')')) {
              break;
            }
            if (!cursor.consume(',')) {
              return fail(line.number, "expected ',' or ')' in the argument list");
            }
          }
        }
        opId = function_->appendIntrinsic(block, address, name, type, arguments);
        defined = function_->op(opId).result;
        break;
      }
      case OpCode::Phi: {
        if (!cursor.consume('(')) {
          return fail(line.number, "expected '(' after phi");
        }
        // Phi operands may name values defined in blocks not yet parsed, so the
        // operand text is set aside and resolved once every block has been read.
        int depth = 1;
        const std::string_view remaining = cursor.rest();
        std::size_t consumed = 0;
        for (; consumed < remaining.size() && depth > 0; ++consumed) {
          if (remaining[consumed] == '(') {
            ++depth;
          } else if (remaining[consumed] == ')') {
            --depth;
          }
        }
        if (depth != 0) {
          return fail(line.number, "unbalanced parentheses in phi operands");
        }
        opId = function_->appendPhi(block, address, type, {});
        defined = function_->op(opId).result;
        deferred_.push_back(
            Deferred{opId, std::string{remaining.substr(0, consumed - 1)}, line.number});
        // Step over the operand text so that a trailing `!from(...)` is still
        // seen by the annotation parser below.
        cursor.advance(consumed);
        break;
      }
      case OpCode::Count:
        return fail(line.number, "invalid operation");
    }

    if (hasResultLabel) {
      if (!defined.valid()) {
        return fail(line.number,
                    std::format("'{}' does not define a value but a result label was given",
                                mnemonic));
      }
      valueByLabel_[resultLabel] = defined;
    } else if (defined.valid()) {
      return fail(line.number, std::format("'{}' defines a value but no label was given",
                                           mnemonic));
    }

    // Trailing annotations, e.g. `!from(deflatten) !note("...")`. A loop rather
    // than a chain of ifs so the order they are written in does not matter:
    // nothing about an op depends on which annotation comes first.
    while (cursor.consume('!')) {
      const std::string_view keyword = cursor.takeIdent();
      if (!cursor.consume('(')) {
        return fail(line.number, std::format("expected '(' after '!{}'", keyword));
      }
      if (keyword == "from") {
        const std::string_view passName = cursor.takeIdent();
        if (!cursor.consume(')')) {
          return fail(line.number, "expected ')' after a pass name");
        }
        function_->op(opId).origin = function_->internPass(passName);
      } else if (keyword == "note") {
        std::string note;
        if (!cursor.takeEscapedString(note)) {
          return fail(line.number, "expected a quoted note");
        }
        if (!cursor.consume(')')) {
          return fail(line.number, "expected ')' after a note");
        }
        function_->annotate(opId, std::move(note));
      } else {
        return fail(line.number, std::format("unknown annotation '!{}'", keyword));
      }
    }

    return ok();
  }

  Result<RegId> parseRegister(const Line& line, Cursor& cursor) {
    const std::string_view name = cursor.takeIdent();
    const RegId reg = registers_->find(name);
    if (!reg.valid()) {
      // Inventing a register here would let a typo silently become a new
      // storage location that no pass knows about.
      return fail(line.number, std::format("unknown register '{}'", name));
    }
    return reg;
  }

  Result<BlockId> parseBlockRef(const Line& line, Cursor& cursor) {
    cursor.skipSpace();
    if (!cursor.consume('b')) {
      return fail(line.number, "expected a block label");
    }
    uint64_t label = 0;
    if (!cursor.takeInteger(label)) {
      return fail(line.number, "expected a block index");
    }
    const auto found = blockByLabel_.find(static_cast<uint32_t>(label));
    if (found == blockByLabel_.end()) {
      return fail(line.number, std::format("unknown block b{}", label));
    }
    return found->second;
  }

  Result<ExprId> parseExpr(const Line& line, Cursor& cursor) {
    const std::string_view name = cursor.takeOpName();
    if (name.empty()) {
      return fail(line.number, std::format("expected an expression at '{}'", cursor.rest()));
    }
    ExprOp op = ExprOp::Undef;
    if (!parseExprOp(name, op)) {
      return fail(line.number, std::format("unknown expression op '{}'", name));
    }

    std::string_view modifier;
    if (cursor.consume(':')) {
      modifier = cursor.takeIdent();
    }

    if (!cursor.consume('(')) {
      return fail(line.number, std::format("expected '(' after '{}'", name));
    }

    switch (op) {
      case ExprOp::Const: {
        XDEC_TRY(const Type type, requireType(line, modifier));
        uint64_t value = 0;
        if (!cursor.takeInteger(value)) {
          return fail(line.number, "expected a constant value");
        }
        if (!cursor.consume(')')) {
          return fail(line.number, "expected ')' after a constant");
        }
        return function_->constant(type, value);
      }
      case ExprOp::Value: {
        if (!cursor.consume('%')) {
          return fail(line.number, "expected '%' in a value reference");
        }
        uint64_t label = 0;
        if (!cursor.takeInteger(label)) {
          return fail(line.number, "expected a value index");
        }
        if (!cursor.consume(')')) {
          return fail(line.number, "expected ')' after a value reference");
        }
        const auto found = valueByLabel_.find(label);
        if (found == valueByLabel_.end()) {
          return fail(line.number, std::format("value %{} is not defined", label));
        }
        return function_->valueRef(found->second);
      }
      case ExprOp::Undef: {
        XDEC_TRY(const Type type, requireType(line, modifier));
        if (!cursor.consume(')')) {
          return fail(line.number, "expected ')' after undef");
        }
        return function_->undefined(type);
      }
      case ExprOp::EntryReg: {
        // The printed type modifier is checked for presence; the register
        // file is the source of truth for the leaf's actual type.
        XDEC_TRY(const Type type, requireType(line, modifier));
        (void)type;
        const std::string_view regName = cursor.takeIdent();
        const RegId reg = function_->registers().find(regName);
        if (!reg.valid()) {
          return fail(line.number, std::format("unknown register '{}'", regName));
        }
        if (!cursor.consume(')')) {
          return fail(line.number, "expected ')' after an entry register");
        }
        return function_->entryReg(reg);
      }
      case ExprOp::Extract: {
        XDEC_TRY(const Type type, requireType(line, modifier));
        XDEC_TRY(const ExprId operand, parseExpr(line, cursor));
        if (!cursor.consume(',')) {
          return fail(line.number, "expected ',' before the extract offset");
        }
        uint64_t lowBit = 0;
        if (!cursor.takeInteger(lowBit)) {
          return fail(line.number, "expected an extract offset");
        }
        if (!cursor.consume(')')) {
          return fail(line.number, "expected ')' after extract");
        }
        return function_->extract(type, operand, static_cast<unsigned>(lowBit));
      }
      case ExprOp::FlagDef: {
        // The modifier is `<flagop>.<width>`, e.g. `sub.64`.
        const std::size_t dot = modifier.rfind('.');
        if (dot == std::string_view::npos) {
          return fail(line.number, "flagdef needs a '<op>.<width>' modifier");
        }
        FlagOp flagOp = FlagOp::Add;
        if (!parseFlagOp(modifier.substr(0, dot), flagOp)) {
          return fail(line.number,
                      std::format("unknown flag op '{}'", modifier.substr(0, dot)));
        }
        unsigned width = 0;
        {
          const std::string_view digits = modifier.substr(dot + 1);
          const auto* end = digits.data() + digits.size();
          if (std::from_chars(digits.data(), end, width, 10).ec != std::errc{}) {
            return fail(line.number, "flagdef width is not a number");
          }
        }
        XDEC_TRY(std::vector<ExprId> operands, parseExprList(line, cursor));
        return function_->flagDef(flagOp, width, operands);
      }
      case ExprOp::FlagCond: {
        ConditionCode condition = ConditionCode::Always;
        if (!parseConditionCode(modifier, condition)) {
          return fail(line.number, std::format("unknown condition code '{}'", modifier));
        }
        XDEC_TRY(const ExprId flags, parseExpr(line, cursor));
        if (!cursor.consume(')')) {
          return fail(line.number, "expected ')' after flagcond");
        }
        return function_->flagCondition(flags, condition);
      }
      case ExprOp::FlagBit: {
        FlagBitIndex bit = FlagBitIndex::Zero;
        if (!parseFlagBit(modifier, bit)) {
          return fail(line.number, std::format("unknown flag bit '{}'", modifier));
        }
        XDEC_TRY(const ExprId flags, parseExpr(line, cursor));
        if (!cursor.consume(')')) {
          return fail(line.number, "expected ')' after flagbit");
        }
        return function_->flagBitOf(flags, bit);
      }
      default:
        break;
    }

    XDEC_TRY(const Type type, requireType(line, modifier));
    XDEC_TRY(std::vector<ExprId> operands, parseExprList(line, cursor));

    const ExprOpInfo& opInfo = info(op);
    if (operands.size() < opInfo.minArity || operands.size() > opInfo.maxArity) {
      return fail(line.number,
                  std::format("'{}' takes {}..{} operands but got {}", name, opInfo.minArity,
                              opInfo.maxArity, operands.size()));
    }

    // Reconstruct through the same builders the lifter uses, so that interning
    // and type derivation behave identically no matter where IL comes from.
    switch (op) {
      case ExprOp::Select:
        return function_->select(operands[0], operands[1], operands[2]);
      case ExprOp::Concat:
        return function_->concat(type, operands[0], operands[1]);
      case ExprOp::ZExt:
      case ExprOp::SExt:
      case ExprOp::Trunc:
      case ExprOp::Bitcast:
      case ExprOp::FpConvert:
      case ExprOp::FpToIntS:
      case ExprOp::FpToIntU:
      case ExprOp::IntToFpS:
      case ExprOp::IntToFpU:
        return function_->cast(op, type, operands[0]);
      default:
        break;
    }

    Expr expr;
    expr.op = op;
    expr.type = type;
    expr.operandCount = static_cast<uint8_t>(operands.size());
    for (std::size_t index = 0; index < operands.size(); ++index) {
      expr.operands[index] = operands[index];
    }
    return function_->intern(expr);
  }

  Result<std::vector<ExprId>> parseExprList(const Line& line, Cursor& cursor) {
    std::vector<ExprId> operands;
    if (cursor.consume(')')) {
      return operands;
    }
    while (true) {
      XDEC_TRY(const ExprId operand, parseExpr(line, cursor));
      operands.push_back(operand);
      if (cursor.consume(')')) {
        return operands;
      }
      if (!cursor.consume(',')) {
        return fail(line.number, "expected ',' or ')' in an operand list");
      }
    }
  }

  Result<Type> requireType(const Line& line, std::string_view spelling) {
    Type type;
    if (!Type::parse(spelling, type)) {
      return fail(line.number, std::format("expected a type but found '{}'", spelling));
    }
    return type;
  }

  Result<void> resolveDeferredOperands() {
    for (const Deferred& entry : deferred_) {
      Cursor cursor{entry.text};
      std::vector<ExprId> operands;
      const Line line{entry.text, entry.lineNumber};
      while (!cursor.atEnd()) {
        XDEC_TRY(const ExprId operand, parseExpr(line, cursor));
        operands.push_back(operand);
        if (!cursor.consume(',')) {
          break;
        }
      }
      function_->setOperands(entry.op, operands);
    }
    return ok();
  }

  struct Deferred {
    OpId op;
    std::string text;
    unsigned lineNumber;
  };

  std::vector<Line> lines_;
  const RegisterFile* registers_;
  std::unique_ptr<Function> function_;
  Maturity pendingMaturity_ = Maturity::Lifted;
  std::unordered_map<uint32_t, BlockId> blockByLabel_;
  std::unordered_map<uint64_t, ValueId> valueByLabel_;
  std::vector<Deferred> deferred_;
};

}  // namespace

Result<std::unique_ptr<Function>> parse(std::string_view text, const RegisterFile& registers) {
  Parser parser{text, registers};
  return parser.run();
}

}  // namespace xdec::il
