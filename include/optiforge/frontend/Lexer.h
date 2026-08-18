#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "optiforge/frontend/Token.h"
#include "optiforge/support/SourceLocation.h"

namespace optiforge {

class SourceManager;
class DiagnosticEngine;

/// Hand-written lexer: single pass, at most two characters of lookahead, no
/// backtracking.
///
/// Errors never stop the scan. A bad character produces a TokenKind::Error
/// token and lexing continues, so one run reports every lexical problem in the
/// file rather than only the first.
class Lexer {
public:
  Lexer(const SourceManager& sources, FileID file, DiagnosticEngine& diags);

  /// Scans the whole file. The result always ends with exactly one EndOfFile
  /// token, which the parser relies on as a sentinel.
  std::vector<Token> tokenize();

private:
  bool isAtEnd() const;
  char peek(std::size_t ahead = 0) const;
  char advance();
  bool matchChar(char expected);

  SourceLocation locAt(std::size_t offset, std::uint32_t line, std::size_t lineStart) const;
  SourceLocation here() const;

  /// Skips whitespace and both comment forms. Reports unterminated block
  /// comments.
  void skipTrivia();

  Token scanToken();
  Token scanIdentifierOrKeyword(SourceLocation start, std::size_t startOffset);
  Token scanNumber(SourceLocation start, std::size_t startOffset);

  Token make(TokenKind kind, SourceLocation start, std::size_t startOffset) const;

  const SourceManager& sources_;
  DiagnosticEngine& diags_;
  FileID file_;
  std::string_view text_;

  std::size_t pos_ = 0;
  std::uint32_t line_ = 1;
  std::size_t lineStart_ = 0;
};

}  // namespace optiforge
