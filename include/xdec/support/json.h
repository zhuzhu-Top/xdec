// A minimal JSON reader and writer.
//
// Two things in the project need JSON and neither needs much of it: the type
// database serialises itself for caching and golden tests, and the syscall
// table is a hand-maintained data file. Both are read and written by this
// code alone, so the subset implemented here is the subset those two use —
// objects, arrays, strings, numbers, booleans, null — with no streaming, no
// comments, and no duplicate-key merging.
//
// The alternative was a dependency. A decompiler that vendors a JSON library
// to read a 120-entry lookup table has bought a supply chain for a lookup
// table, and the parser below is smaller than the code that would configure
// one.
//
// Object members keep insertion order. That is not a JSON requirement; it is
// what makes a serialised database diffable against the last one, which is the
// only reason the serialiser exists.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xdec/support/result.h"

namespace xdec::json {

class Value;

using Member = std::pair<std::string, Value>;

enum class Kind : uint8_t { Null, Bool, Number, String, Array, Object };

class Value {
 public:
  Value() = default;
  Value(std::nullptr_t) noexcept {}                              // NOLINT
  Value(bool value) noexcept : kind_(Kind::Bool), bool_(value) {} // NOLINT
  Value(int64_t value) noexcept                                  // NOLINT
      : kind_(Kind::Number), integer_(value), number_(static_cast<double>(value)) {}
  Value(uint64_t value) noexcept                                 // NOLINT
      : kind_(Kind::Number), integer_(static_cast<int64_t>(value)),
        number_(static_cast<double>(value)) {}
  Value(double value) noexcept                                   // NOLINT
      : kind_(Kind::Number), isInteger_(false), number_(value) {}
  Value(std::string value)                                       // NOLINT
      : kind_(Kind::String), string_(std::move(value)) {}
  Value(std::string_view value) : Value(std::string{value}) {}   // NOLINT
  Value(const char* value) : Value(std::string{value}) {}        // NOLINT

  [[nodiscard]] static Value array(std::vector<Value> items);
  [[nodiscard]] static Value object(std::vector<Member> members);

  [[nodiscard]] Kind kind() const noexcept { return kind_; }
  [[nodiscard]] bool isNull() const noexcept { return kind_ == Kind::Null; }
  [[nodiscard]] bool isBool() const noexcept { return kind_ == Kind::Bool; }
  [[nodiscard]] bool isNumber() const noexcept { return kind_ == Kind::Number; }
  [[nodiscard]] bool isString() const noexcept { return kind_ == Kind::String; }
  [[nodiscard]] bool isArray() const noexcept { return kind_ == Kind::Array; }
  [[nodiscard]] bool isObject() const noexcept { return kind_ == Kind::Object; }

  /// Accessors return the type's zero value for a mismatched kind. Callers
  /// that care about the difference between "absent" and "false" ask through
  /// the optional-returning member lookups below.
  [[nodiscard]] bool asBool() const noexcept { return isBool() && bool_; }
  [[nodiscard]] double asDouble() const noexcept { return isNumber() ? number_ : 0.0; }
  [[nodiscard]] int64_t asInt() const noexcept;
  [[nodiscard]] const std::string& asString() const noexcept;
  [[nodiscard]] const std::vector<Value>& items() const noexcept { return items_; }
  [[nodiscard]] const std::vector<Member>& members() const noexcept { return members_; }

  /// Object member lookup by key; nullptr when this is not an object or the
  /// key is absent. Linear, which is the right shape for the handful of keys
  /// every object in this project has.
  [[nodiscard]] const Value* find(std::string_view key) const noexcept;
  [[nodiscard]] std::optional<int64_t> intAt(std::string_view key) const noexcept;
  [[nodiscard]] std::optional<std::string> stringAt(std::string_view key) const;
  [[nodiscard]] std::optional<bool> boolAt(std::string_view key) const noexcept;

  void push(Value value) { items_.push_back(std::move(value)); }
  void set(std::string key, Value value);

  /// `indent` of zero writes one line; anything else pretty-prints with that
  /// many spaces per level.
  [[nodiscard]] std::string dump(unsigned indent = 2) const;

 private:
  Kind kind_ = Kind::Null;
  bool bool_ = false;
  bool isInteger_ = true;
  int64_t integer_ = 0;
  double number_ = 0.0;
  std::string string_;
  std::vector<Value> items_;
  std::vector<Member> members_;
};

/// Parses a complete document. Trailing content other than whitespace is an
/// error, because a truncated write that happens to end on a valid value is
/// exactly the corruption this would otherwise hide.
[[nodiscard]] Result<Value> parse(std::string_view text);

/// Reads and parses a file, reporting the path in any diagnostic.
[[nodiscard]] Result<Value> parseFile(const std::string& path);

}  // namespace xdec::json
