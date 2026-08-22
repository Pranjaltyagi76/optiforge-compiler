#include "optiforge/frontend/Parser.h"

#include <utility>

#include "optiforge/support/Diagnostic.h"

namespace optiforge {

namespace {

/// Binding power of a binary operator; higher binds tighter. 0 means "not a
/// binary operator", which terminates the precedence-climbing loop.
int precedenceOf(TokenKind kind) {
  switch (kind) {
    case TokenKind::PipePipe:
      return 1;
    case TokenKind::AmpAmp:
      return 2;
    case TokenKind::EqualEqual:
    case TokenKind::BangEqual:
      return 3;
    case TokenKind::Less:
    case TokenKind::Greater:
    case TokenKind::LessEqual:
    case TokenKind::GreaterEqual:
      return 4;
    case TokenKind::Plus:
    case TokenKind::Minus:
      return 5;
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Percent:
      return 6;
    default:
      return 0;
  }
}

BinaryOp binaryOpOf(TokenKind kind) {
  switch (kind) {
    case TokenKind::Plus:
      return BinaryOp::Add;
    case TokenKind::Minus:
      return BinaryOp::Sub;
    case TokenKind::Star:
      return BinaryOp::Mul;
    case TokenKind::Slash:
      return BinaryOp::Div;
    case TokenKind::Percent:
      return BinaryOp::Mod;
    case TokenKind::EqualEqual:
      return BinaryOp::Eq;
    case TokenKind::BangEqual:
      return BinaryOp::Ne;
    case TokenKind::Less:
      return BinaryOp::Lt;
    case TokenKind::Greater:
      return BinaryOp::Gt;
    case TokenKind::LessEqual:
      return BinaryOp::Le;
    case TokenKind::GreaterEqual:
      return BinaryOp::Ge;
    case TokenKind::AmpAmp:
      return BinaryOp::And;
    case TokenKind::PipePipe:
      return BinaryOp::Or;
    default:
      return BinaryOp::Add;  // unreachable: guarded by precedenceOf
  }
}

TypeSpec typeSpecOf(TokenKind kind) {
  switch (kind) {
    case TokenKind::KwInt:
      return TypeSpec::Int;
    case TokenKind::KwFloat:
      return TypeSpec::Float;
    case TokenKind::KwBool:
      return TypeSpec::Bool;
    default:
      return TypeSpec::Void;
  }
}

SourceRange spanning(SourceLocation begin, SourceLocation end) { return {begin, end}; }

}  // namespace

Parser::Parser(const std::vector<Token>& tokens, DiagnosticEngine& diags)
    : tokens_(tokens), diags_(diags) {}

// ---------------------------------------------------------------------------
// Token access
// ---------------------------------------------------------------------------

const Token& Parser::peek(std::size_t ahead) const {
  const std::size_t index = current_ + ahead;
  // The token stream always ends with exactly one EndOfFile, so clamping here
  // makes lookahead past the end safe rather than undefined.
  return index < tokens_.size() ? tokens_[index] : tokens_.back();
}

const Token& Parser::previous() const {
  return current_ > 0 ? tokens_[current_ - 1] : tokens_.front();
}

bool Parser::check(TokenKind kind) const { return peek().is(kind); }

bool Parser::isAtEnd() const { return peek().is(TokenKind::EndOfFile); }

const Token& Parser::advance() {
  if (!isAtEnd()) {
    ++current_;
  }
  return previous();
}

bool Parser::match(TokenKind kind) {
  if (!check(kind)) {
    return false;
  }
  advance();
  return true;
}

const Token* Parser::expect(TokenKind kind, std::string_view context) {
  if (check(kind)) {
    return &advance();
  }
  errorAt(peek(), "expected " + std::string(describe(kind)) + " " + std::string(context));
  return nullptr;
}

// ---------------------------------------------------------------------------
// Diagnostics and recovery
// ---------------------------------------------------------------------------

void Parser::errorAt(const Token& token, const std::string& message) {
  hadError_ = true;
  // One stray token can make every following production fail. Report the first
  // problem and stay quiet until recovery reaches a known-good boundary.
  //
  // `fatal_` is checked separately because unwinding from the nesting limit
  // passes through synchronize(), which clears panicMode_ and would otherwise
  // let a spurious "expected '}'" escape on the way out.
  if (panicMode_ || fatal_) {
    return;
  }
  panicMode_ = true;
  diags_.report(token.range(), DiagSeverity::Error, message);
}

const Token* Parser::expectSemicolon(std::string_view context) {
  if (check(TokenKind::Semicolon)) {
    return &advance();
  }

  // Report just past the previous token rather than at the token that revealed
  // the problem. A missing semicolon is usually noticed on the *next* line, and
  // pointing there sends the reader to the wrong place.
  hadError_ = true;
  if (!panicMode_ && !fatal_) {
    panicMode_ = true;
    diags_.report(makeRange(previous().endLoc()), DiagSeverity::Error,
                  "expected ';' " + std::string(context));
  }
  return nullptr;
}

bool Parser::checkDepth(const DepthGuard& guard) {
  if (fatal_) {
    return true;  // already unwinding
  }
  if (!guard.exceeded()) {
    return false;
  }
  errorAt(peek(), "input nests too deeply (limit " + std::to_string(kMaxNestingDepth) +
                      "); simplify the expression or block structure");
  fatal_ = true;
  return true;
}

void Parser::synchronizeToTopLevel() {
  // At file scope only 'fn' can legitimately begin a declaration, so anything
  // else is skipped outright. Using the statement-level synchronize() here
  // would stop at every 'int'/'if'/'return' left over from the broken
  // declaration and report a fresh error at each one.
  panicMode_ = false;
  while (!isAtEnd() && !check(TokenKind::KwFn)) {
    advance();
  }
}

void Parser::synchronize() {
  panicMode_ = false;

  while (!isAtEnd()) {
    // A semicolon just consumed ends a statement: the next token starts a new
    // one, so this is a safe place to resume.
    if (previous().is(TokenKind::Semicolon)) {
      return;
    }
    // Let the enclosing block or program loop decide what to do with these.
    if (check(TokenKind::RBrace) || check(TokenKind::KwFn)) {
      return;
    }
    if (canStartStatement(peek().kind)) {
      return;
    }
    advance();
  }
}

// ---------------------------------------------------------------------------
// Program and declarations
// ---------------------------------------------------------------------------

std::unique_ptr<Program> Parser::parseProgram() {
  const SourceLocation begin = peek().loc;
  std::vector<FunctionPtr> functions;

  while (!isAtEnd() && !fatal_) {
    const std::size_t before = current_;

    if (check(TokenKind::KwFn)) {
      if (FunctionPtr fn = parseFunctionDecl()) {
        functions.push_back(std::move(fn));
      } else {
        synchronizeToTopLevel();
      }
    } else {
      errorAt(peek(), "expected 'fn' to begin a function declaration");
      synchronizeToTopLevel();
    }

    // Hard guarantee of forward progress. Without this, a token that neither
    // parses nor triggers a synchronize() advance would spin forever, and
    // CLI-10 requires that malformed input never hangs the compiler.
    if (current_ == before) {
      advance();
    }
  }

  return std::make_unique<Program>(std::move(functions), spanning(begin, peek().loc));
}

bool Parser::parseTypeSpec(TypeSpec& out) {
  if (isTypeKeyword(peek().kind)) {
    out = typeSpecOf(advance().kind);
    return true;
  }
  errorAt(peek(), "expected a type name ('int', 'float', 'bool', or 'void')");
  return false;
}

bool Parser::parseParamList(std::vector<ParamPtr>& out) {
  do {
    const SourceLocation begin = peek().loc;

    TypeSpec type = TypeSpec::Void;
    if (!parseTypeSpec(type)) {
      return false;
    }

    const Token* name = expect(TokenKind::Identifier, "in parameter declaration");
    if (name == nullptr) {
      return false;
    }

    out.push_back(std::make_unique<ParamDecl>(type, std::string(name->lexeme),
                                              spanning(begin, name->endLoc())));
  } while (match(TokenKind::Comma));

  return true;
}

FunctionPtr Parser::parseFunctionDecl() {
  const SourceLocation begin = peek().loc;
  advance();  // 'fn'

  const Token* name = expect(TokenKind::Identifier, "after 'fn'");
  if (name == nullptr) {
    return nullptr;
  }
  const SourceRange nameRange = name->range();
  std::string functionName(name->lexeme);

  if (expect(TokenKind::LParen, "after function name") == nullptr) {
    return nullptr;
  }

  std::vector<ParamPtr> params;
  if (!check(TokenKind::RParen)) {
    if (!parseParamList(params)) {
      return nullptr;
    }
  }

  if (expect(TokenKind::RParen, "after parameter list") == nullptr) {
    return nullptr;
  }

  // An omitted return type means void, matching the grammar in
  // context/System_design.md 3.2.
  TypeSpec returnType = TypeSpec::Void;
  if (match(TokenKind::Arrow)) {
    if (!parseTypeSpec(returnType)) {
      return nullptr;
    }
  }

  BlockPtr body = parseBlock();
  if (body == nullptr) {
    return nullptr;
  }

  const SourceLocation end = body->range().end;
  return std::make_unique<FunctionDecl>(std::move(functionName), nameRange, std::move(params),
                                        returnType, std::move(body), spanning(begin, end));
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

BlockPtr Parser::parseBlock() {
  DepthGuard guard(*this);
  if (checkDepth(guard)) {
    return nullptr;
  }

  const Token* open = expect(TokenKind::LBrace, "to begin a block");
  if (open == nullptr) {
    return nullptr;
  }
  const SourceLocation begin = open->loc;

  std::vector<StmtPtr> statements;
  while (!check(TokenKind::RBrace) && !isAtEnd() && !fatal_) {
    const std::size_t before = current_;

    if (StmtPtr stmt = parseStatement()) {
      statements.push_back(std::move(stmt));
    } else {
      synchronize();
    }

    if (current_ == before) {
      advance();  // forward-progress guarantee, as in parseProgram
    }
  }

  const Token* close = expect(TokenKind::RBrace, "to close a block");
  const SourceLocation end = close != nullptr ? close->endLoc() : previous().endLoc();

  return std::make_unique<Block>(std::move(statements), spanning(begin, end));
}

StmtPtr Parser::parseStatement() {
  if (isTypeKeyword(peek().kind)) {
    return parseVarDecl();
  }
  if (check(TokenKind::KwIf)) {
    return parseIfStmt();
  }
  if (check(TokenKind::KwWhile)) {
    return parseWhileStmt();
  }
  if (check(TokenKind::KwFor)) {
    return parseForStmt();
  }
  if (check(TokenKind::KwBreak) || check(TokenKind::KwContinue)) {
    const bool isBreak = check(TokenKind::KwBreak);
    const SourceLocation begin = peek().loc;
    advance();
    const Token* semi = expectSemicolon(isBreak ? "after 'break'" : "after 'continue'");
    if (semi == nullptr) {
      return nullptr;
    }
    const SourceRange range = spanning(begin, semi->endLoc());
    if (isBreak) {
      return std::make_unique<BreakStmt>(range);
    }
    return std::make_unique<ContinueStmt>(range);
  }
  if (check(TokenKind::KwReturn)) {
    return parseReturnStmt();
  }
  if (check(TokenKind::LBrace)) {
    return parseBlock();
  }
  return parseAssignOrExprStmt();
}

StmtPtr Parser::parseVarDecl() {
  const SourceLocation begin = peek().loc;

  TypeSpec type = TypeSpec::Void;
  if (!parseTypeSpec(type)) {
    return nullptr;
  }

  const Token* name = expect(TokenKind::Identifier, "in variable declaration");
  if (name == nullptr) {
    return nullptr;
  }
  const SourceRange nameRange = name->range();
  std::string varName(name->lexeme);

  // `int a[10];` -- the length is a literal, not an expression. A constant
  // expression would need folding before sema, and there is no case for it
  // until the language has named constants.
  unsigned arrayLength = 0;
  if (match(TokenKind::LBracket)) {
    const Token* lengthToken = expect(TokenKind::IntLiteral, "as an array length");
    if (lengthToken == nullptr) {
      return nullptr;
    }
    if (lengthToken->intValue <= 0) {
      // Through the parser's own error path, not the diagnostic engine
      // directly: `hadError()` is what tells the driver a parse failed, and
      // reporting round it would leave a program that looks well-formed.
      errorAt(*lengthToken, "array length must be positive");
      return nullptr;
    }
    arrayLength = static_cast<unsigned>(lengthToken->intValue);
    if (expect(TokenKind::RBracket, "after an array length") == nullptr) {
      return nullptr;
    }
  }

  ExprPtr init;
  if (match(TokenKind::Assign)) {
    init = parseExpression();
    if (init == nullptr) {
      return nullptr;
    }
  }

  const Token* semi = expectSemicolon("after variable declaration");
  if (semi == nullptr) {
    return nullptr;
  }

  return std::make_unique<VarDeclStmt>(type, std::move(varName), nameRange, std::move(init),
                                       spanning(begin, semi->endLoc()), arrayLength);
}

StmtPtr Parser::parseIfStmt() {
  DepthGuard guard(*this);
  if (checkDepth(guard)) {
    return nullptr;
  }

  const SourceLocation begin = peek().loc;
  advance();  // 'if'

  if (expect(TokenKind::LParen, "after 'if'") == nullptr) {
    return nullptr;
  }
  ExprPtr cond = parseExpression();
  if (cond == nullptr) {
    return nullptr;
  }
  if (expect(TokenKind::RParen, "after the 'if' condition") == nullptr) {
    return nullptr;
  }

  BlockPtr thenBlock = parseBlock();
  if (thenBlock == nullptr) {
    return nullptr;
  }

  StmtPtr elseBranch;
  if (match(TokenKind::KwElse)) {
    // `else if` chains as a nested IfStmt rather than a block, so the AST
    // mirrors the source instead of inventing a wrapper block.
    if (check(TokenKind::KwIf)) {
      elseBranch = parseIfStmt();
    } else {
      elseBranch = parseBlock();
    }
    if (elseBranch == nullptr) {
      return nullptr;
    }
  }

  const SourceLocation end =
      elseBranch != nullptr ? elseBranch->range().end : thenBlock->range().end;
  return std::make_unique<IfStmt>(std::move(cond), std::move(thenBlock), std::move(elseBranch),
                                  spanning(begin, end));
}

/// One clause of a `for` header: a declaration, an assignment, or nothing.
///
/// Reuses `parseVarDecl` and `parseAssignOrExprStmt`, which consume their own
/// semicolon -- so the init clause's `;` is theirs, and the step clause, which
/// has no trailing `;`, is parsed here instead of borrowing them.
StmtPtr Parser::parseForClause(bool isInit) {
  if (isInit) {
    if (isTypeKeyword(peek().kind)) {
      return parseVarDecl();  // consumes the ';'
    }
    if (check(TokenKind::Semicolon)) {
      advance();
      return nullptr;
    }
    return parseAssignOrExprStmt();  // consumes the ';'
  }

  // The step clause is followed by ')', not ';', so it cannot use the helpers
  // above. Only an assignment is allowed, which is the only useful thing to
  // write there in a language without ++ or compound assignment.
  if (check(TokenKind::RParen)) {
    return nullptr;
  }
  const SourceLocation begin = peek().loc;
  const Token* name = expect(TokenKind::Identifier, "in the 'for' step clause");
  if (name == nullptr) {
    return nullptr;
  }
  const SourceRange nameRange = name->range();

  ExprPtr index;
  if (match(TokenKind::LBracket)) {
    index = parseExpression();
    if (index == nullptr || expect(TokenKind::RBracket, "after an array index") == nullptr) {
      return nullptr;
    }
  }
  if (expect(TokenKind::Assign, "in the 'for' step clause") == nullptr) {
    return nullptr;
  }
  ExprPtr value = parseExpression();
  if (value == nullptr) {
    return nullptr;
  }
  return std::make_unique<AssignStmt>(std::string(name->lexeme), nameRange,
                                      std::move(value),
                                      spanning(begin, value->range().end),
                                      std::move(index));
}

StmtPtr Parser::parseForStmt() {
  const SourceLocation begin = peek().loc;
  advance();  // 'for'

  if (expect(TokenKind::LParen, "after 'for'") == nullptr) {
    return nullptr;
  }

  StmtPtr init = parseForClause(/*isInit=*/true);
  if (init == nullptr && !previous().is(TokenKind::Semicolon)) {
    return nullptr;  // the clause failed rather than being empty
  }

  ExprPtr cond;
  if (!check(TokenKind::Semicolon)) {
    cond = parseExpression();
    if (cond == nullptr) {
      return nullptr;
    }
  }
  if (expect(TokenKind::Semicolon, "after the 'for' condition") == nullptr) {
    return nullptr;
  }

  StmtPtr step = parseForClause(/*isInit=*/false);
  if (step == nullptr && !check(TokenKind::RParen)) {
    return nullptr;
  }
  if (expect(TokenKind::RParen, "after the 'for' clauses") == nullptr) {
    return nullptr;
  }

  BlockPtr body = parseBlock();
  if (body == nullptr) {
    return nullptr;
  }

  const SourceLocation end = body->range().end;
  return std::make_unique<ForStmt>(std::move(init), std::move(cond), std::move(step),
                                   std::move(body), spanning(begin, end));
}

StmtPtr Parser::parseWhileStmt() {
  const SourceLocation begin = peek().loc;
  advance();  // 'while'

  if (expect(TokenKind::LParen, "after 'while'") == nullptr) {
    return nullptr;
  }
  ExprPtr cond = parseExpression();
  if (cond == nullptr) {
    return nullptr;
  }
  if (expect(TokenKind::RParen, "after the 'while' condition") == nullptr) {
    return nullptr;
  }

  BlockPtr body = parseBlock();
  if (body == nullptr) {
    return nullptr;
  }

  const SourceLocation end = body->range().end;
  return std::make_unique<WhileStmt>(std::move(cond), std::move(body), spanning(begin, end));
}

StmtPtr Parser::parseReturnStmt() {
  const SourceLocation begin = peek().loc;
  advance();  // 'return'

  ExprPtr value;
  if (!check(TokenKind::Semicolon)) {
    value = parseExpression();
    if (value == nullptr) {
      return nullptr;
    }
  }

  const Token* semi = expectSemicolon("after a return statement");
  if (semi == nullptr) {
    return nullptr;
  }

  return std::make_unique<ReturnStmt>(std::move(value), spanning(begin, semi->endLoc()));
}

StmtPtr Parser::parseAssignOrExprStmt() {
  const SourceLocation begin = peek().loc;

  // Assignment is a statement, not an expression, so one token of lookahead
  // distinguishes it without any backtracking.
  if (check(TokenKind::Identifier) &&
      (peek(1).is(TokenKind::Assign) || peek(1).is(TokenKind::LBracket))) {
    // Two tokens of lookahead now, still no backtracking: an identifier
    // followed by '=' is a scalar assignment, and one followed by '[' is an
    // indexed assignment *or* an index expression used as a statement. Only
    // the first two are assignments, so `a[i]` alone falls through to the
    // expression path below by rewinding to where this started.
    const std::size_t rewind = current_;
    const Token& name = advance();
    const SourceRange nameRange = name.range();

    ExprPtr index;
    if (match(TokenKind::LBracket)) {
      index = parseExpression();
      if (index == nullptr) {
        return nullptr;
      }
      if (expect(TokenKind::RBracket, "after an array index") == nullptr) {
        return nullptr;
      }
    }

    if (check(TokenKind::Assign)) {
      advance();  // '='
      ExprPtr value = parseExpression();
      if (value == nullptr) {
        return nullptr;
      }
      const Token* semi = expectSemicolon("after an assignment");
      if (semi == nullptr) {
        return nullptr;
      }
      return std::make_unique<AssignStmt>(std::string(name.lexeme), nameRange,
                                          std::move(value),
                                          spanning(begin, semi->endLoc()),
                                          std::move(index));
    }

    // Not an assignment after all -- `a[i];` as an expression statement. Nothing
    // was reported while scanning the index, so rewinding costs a re-parse and
    // no duplicate diagnostics.
    current_ = rewind;
  }

  ExprPtr expr = parseExpression();
  if (expr == nullptr) {
    return nullptr;
  }
  const Token* semi = expectSemicolon("after an expression statement");
  if (semi == nullptr) {
    return nullptr;
  }
  return std::make_unique<ExprStmt>(std::move(expr), spanning(begin, semi->endLoc()));
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

ExprPtr Parser::parseExpression() { return parseBinaryExpr(1); }

ExprPtr Parser::parseBinaryExpr(int minPrecedence) {
  ExprPtr lhs = parseUnaryExpr();
  if (lhs == nullptr) {
    return nullptr;
  }

  while (true) {
    const int precedence = precedenceOf(peek().kind);
    if (precedence < minPrecedence) {
      break;
    }

    const BinaryOp op = binaryOpOf(peek().kind);
    advance();

    // precedence + 1 makes every operator left-associative: the right operand
    // may only contain strictly tighter-binding operators.
    ExprPtr rhs = parseBinaryExpr(precedence + 1);
    if (rhs == nullptr) {
      return nullptr;
    }

    const SourceRange range = spanning(lhs->range().begin, rhs->range().end);
    lhs = std::make_unique<BinaryExpr>(op, std::move(lhs), std::move(rhs), range);
  }

  return lhs;
}

ExprPtr Parser::parseUnaryExpr() {
  DepthGuard guard(*this);
  if (checkDepth(guard)) {
    return nullptr;
  }

  if (check(TokenKind::Minus) || check(TokenKind::Bang)) {
    const Token& opToken = advance();
    const UnaryOp op = opToken.is(TokenKind::Minus) ? UnaryOp::Neg : UnaryOp::Not;

    ExprPtr operand = parseUnaryExpr();
    if (operand == nullptr) {
      return nullptr;
    }
    return std::make_unique<UnaryExpr>(op, std::move(operand),
                                       spanning(opToken.loc, operand->range().end));
  }
  return parsePrimaryExpr();
}

ExprPtr Parser::parsePrimaryExpr() {
  DepthGuard guard(*this);
  if (checkDepth(guard)) {
    return nullptr;
  }

  const Token& token = peek();

  switch (token.kind) {
    case TokenKind::IntLiteral:
      advance();
      return std::make_unique<IntLiteralExpr>(token.intValue, token.range());

    case TokenKind::FloatLiteral:
      advance();
      return std::make_unique<FloatLiteralExpr>(token.floatValue, token.range());

    case TokenKind::KwTrue:
    case TokenKind::KwFalse:
      advance();
      return std::make_unique<BoolLiteralExpr>(token.is(TokenKind::KwTrue), token.range());

    case TokenKind::Identifier: {
      advance();
      if (check(TokenKind::LBracket)) {
        advance();  // '['
        ExprPtr index = parseExpression();
        if (index == nullptr) {
          return nullptr;
        }
        const Token* close = expect(TokenKind::RBracket, "after an array index");
        if (close == nullptr) {
          return nullptr;
        }
        return std::make_unique<IndexExpr>(std::string(token.lexeme), token.range(),
                                           std::move(index),
                                           spanning(token.loc, close->endLoc()));
      }
      if (!check(TokenKind::LParen)) {
        return std::make_unique<VarRefExpr>(std::string(token.lexeme), token.range());
      }

      advance();  // '('
      std::vector<ExprPtr> args;
      if (!check(TokenKind::RParen)) {
        do {
          ExprPtr arg = parseExpression();
          if (arg == nullptr) {
            return nullptr;
          }
          args.push_back(std::move(arg));
        } while (match(TokenKind::Comma));
      }

      const Token* close = expect(TokenKind::RParen, "after the argument list");
      if (close == nullptr) {
        return nullptr;
      }
      return std::make_unique<CallExpr>(std::string(token.lexeme), std::move(args),
                                        spanning(token.loc, close->endLoc()));
    }

    case TokenKind::LParen: {
      advance();
      ExprPtr inner = parseExpression();
      if (inner == nullptr) {
        return nullptr;
      }
      const Token* close = expect(TokenKind::RParen, "after a parenthesized expression");
      if (close == nullptr) {
        return nullptr;
      }
      // Keep the parentheses in the range but not in the tree: precedence is
      // already encoded by the shape, so a ParenExpr node would add nothing.
      inner->setRange(spanning(token.loc, close->endLoc()));
      return inner;
    }

    default:
      errorAt(token, "expected an expression");
      return nullptr;
  }
}

}  // namespace optiforge
