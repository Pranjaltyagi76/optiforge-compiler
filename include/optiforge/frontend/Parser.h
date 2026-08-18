#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "optiforge/frontend/AST.h"
#include "optiforge/frontend/Token.h"

namespace optiforge {

class DiagnosticEngine;

/// Recursive-descent parser with precedence climbing for binary expressions.
///
/// On a syntax error the parser reports, then resynchronizes at a statement or
/// declaration boundary and keeps going, so one run reports several independent
/// errors instead of only the first. While resynchronizing it suppresses
/// further diagnostics, which stops a single stray token from producing a
/// cascade of nonsense.
class Parser {
public:
  Parser(const std::vector<Token>& tokens, DiagnosticEngine& diags);

  /// Always returns a Program, possibly with fewer functions than the source
  /// appeared to contain. Check `hadError()` before using the result.
  std::unique_ptr<Program> parseProgram();

  bool hadError() const { return hadError_; }

private:
  // --- Token access ---
  const Token& peek(std::size_t ahead = 0) const;
  const Token& previous() const;
  bool check(TokenKind kind) const;
  bool isAtEnd() const;
  const Token& advance();
  bool match(TokenKind kind);

  /// Consumes `kind` or reports "expected X <context>". Returns null on
  /// failure without consuming.
  const Token* expect(TokenKind kind, std::string_view context);

  /// Like expect(Semicolon, ...) but anchors the diagnostic just past the
  /// previous token, which is where the semicolon should have been.
  const Token* expectSemicolon(std::string_view context);

  // --- Diagnostics and recovery ---
  void errorAt(const Token& token, const std::string& message);
  void synchronize();
  /// Recovery at file scope: skip to the next 'fn' or end of file.
  void synchronizeToTopLevel();

  // --- Grammar ---
  FunctionPtr parseFunctionDecl();
  bool parseParamList(std::vector<ParamPtr>& out);
  bool parseTypeSpec(TypeSpec& out);

  BlockPtr parseBlock();
  StmtPtr parseStatement();
  StmtPtr parseVarDecl();
  StmtPtr parseIfStmt();
  StmtPtr parseWhileStmt();
  StmtPtr parseReturnStmt();
  StmtPtr parseAssignOrExprStmt();

  ExprPtr parseExpression();
  ExprPtr parseBinaryExpr(int minPrecedence);
  ExprPtr parseUnaryExpr();
  ExprPtr parsePrimaryExpr();

  // Recursion limit. Deeply nested input is valid but would otherwise exhaust
  // the native stack: measured, this parser crashed at roughly 2000 nested
  // parentheses. CLI-10 requires a clean diagnostic instead of a crash.
  static constexpr int kMaxNestingDepth = 1000;

  /// RAII depth counter for the mutually recursive grammar functions.
  class DepthGuard {
  public:
    explicit DepthGuard(Parser& parser) : parser_(parser) { ++parser_.depth_; }
    ~DepthGuard() { --parser_.depth_; }
    DepthGuard(const DepthGuard&) = delete;
    DepthGuard& operator=(const DepthGuard&) = delete;
    bool exceeded() const { return parser_.depth_ > kMaxNestingDepth; }

  private:
    Parser& parser_;
  };

  /// Reports the nesting-limit diagnostic. Returns true when the limit is hit.
  bool checkDepth(const DepthGuard& guard);

  const std::vector<Token>& tokens_;
  DiagnosticEngine& diags_;
  std::size_t current_ = 0;
  bool hadError_ = false;
  /// Set after an error, cleared once recovery reaches a safe point. Suppresses
  /// cascading diagnostics.
  bool panicMode_ = false;
  int depth_ = 0;
  /// Unrecoverable state: the nesting limit was hit. There is no sensible
  /// resynchronization point, and continuing would re-descend the full depth
  /// on every remaining token, so all parsing stops.
  bool fatal_ = false;
};

}  // namespace optiforge
