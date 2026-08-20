#pragma once

#include <cstdint>
#include <string_view>

#include "optiforge/support/SourceLocation.h"

namespace optiforge {

enum class TokenKind : std::uint8_t {
  // Literals and identifiers
  Identifier,
  IntLiteral,
  FloatLiteral,

  // Keywords
  KwFn,
  KwInt,
  KwFloat,
  KwBool,
  KwVoid,
  KwIf,
  KwElse,
  KwWhile,
  KwReturn,
  KwTrue,
  KwFalse,

  // Operators
  Plus,
  Minus,
  Star,
  Slash,
  Percent,
  Bang,
  Assign,
  EqualEqual,
  BangEqual,
  Less,
  Greater,
  LessEqual,
  GreaterEqual,
  AmpAmp,
  PipePipe,

  // Punctuation
  LParen,
  RParen,
  LBrace,
  RBrace,
  LBracket,
  RBracket,
  Comma,
  Semicolon,
  Arrow,

  // Control
  EndOfFile,
  Error,
};

/// Stable machine-facing spelling, used by `--emit=tokens`. Golden tests pin
/// these, so changing one is a deliberate act.
std::string_view toString(TokenKind kind);

/// Human-facing phrase for diagnostics, already quoted where appropriate:
/// "')'", "an identifier", "end of file".
std::string_view describe(TokenKind kind);

/// True for tokens that can begin a statement. Used by parser error recovery
/// to decide where it is safe to resume.
bool canStartStatement(TokenKind kind);

/// True for the four type keywords.
bool isTypeKeyword(TokenKind kind);

struct Token {
  TokenKind kind = TokenKind::Error;
  SourceLocation loc{};
  /// Text exactly as it appeared, pointing into the SourceManager buffer.
  std::string_view lexeme{};

  // Only one of these is meaningful, selected by `kind`. Kept as plain members
  // rather than a union: 16 bytes per token is irrelevant at our scale, and a
  // union here would buy nothing but a chance to read the wrong member.
  std::int64_t intValue = 0;
  double floatValue = 0.0;

  bool is(TokenKind k) const { return kind == k; }
  bool isNot(TokenKind k) const { return kind != k; }

  /// End of this token, for building ranges that underline the whole lexeme.
  SourceLocation endLoc() const {
    SourceLocation e = loc;
    e.col += static_cast<std::uint32_t>(lexeme.size());
    return e;
  }

  SourceRange range() const { return {loc, endLoc()}; }
};

}  // namespace optiforge
