#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "TestHarness.h"
#include "optiforge/frontend/Lexer.h"
#include "optiforge/frontend/Token.h"
#include "optiforge/support/Diagnostic.h"
#include "optiforge/support/SourceManager.h"

using namespace optiforge;

namespace {

/// Lexes `source` in isolation, capturing diagnostics rather than printing.
struct LexResult {
  SourceManager sm;
  std::ostringstream diagOut;
  std::vector<Token> tokens;
  unsigned errors = 0;

  /// Token kinds excluding the trailing EndOfFile, for concise comparisons.
  std::vector<TokenKind> kinds() const {
    std::vector<TokenKind> out;
    for (const Token& t : tokens) {
      if (t.is(TokenKind::EndOfFile)) break;
      out.push_back(t.kind);
    }
    return out;
  }

  std::string diagnostics() const { return diagOut.str(); }
};

std::unique_ptr<LexResult> lex(std::string source) {
  auto result = std::make_unique<LexResult>();
  const FileID file = result->sm.addBuffer("t.of", std::move(source));
  DiagnosticEngine diags(result->sm, result->diagOut);
  Lexer lexer(result->sm, file, diags);
  result->tokens = lexer.tokenize();
  result->errors = diags.errorCount();
  return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// Structure
// ---------------------------------------------------------------------------

TEST("the stream always ends with exactly one EOF") {
  for (const char* src : {"", "   ", "x", "fn f() {}", "// only a comment"}) {
    auto r = lex(src);
    CHECK(!r->tokens.empty());
    CHECK(r->tokens.back().is(TokenKind::EndOfFile));
    unsigned eofCount = 0;
    for (const Token& t : r->tokens) {
      if (t.is(TokenKind::EndOfFile)) ++eofCount;
    }
    CHECK_EQ(eofCount, 1u);
  }
}

TEST("an empty file yields only EOF") {
  auto r = lex("");
  CHECK_EQ(r->tokens.size(), std::size_t{1});
  CHECK_EQ(r->errors, 0u);
}

// ---------------------------------------------------------------------------
// Keywords and identifiers
// ---------------------------------------------------------------------------

TEST("every keyword is recognized") {
  auto r = lex("fn int float bool void if else while return true false");
  const std::vector<TokenKind> expected{
      TokenKind::KwFn,    TokenKind::KwInt,   TokenKind::KwFloat, TokenKind::KwBool,
      TokenKind::KwVoid,  TokenKind::KwIf,    TokenKind::KwElse,  TokenKind::KwWhile,
      TokenKind::KwReturn, TokenKind::KwTrue, TokenKind::KwFalse};
  CHECK(r->kinds() == expected);
  CHECK_EQ(r->errors, 0u);
}

TEST("identifiers that merely contain a keyword stay identifiers") {
  auto r = lex("integer iffy returned _if fn2");
  const std::vector<TokenKind> expected(5, TokenKind::Identifier);
  CHECK(r->kinds() == expected);
}

TEST("identifiers may start with or contain underscores and digits") {
  auto r = lex("_x a1 _ print_int");
  const std::vector<TokenKind> expected(4, TokenKind::Identifier);
  CHECK(r->kinds() == expected);
  CHECK_EQ(std::string(r->tokens[3].lexeme), std::string("print_int"));
}

// ---------------------------------------------------------------------------
// Operators: maximal munch
// ---------------------------------------------------------------------------

TEST("two-character operators win over one-character prefixes") {
  auto r = lex("== != <= >= && || -> = ! < > - &&");
  const std::vector<TokenKind> expected{
      TokenKind::EqualEqual, TokenKind::BangEqual,  TokenKind::LessEqual,
      TokenKind::GreaterEqual, TokenKind::AmpAmp,   TokenKind::PipePipe,
      TokenKind::Arrow,      TokenKind::Assign,     TokenKind::Bang,
      TokenKind::Less,       TokenKind::Greater,    TokenKind::Minus,
      TokenKind::AmpAmp};
  CHECK(r->kinds() == expected);
  CHECK_EQ(r->errors, 0u);
}

TEST("arithmetic and punctuation") {
  auto r = lex("+ - * / % ( ) { } , ;");
  const std::vector<TokenKind> expected{
      TokenKind::Plus,   TokenKind::Minus,  TokenKind::Star,   TokenKind::Slash,
      TokenKind::Percent, TokenKind::LParen, TokenKind::RParen, TokenKind::LBrace,
      TokenKind::RBrace, TokenKind::Comma,  TokenKind::Semicolon};
  CHECK(r->kinds() == expected);
}

TEST("a lone ampersand or pipe is an error, not a silent token") {
  auto r = lex("a & b");
  CHECK_EQ(r->errors, 1u);
  CHECK(r->diagnostics().find("expected '&&'") != std::string::npos);

  auto r2 = lex("a | b");
  CHECK_EQ(r2->errors, 1u);
  CHECK(r2->diagnostics().find("expected '||'") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Numbers
// ---------------------------------------------------------------------------

TEST("integer literals carry their value") {
  auto r = lex("0 7 42 9223372036854775807");
  CHECK_EQ(r->errors, 0u);
  CHECK_EQ(r->tokens[0].intValue, std::int64_t{0});
  CHECK_EQ(r->tokens[1].intValue, std::int64_t{7});
  CHECK_EQ(r->tokens[2].intValue, std::int64_t{42});
  CHECK_EQ(r->tokens[3].intValue, std::int64_t{9223372036854775807LL});
}

TEST("float literals carry their value") {
  auto r = lex("3.14 0.5 1e3 2.5e-2 1E+2");
  CHECK_EQ(r->errors, 0u);
  for (std::size_t i = 0; i < 5; ++i) {
    CHECK(r->tokens[i].is(TokenKind::FloatLiteral));
  }
  CHECK_EQ(r->tokens[0].floatValue, 3.14);
  CHECK_EQ(r->tokens[1].floatValue, 0.5);
  CHECK_EQ(r->tokens[2].floatValue, 1000.0);
  CHECK_EQ(r->tokens[3].floatValue, 0.025);
  CHECK_EQ(r->tokens[4].floatValue, 100.0);
}

TEST("an integer too large for int is rejected, not silently wrapped") {
  auto r = lex("99999999999999999999");
  CHECK_EQ(r->errors, 1u);
  CHECK(r->diagnostics().find("too large") != std::string::npos);
}

TEST("malformed numbers are rejected with one error each") {
  struct Case {
    const char* source;
    const char* expectedText;
  };
  const Case cases[] = {
      {"1.2.3", "invalid number literal"},
      {"123abc", "invalid number literal"},
      {"0x10", "invalid number literal"},
      {"1e", "exponent has no digits"},
      {"1e+", "exponent has no digits"},
      {"1.", "expected a digit after '.'"},
  };

  for (const Case& c : cases) {
    auto r = lex(c.source);
    CHECK_EQ(r->errors, 1u);
    CHECK(r->diagnostics().find(c.expectedText) != std::string::npos);
  }
}

TEST("a member-like dot after a number does not swallow the next token") {
  // "1 . 2" is three tokens; only "1.2" is one float.
  auto r = lex("1.2");
  CHECK_EQ(r->errors, 0u);
  CHECK_EQ(r->kinds().size(), std::size_t{1});
}

// ---------------------------------------------------------------------------
// Comments and whitespace
// ---------------------------------------------------------------------------

TEST("line comments run to end of line only") {
  auto r = lex("a // comment with fn int 3\nb");
  const std::vector<TokenKind> expected{TokenKind::Identifier, TokenKind::Identifier};
  CHECK(r->kinds() == expected);
  CHECK_EQ(r->errors, 0u);
}

TEST("block comments may span lines") {
  auto r = lex("a /* fn int\n   still comment */ b");
  const std::vector<TokenKind> expected{TokenKind::Identifier, TokenKind::Identifier};
  CHECK(r->kinds() == expected);
  CHECK_EQ(r->errors, 0u);
}

TEST("an unterminated block comment reports at its opening delimiter") {
  auto r = lex("a\nb /* runs off the end\nc\n");
  CHECK_EQ(r->errors, 1u);
  CHECK(r->diagnostics().find("unterminated block comment") != std::string::npos);
  // Line 2 is where the "/*" is; reporting at EOF would send the reader to the
  // wrong place entirely.
  CHECK(r->diagnostics().find("t.of:2:3") != std::string::npos);
}

TEST("block comments do not nest") {
  // The first "*/" closes; "b" is code and the trailing "*/" is then garbage.
  auto r = lex("/* outer /* inner */ b");
  CHECK_EQ(r->errors, 0u);
  const std::vector<TokenKind> expected{TokenKind::Identifier};
  CHECK(r->kinds() == expected);
}

// ---------------------------------------------------------------------------
// Locations
// ---------------------------------------------------------------------------

TEST("line and column track across newlines") {
  auto r = lex("ab cd\n  ef\n\ngh");
  CHECK_EQ(r->tokens[0].loc.line, 1u);
  CHECK_EQ(r->tokens[0].loc.col, 1u);
  CHECK_EQ(r->tokens[1].loc.line, 1u);
  CHECK_EQ(r->tokens[1].loc.col, 4u);
  CHECK_EQ(r->tokens[2].loc.line, 2u);
  CHECK_EQ(r->tokens[2].loc.col, 3u);
  CHECK_EQ(r->tokens[3].loc.line, 4u);
  CHECK_EQ(r->tokens[3].loc.col, 1u);
}

TEST("CRLF line endings do not corrupt column numbers") {
  auto r = lex("ab\r\n  cd\r\n");
  CHECK_EQ(r->tokens[1].loc.line, 2u);
  CHECK_EQ(r->tokens[1].loc.col, 3u);
}

TEST("lexemes point at the exact source text") {
  auto r = lex("fn hello_world(int n)");
  CHECK_EQ(std::string(r->tokens[1].lexeme), std::string("hello_world"));
  CHECK_EQ(r->tokens[1].endLoc().col, 15u);  // 4 + len("hello_world")
}

// ---------------------------------------------------------------------------
// Error recovery
// ---------------------------------------------------------------------------

TEST("lexing continues after a bad character so all errors are reported") {
  auto r = lex("a # b $ c");
  CHECK_EQ(r->errors, 2u);
  // The three identifiers must still be present either side of the junk.
  unsigned identifiers = 0;
  for (const Token& t : r->tokens) {
    if (t.is(TokenKind::Identifier)) ++identifiers;
  }
  CHECK_EQ(identifiers, 3u);
}

TEST("unprintable bytes are escaped in the diagnostic") {
  auto r = lex(std::string("a\x01 b"));
  CHECK_EQ(r->errors, 1u);
  CHECK(r->diagnostics().find("'\\x01'") != std::string::npos);
}

TEST("string literals are rejected with a specific message") {
  auto r = lex("a \"hello\"");
  CHECK(r->errors >= 1u);
  CHECK(r->diagnostics().find("string literals are not supported") != std::string::npos);
}
