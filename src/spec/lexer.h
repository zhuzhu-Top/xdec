// Tokeniser for the instruction semantics DSL.
//
// Scans the whole file up front into a token vector. Spec files are small
// (thousands of lines), and having the full sequence available lets the parser
// look ahead freely without a pushback buffer.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xdec/spec/ast.h"
#include "xdec/support/result.h"

namespace xdec::spec {

enum class TokenKind : uint8_t {
  End,
  Identifier,
  Integer,
  /// A double-quoted string; also carries the raw text of an asm template.
  String,
  /// A bit-pattern literal such as `"1101011"`, distinguished from a string by
  /// the parser's context rather than by the lexer.
  LBrace,
  RBrace,
  LParen,
  RParen,
  LBracket,
  RBracket,
  Comma,
  Semicolon,
  Colon,
  Dot,
  DotDot,
  Question,
  Arrow,       // ->
  Assign,      // =
  Equal,       // ==
  NotEqual,    // !=
  Less,        // <
  LessEqual,   // <=
  Greater,     // >
  GreaterEqual,// >=
  LessU,       // <u
  LessEqualU,  // <=u
  GreaterU,    // >u
  GreaterEqualU,  // >=u
  Plus,
  Minus,
  Star,
  Slash,
  SlashS,      // /s
  Percent,
  PercentS,    // %s
  Ampersand,
  Pipe,
  Caret,
  Tilde,
  Bang,
  Shl,         // <<
  Shr,         // >>
  ShrS,        // >>>
  AndAnd,      // &&
  OrOr,        // ||
};

[[nodiscard]] std::string_view toString(TokenKind kind) noexcept;

struct Token {
  TokenKind kind = TokenKind::End;
  SourceLoc loc;
  /// Identifier or string contents.
  std::string text;
  uint64_t integer = 0;
  /// Digit count for a literal written in binary or hex, so that a bit pattern
  /// keeps its leading zeroes.
  unsigned digits = 0;
};

/// Scans `text`. Reports the first lexical error rather than recovering: a
/// malformed token means the rest of the file cannot be trusted.
[[nodiscard]] Result<std::vector<Token>> tokenize(std::string_view text,
                                                  std::string_view sourceName);

}  // namespace xdec::spec
