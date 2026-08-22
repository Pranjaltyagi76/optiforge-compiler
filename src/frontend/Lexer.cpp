#include "optiforge/frontend/Lexer.h"

#include <charconv>
#include <string>
#include <system_error>

#include "optiforge/support/Diagnostic.h"
#include "optiforge/support/SourceManager.h"

namespace optiforge {

namespace {

// Explicit character classification rather than <cctype>: those take an int and
// have undefined behaviour for negative char values, which is exactly what a
// non-ASCII byte in a source file produces on a platform with signed char.
constexpr bool isDigitChar(char c) { return c >= '0' && c <= '9'; }

constexpr bool isAlphaChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

constexpr bool isAlnumChar(char c) { return isAlphaChar(c) || isDigitChar(c); }

/// Renders a byte for a diagnostic, escaping anything unprintable so the
/// message stays on one line and is readable for non-ASCII input.
std::string describeChar(char c) {
  const auto byte = static_cast<unsigned char>(c);
  if (byte >= 0x20 && byte < 0x7f) {
    return std::string("'") + c + "'";
  }
  static const char* kHex = "0123456789abcdef";
  std::string out = "'\\x";
  out += kHex[(byte >> 4) & 0xf];
  out += kHex[byte & 0xf];
  out += "'";
  return out;
}

TokenKind keywordKind(std::string_view text) {
  if (text == "fn") return TokenKind::KwFn;
  if (text == "int") return TokenKind::KwInt;
  if (text == "float") return TokenKind::KwFloat;
  if (text == "bool") return TokenKind::KwBool;
  if (text == "void") return TokenKind::KwVoid;
  if (text == "if") return TokenKind::KwIf;
  if (text == "else") return TokenKind::KwElse;
  if (text == "while") return TokenKind::KwWhile;
  if (text == "for") return TokenKind::KwFor;
  if (text == "break") return TokenKind::KwBreak;
  if (text == "continue") return TokenKind::KwContinue;
  if (text == "return") return TokenKind::KwReturn;
  if (text == "true") return TokenKind::KwTrue;
  if (text == "false") return TokenKind::KwFalse;
  return TokenKind::Identifier;
}

}  // namespace

Lexer::Lexer(const SourceManager& sources, FileID file, DiagnosticEngine& diags)
    : sources_(sources), diags_(diags), file_(file), text_(sources.contents(file)) {}

bool Lexer::isAtEnd() const { return pos_ >= text_.size(); }

char Lexer::peek(std::size_t ahead) const {
  const std::size_t index = pos_ + ahead;
  return index < text_.size() ? text_[index] : '\0';
}

char Lexer::advance() {
  const char c = text_[pos_++];
  if (c == '\n') {
    ++line_;
    lineStart_ = pos_;
  }
  return c;
}

bool Lexer::matchChar(char expected) {
  if (isAtEnd() || text_[pos_] != expected) {
    return false;
  }
  advance();
  return true;
}

SourceLocation Lexer::locAt(std::size_t offset, std::uint32_t line,
                            std::size_t lineStart) const {
  SourceLocation loc;
  loc.file = file_;
  loc.line = line;
  loc.col = static_cast<std::uint32_t>(offset - lineStart) + 1;
  return loc;
}

SourceLocation Lexer::here() const { return locAt(pos_, line_, lineStart_); }

Token Lexer::make(TokenKind kind, SourceLocation start, std::size_t startOffset) const {
  Token token;
  token.kind = kind;
  token.loc = start;
  token.lexeme = text_.substr(startOffset, pos_ - startOffset);
  return token;
}

void Lexer::skipTrivia() {
  while (!isAtEnd()) {
    const char c = peek();

    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      advance();
      continue;
    }

    if (c == '/' && peek(1) == '/') {
      while (!isAtEnd() && peek() != '\n') {
        advance();
      }
      continue;
    }

    if (c == '/' && peek(1) == '*') {
      const SourceLocation open = here();
      advance();  // '/'
      advance();  // '*'
      bool closed = false;
      while (!isAtEnd()) {
        if (peek() == '*' && peek(1) == '/') {
          advance();
          advance();
          closed = true;
          break;
        }
        advance();
      }
      if (!closed) {
        // Report at the opening delimiter: that is where the reader must look,
        // not at the end of file where the scan happened to stop.
        diags_.error(open, "unterminated block comment");
      }
      continue;
    }

    break;
  }
}

Token Lexer::scanIdentifierOrKeyword(SourceLocation start, std::size_t startOffset) {
  while (isAlnumChar(peek())) {
    advance();
  }
  const std::string_view text = text_.substr(startOffset, pos_ - startOffset);
  return make(keywordKind(text), start, startOffset);
}

Token Lexer::scanNumber(SourceLocation start, std::size_t startOffset) {
  while (isDigitChar(peek())) {
    advance();
  }

  bool isFloat = false;

  if (peek() == '.') {
    if (isDigitChar(peek(1))) {
      isFloat = true;
      advance();  // '.'
      while (isDigitChar(peek())) {
        advance();
      }
    } else {
      advance();  // consume the '.' so the scan makes progress
      diags_.report({start, here()}, DiagSeverity::Error, "expected a digit after '.' in floating-point literal");
      return make(TokenKind::Error, start, startOffset);
    }
  }

  if (peek() == 'e' || peek() == 'E') {
    const std::size_t digitOffset = (peek(1) == '+' || peek(1) == '-') ? 2 : 1;
    if (isDigitChar(peek(digitOffset))) {
      isFloat = true;
      advance();  // 'e'
      if (peek() == '+' || peek() == '-') {
        advance();
      }
      while (isDigitChar(peek())) {
        advance();
      }
    } else {
      advance();  // 'e'
      if (peek() == '+' || peek() == '-') {
        advance();
      }
      diags_.report({start, here()}, DiagSeverity::Error, "exponent has no digits");
      return make(TokenKind::Error, start, startOffset);
    }
  }

  // Anything alphanumeric or a further '.' butted against the number is junk:
  // 123abc, 1.2.3, 0x10. Consume it all so one bad literal yields one error.
  if (isAlnumChar(peek()) || peek() == '.') {
    while (isAlnumChar(peek()) || peek() == '.') {
      advance();
    }
    const std::string_view text = text_.substr(startOffset, pos_ - startOffset);
    diags_.report({start, here()}, DiagSeverity::Error, "invalid number literal '" + std::string(text) + "'");
    return make(TokenKind::Error, start, startOffset);
  }

  const std::string_view text = text_.substr(startOffset, pos_ - startOffset);
  const char* first = text.data();
  const char* last = text.data() + text.size();

  Token token = make(isFloat ? TokenKind::FloatLiteral : TokenKind::IntLiteral, start,
                     startOffset);

  if (isFloat) {
    double value = 0.0;
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last) {
      diags_.report({start, here()}, DiagSeverity::Error, "invalid floating-point literal '" + std::string(text) + "'");
      token.kind = TokenKind::Error;
      return token;
    }
    token.floatValue = value;
  } else {
    std::int64_t value = 0;
    const auto result = std::from_chars(first, last, value);
    if (result.ec == std::errc::result_out_of_range) {
      diags_.report({start, here()}, DiagSeverity::Error, "integer literal '" + std::string(text) +
                              "' is too large for type 'int'");
      token.kind = TokenKind::Error;
      return token;
    }
    if (result.ec != std::errc{} || result.ptr != last) {
      diags_.report({start, here()}, DiagSeverity::Error, "invalid integer literal '" + std::string(text) + "'");
      token.kind = TokenKind::Error;
      return token;
    }
    token.intValue = value;
  }

  return token;
}

Token Lexer::scanToken() {
  skipTrivia();

  const SourceLocation start = here();
  const std::size_t startOffset = pos_;

  if (isAtEnd()) {
    return make(TokenKind::EndOfFile, start, startOffset);
  }

  const char c = peek();

  if (isAlphaChar(c)) {
    return scanIdentifierOrKeyword(start, startOffset);
  }
  if (isDigitChar(c)) {
    return scanNumber(start, startOffset);
  }

  advance();

  switch (c) {
    case '+':
      return make(TokenKind::Plus, start, startOffset);
    case '-':
      return make(matchChar('>') ? TokenKind::Arrow : TokenKind::Minus, start, startOffset);
    case '*':
      return make(TokenKind::Star, start, startOffset);
    case '/':
      return make(TokenKind::Slash, start, startOffset);
    case '%':
      return make(TokenKind::Percent, start, startOffset);
    case '!':
      return make(matchChar('=') ? TokenKind::BangEqual : TokenKind::Bang, start, startOffset);
    case '=':
      return make(matchChar('=') ? TokenKind::EqualEqual : TokenKind::Assign, start,
                  startOffset);
    case '<':
      return make(matchChar('=') ? TokenKind::LessEqual : TokenKind::Less, start, startOffset);
    case '>':
      return make(matchChar('=') ? TokenKind::GreaterEqual : TokenKind::Greater, start,
                  startOffset);
    case '&':
      if (matchChar('&')) {
        return make(TokenKind::AmpAmp, start, startOffset);
      }
      diags_.report({start, here()}, DiagSeverity::Error, "expected '&&'; a single '&' is not an operator in this language");
      return make(TokenKind::Error, start, startOffset);
    case '|':
      if (matchChar('|')) {
        return make(TokenKind::PipePipe, start, startOffset);
      }
      diags_.report({start, here()}, DiagSeverity::Error, "expected '||'; a single '|' is not an operator in this language");
      return make(TokenKind::Error, start, startOffset);
    case '(':
      return make(TokenKind::LParen, start, startOffset);
    case ')':
      return make(TokenKind::RParen, start, startOffset);
    case '{':
      return make(TokenKind::LBrace, start, startOffset);
    case '}':
      return make(TokenKind::RBrace, start, startOffset);
    case '[':
      return make(TokenKind::LBracket, start, startOffset);
    case ']':
      return make(TokenKind::RBracket, start, startOffset);
    case ',':
      return make(TokenKind::Comma, start, startOffset);
    case ';':
      return make(TokenKind::Semicolon, start, startOffset);
    case '"':
      diags_.report({start, here()}, DiagSeverity::Error, "string literals are not supported");
      return make(TokenKind::Error, start, startOffset);
    default:
      diags_.report({start, here()}, DiagSeverity::Error, "unexpected character " + describeChar(c));
      return make(TokenKind::Error, start, startOffset);
  }
}

std::vector<Token> Lexer::tokenize() {
  std::vector<Token> tokens;
  while (true) {
    Token token = scanToken();
    const bool done = token.is(TokenKind::EndOfFile);
    tokens.push_back(token);
    if (done) {
      break;
    }
  }
  return tokens;
}

}  // namespace optiforge
