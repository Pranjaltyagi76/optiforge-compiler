#include "optiforge/frontend/Token.h"

namespace optiforge {

std::string_view toString(TokenKind kind) {
  switch (kind) {
    case TokenKind::Identifier:
      return "identifier";
    case TokenKind::IntLiteral:
      return "int_literal";
    case TokenKind::FloatLiteral:
      return "float_literal";
    case TokenKind::KwFn:
      return "kw_fn";
    case TokenKind::KwInt:
      return "kw_int";
    case TokenKind::KwFloat:
      return "kw_float";
    case TokenKind::KwBool:
      return "kw_bool";
    case TokenKind::KwVoid:
      return "kw_void";
    case TokenKind::KwIf:
      return "kw_if";
    case TokenKind::KwElse:
      return "kw_else";
    case TokenKind::KwWhile:
      return "kw_while";
    case TokenKind::KwReturn:
      return "kw_return";
    case TokenKind::KwTrue:
      return "kw_true";
    case TokenKind::KwFalse:
      return "kw_false";
    case TokenKind::Plus:
      return "plus";
    case TokenKind::Minus:
      return "minus";
    case TokenKind::Star:
      return "star";
    case TokenKind::Slash:
      return "slash";
    case TokenKind::Percent:
      return "percent";
    case TokenKind::Bang:
      return "bang";
    case TokenKind::Assign:
      return "assign";
    case TokenKind::EqualEqual:
      return "equal_equal";
    case TokenKind::BangEqual:
      return "bang_equal";
    case TokenKind::Less:
      return "less";
    case TokenKind::Greater:
      return "greater";
    case TokenKind::LessEqual:
      return "less_equal";
    case TokenKind::GreaterEqual:
      return "greater_equal";
    case TokenKind::AmpAmp:
      return "amp_amp";
    case TokenKind::PipePipe:
      return "pipe_pipe";
    case TokenKind::LParen:
      return "l_paren";
    case TokenKind::RParen:
      return "r_paren";
    case TokenKind::LBrace:
      return "l_brace";
    case TokenKind::RBrace:
      return "r_brace";
    case TokenKind::LBracket:
      return "l_bracket";
    case TokenKind::RBracket:
      return "r_bracket";
    case TokenKind::Comma:
      return "comma";
    case TokenKind::Semicolon:
      return "semicolon";
    case TokenKind::Arrow:
      return "arrow";
    case TokenKind::EndOfFile:
      return "eof";
    case TokenKind::Error:
      return "error";
  }
  return "unknown";
}

std::string_view describe(TokenKind kind) {
  switch (kind) {
    case TokenKind::Identifier:
      return "an identifier";
    case TokenKind::IntLiteral:
      return "an integer literal";
    case TokenKind::FloatLiteral:
      return "a floating-point literal";
    case TokenKind::KwFn:
      return "'fn'";
    case TokenKind::KwInt:
      return "'int'";
    case TokenKind::KwFloat:
      return "'float'";
    case TokenKind::KwBool:
      return "'bool'";
    case TokenKind::KwVoid:
      return "'void'";
    case TokenKind::KwIf:
      return "'if'";
    case TokenKind::KwElse:
      return "'else'";
    case TokenKind::KwWhile:
      return "'while'";
    case TokenKind::KwReturn:
      return "'return'";
    case TokenKind::KwTrue:
      return "'true'";
    case TokenKind::KwFalse:
      return "'false'";
    case TokenKind::Plus:
      return "'+'";
    case TokenKind::Minus:
      return "'-'";
    case TokenKind::Star:
      return "'*'";
    case TokenKind::Slash:
      return "'/'";
    case TokenKind::Percent:
      return "'%'";
    case TokenKind::Bang:
      return "'!'";
    case TokenKind::Assign:
      return "'='";
    case TokenKind::EqualEqual:
      return "'=='";
    case TokenKind::BangEqual:
      return "'!='";
    case TokenKind::Less:
      return "'<'";
    case TokenKind::Greater:
      return "'>'";
    case TokenKind::LessEqual:
      return "'<='";
    case TokenKind::GreaterEqual:
      return "'>='";
    case TokenKind::AmpAmp:
      return "'&&'";
    case TokenKind::PipePipe:
      return "'||'";
    case TokenKind::LParen:
      return "'('";
    case TokenKind::RParen:
      return "')'";
    case TokenKind::LBrace:
      return "'{'";
    case TokenKind::RBrace:
      return "'}'";
    case TokenKind::LBracket:
      return "'['";
    case TokenKind::RBracket:
      return "']'";
    case TokenKind::Comma:
      return "','";
    case TokenKind::Semicolon:
      return "';'";
    case TokenKind::Arrow:
      return "'->'";
    case TokenKind::EndOfFile:
      return "end of file";
    case TokenKind::Error:
      return "an invalid token";
  }
  return "an unknown token";
}

bool canStartStatement(TokenKind kind) {
  switch (kind) {
    case TokenKind::KwInt:
    case TokenKind::KwFloat:
    case TokenKind::KwBool:
    case TokenKind::KwVoid:
    case TokenKind::KwIf:
    case TokenKind::KwWhile:
    case TokenKind::KwReturn:
    case TokenKind::LBrace:
      return true;
    default:
      return false;
  }
}

bool isTypeKeyword(TokenKind kind) {
  switch (kind) {
    case TokenKind::KwInt:
    case TokenKind::KwFloat:
    case TokenKind::KwBool:
    case TokenKind::KwVoid:
      return true;
    default:
      return false;
  }
}

}  // namespace optiforge
