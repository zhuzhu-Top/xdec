#include "xdec/support/json.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace xdec::json {
namespace {

const std::string kEmptyString;

void writeEscaped(std::string& out, std::string_view text) {
  out.push_back('"');
  for (const char raw : text) {
    const auto byte = static_cast<unsigned char>(raw);
    switch (raw) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (byte < 0x20) {
          char buffer[7];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", byte);
          out += buffer;
        } else {
          out.push_back(raw);
        }
        break;
    }
  }
  out.push_back('"');
}

void writeNumber(std::string& out, bool isInteger, int64_t integer, double number) {
  if (isInteger) {
    out += std::to_string(integer);
    return;
  }
  if (!std::isfinite(number)) {
    // JSON has no spelling for these; null is the honest answer.
    out += "null";
    return;
  }
  char buffer[32];
  const auto end = std::to_chars(buffer, buffer + sizeof(buffer), number).ptr;
  out.append(buffer, static_cast<std::size_t>(end - buffer));
}

class Writer {
 public:
  explicit Writer(unsigned indent) noexcept : indent_(indent) {}

  void write(const Value& value, unsigned depth) {
    switch (value.kind()) {
      case Kind::Null: out_ += "null"; return;
      case Kind::Bool: out_ += value.asBool() ? "true" : "false"; return;
      case Kind::Number: writeScalarNumber(value); return;
      case Kind::String: writeEscaped(out_, value.asString()); return;
      case Kind::Array: writeArray(value, depth); return;
      case Kind::Object: writeObject(value, depth); return;
    }
  }

  [[nodiscard]] std::string take() && { return std::move(out_); }

 private:
  void writeScalarNumber(const Value& value) {
    const double number = value.asDouble();
    const int64_t integer = value.asInt();
    const bool isInteger = static_cast<double>(integer) == number;
    writeNumber(out_, isInteger, integer, number);
  }

  void newline(unsigned depth) {
    if (indent_ == 0) {
      return;
    }
    out_.push_back('\n');
    out_.append(static_cast<std::size_t>(indent_) * depth, ' ');
  }

  void writeArray(const Value& value, unsigned depth) {
    if (value.items().empty()) {
      out_ += "[]";
      return;
    }
    out_.push_back('[');
    bool first = true;
    for (const Value& item : value.items()) {
      if (!first) {
        out_.push_back(',');
      }
      first = false;
      newline(depth + 1);
      write(item, depth + 1);
    }
    newline(depth);
    out_.push_back(']');
  }

  void writeObject(const Value& value, unsigned depth) {
    if (value.members().empty()) {
      out_ += "{}";
      return;
    }
    out_.push_back('{');
    bool first = true;
    for (const Member& member : value.members()) {
      if (!first) {
        out_.push_back(',');
      }
      first = false;
      newline(depth + 1);
      writeEscaped(out_, member.first);
      out_.push_back(':');
      if (indent_ != 0) {
        out_.push_back(' ');
      }
      write(member.second, depth + 1);
    }
    newline(depth);
    out_.push_back('}');
  }

  unsigned indent_;
  std::string out_;
};

/// Recursive-descent parser over the whole document. Depth is bounded because
/// a deeply nested input is the one way a data file can crash the process, and
/// these files are all shallow by construction.
constexpr unsigned kMaxDepth = 64;

class Parser {
 public:
  explicit Parser(std::string_view text) noexcept : text_(text) {}

  [[nodiscard]] Result<Value> parseDocument() {
    skipSpace();
    XDEC_TRY(Value value, parseValue(0));
    skipSpace();
    if (pos_ != text_.size()) {
      return fail("trailing content after JSON document");
    }
    return value;
  }

 private:
  [[nodiscard]] Unexpected fail(std::string message) const {
    return err(DiagCode::ParseError,
               std::format("json: {} at offset {}", message, pos_));
  }

  void skipSpace() noexcept {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  [[nodiscard]] bool consume(char expected) noexcept {
    if (pos_ < text_.size() && text_[pos_] == expected) {
      ++pos_;
      return true;
    }
    return false;
  }

  [[nodiscard]] bool consumeWord(std::string_view word) noexcept {
    if (text_.substr(pos_, word.size()) == word) {
      pos_ += word.size();
      return true;
    }
    return false;
  }

  [[nodiscard]] Result<Value> parseValue(unsigned depth) {
    if (depth > kMaxDepth) {
      return fail("nesting too deep");
    }
    if (pos_ >= text_.size()) {
      return fail("unexpected end of input");
    }
    switch (text_[pos_]) {
      case '{': return parseObject(depth);
      case '[': return parseArray(depth);
      case '"': {
        XDEC_TRY(std::string text, parseString());
        return Value{std::move(text)};
      }
      case 't':
        if (consumeWord("true")) {
          return Value{true};
        }
        return fail("invalid literal");
      case 'f':
        if (consumeWord("false")) {
          return Value{false};
        }
        return fail("invalid literal");
      case 'n':
        if (consumeWord("null")) {
          return Value{};
        }
        return fail("invalid literal");
      default: return parseNumber();
    }
  }

  [[nodiscard]] Result<std::string> parseString() {
    if (!consume('"')) {
      return fail("expected string");
    }
    std::string out;
    while (true) {
      if (pos_ >= text_.size()) {
        return fail("unterminated string");
      }
      const char c = text_[pos_++];
      if (c == '"') {
        return out;
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) {
        return fail("unterminated escape");
      }
      const char escape = text_[pos_++];
      switch (escape) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          XDEC_TRY(uint32_t code, parseHex4());
          appendUtf8(out, code);
          break;
        }
        default: return fail("invalid escape");
      }
    }
  }

  [[nodiscard]] Result<uint32_t> parseHex4() {
    if (pos_ + 4 > text_.size()) {
      return fail("truncated \\u escape");
    }
    uint32_t code = 0;
    for (unsigned i = 0; i < 4; ++i) {
      const char c = text_[pos_++];
      code <<= 4U;
      if (c >= '0' && c <= '9') {
        code |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        code |= static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        code |= static_cast<uint32_t>(c - 'A' + 10);
      } else {
        return fail("invalid hex digit in \\u escape");
      }
    }
    return code;
  }

  /// Surrogate pairs are written through as-is rather than combined: nothing
  /// in this project's data files is outside the BMP, and a lone surrogate
  /// round-tripping unchanged is better than one silently becoming U+FFFD.
  static void appendUtf8(std::string& out, uint32_t code) {
    if (code < 0x80) {
      out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out.push_back(static_cast<char>(0xC0U | (code >> 6U)));
      out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    } else {
      out.push_back(static_cast<char>(0xE0U | (code >> 12U)));
      out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    }
  }

  [[nodiscard]] Result<Value> parseNumber() {
    const std::size_t start = pos_;
    if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) {
      ++pos_;
    }
    bool isInteger = true;
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c >= '0' && c <= '9') {
        ++pos_;
      } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
        isInteger = false;
        ++pos_;
      } else {
        break;
      }
    }
    const std::string_view token = text_.substr(start, pos_ - start);
    if (token.empty() || token == "-" || token == "+") {
      return fail("expected number");
    }
    if (isInteger) {
      int64_t integer = 0;
      const char* first = token.data();
      const char* last = first + token.size();
      if (first != last && *first == '+') {
        ++first;
      }
      const auto parsed = std::from_chars(first, last, integer);
      if (parsed.ec == std::errc{} && parsed.ptr == last) {
        return Value{integer};
      }
      // Out of int64 range: fall through to the floating-point reading, which
      // loses precision but does not lose the document.
    }
    double number = 0.0;
    std::string owned{token};
    try {
      std::size_t consumed = 0;
      number = std::stod(owned, &consumed);
      if (consumed != owned.size()) {
        return fail("invalid number");
      }
    } catch (const std::exception&) {
      return fail("invalid number");
    }
    return Value{number};
  }

  [[nodiscard]] Result<Value> parseArray(unsigned depth) {
    if (!consume('[')) {
      return fail("expected '['");
    }
    std::vector<Value> items;
    skipSpace();
    if (consume(']')) {
      return Value::array(std::move(items));
    }
    while (true) {
      skipSpace();
      XDEC_TRY(Value item, parseValue(depth + 1));
      items.push_back(std::move(item));
      skipSpace();
      if (consume(',')) {
        continue;
      }
      if (consume(']')) {
        return Value::array(std::move(items));
      }
      return fail("expected ',' or ']'");
    }
  }

  [[nodiscard]] Result<Value> parseObject(unsigned depth) {
    if (!consume('{')) {
      return fail("expected '{'");
    }
    std::vector<Member> members;
    skipSpace();
    if (consume('}')) {
      return Value::object(std::move(members));
    }
    while (true) {
      skipSpace();
      XDEC_TRY(std::string key, parseString());
      skipSpace();
      if (!consume(':')) {
        return fail("expected ':'");
      }
      skipSpace();
      XDEC_TRY(Value item, parseValue(depth + 1));
      members.emplace_back(std::move(key), std::move(item));
      skipSpace();
      if (consume(',')) {
        continue;
      }
      if (consume('}')) {
        return Value::object(std::move(members));
      }
      return fail("expected ',' or '}'");
    }
  }

  std::string_view text_;
  std::size_t pos_ = 0;
};

}  // namespace

Value Value::array(std::vector<Value> items) {
  Value value;
  value.kind_ = Kind::Array;
  value.items_ = std::move(items);
  return value;
}

Value Value::object(std::vector<Member> members) {
  Value value;
  value.kind_ = Kind::Object;
  value.members_ = std::move(members);
  return value;
}

int64_t Value::asInt() const noexcept {
  if (!isNumber()) {
    return 0;
  }
  return isInteger_ ? integer_ : static_cast<int64_t>(number_);
}

const std::string& Value::asString() const noexcept {
  return isString() ? string_ : kEmptyString;
}

const Value* Value::find(std::string_view key) const noexcept {
  if (!isObject()) {
    return nullptr;
  }
  for (const Member& member : members_) {
    if (member.first == key) {
      return &member.second;
    }
  }
  return nullptr;
}

std::optional<int64_t> Value::intAt(std::string_view key) const noexcept {
  const Value* found = find(key);
  if (found == nullptr || !found->isNumber()) {
    return std::nullopt;
  }
  return found->asInt();
}

std::optional<std::string> Value::stringAt(std::string_view key) const {
  const Value* found = find(key);
  if (found == nullptr || !found->isString()) {
    return std::nullopt;
  }
  return found->asString();
}

std::optional<bool> Value::boolAt(std::string_view key) const noexcept {
  const Value* found = find(key);
  if (found == nullptr || !found->isBool()) {
    return std::nullopt;
  }
  return found->asBool();
}

void Value::set(std::string key, Value value) {
  kind_ = Kind::Object;
  for (Member& member : members_) {
    if (member.first == key) {
      member.second = std::move(value);
      return;
    }
  }
  members_.emplace_back(std::move(key), std::move(value));
}

std::string Value::dump(unsigned indent) const {
  Writer writer{indent};
  writer.write(*this, 0);
  return std::move(writer).take();
}

Result<Value> parse(std::string_view text) {
  Parser parser{text};
  return parser.parseDocument();
}

Result<Value> parseFile(const std::string& path) {
  std::ifstream stream{path, std::ios::binary};
  if (!stream) {
    return err(DiagCode::IoError, std::format("cannot open '{}'", path));
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  const std::string text = buffer.str();
  Result<Value> parsed = parse(text);
  if (!parsed) {
    return err(Diag{parsed.error()}.note(std::format("while reading '{}'", path)));
  }
  return parsed;
}

}  // namespace xdec::json
