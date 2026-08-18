#include "optiforge/frontend/ASTPrinter.h"

#include <charconv>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>

#include "optiforge/frontend/Type.h"

namespace optiforge {

namespace {

/// Shortest representation that round-trips exactly. `operator<<` would round
/// to six significant digits, which is both lossy and a poor thing to pin a
/// golden test to.
std::string formatDouble(double value) {
  char buffer[64];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (result.ec != std::errc{}) {
    return "<unprintable>";
  }
  return std::string(buffer, result.ptr);
}

class AstWriter {
public:
  explicit AstWriter(std::ostream& out) : out_(out) {}

  void writeProgram(const Program& program) {
    line("Program");
    ++depth_;
    for (const FunctionPtr& fn : program.functions()) {
      writeFunction(*fn);
    }
    --depth_;
  }

private:
  void indent() {
    for (int i = 0; i < depth_; ++i) {
      out_ << "  ";
    }
  }

  void line(std::string_view text) {
    indent();
    out_ << text << '\n';
  }

  void writeFunction(const FunctionDecl& fn) {
    std::ostringstream header;
    header << "FunctionDecl '" << fn.name() << "' -> " << toString(fn.returnType());
    line(header.str());

    ++depth_;
    for (const ParamPtr& param : fn.params()) {
      std::ostringstream text;
      text << "ParamDecl '" << param->name() << "' : " << toString(param->declaredType());
      line(text.str());
    }
    writeStmt(fn.body());
    --depth_;
  }

  void writeLabelled(const char* label, const Node* child, bool isExpr) {
    line(label);
    ++depth_;
    if (isExpr) {
      writeExpr(static_cast<const Expr*>(child));
    } else {
      writeStmt(static_cast<const Stmt*>(child));
    }
    --depth_;
  }

  void writeStmt(const Stmt* stmt) {
    if (stmt == nullptr) {
      line("<null-stmt>");
      return;
    }

    switch (stmt->kind()) {
      case Node::Kind::Block: {
        const auto* block = static_cast<const Block*>(stmt);
        line("Block");
        ++depth_;
        for (const StmtPtr& child : block->statements()) {
          writeStmt(child.get());
        }
        --depth_;
        break;
      }

      case Node::Kind::VarDeclStmt: {
        const auto* decl = static_cast<const VarDeclStmt*>(stmt);
        std::ostringstream text;
        text << "VarDeclStmt '" << decl->name() << "' : " << toString(decl->declaredType());
        line(text.str());
        if (decl->init() != nullptr) {
          ++depth_;
          writeExpr(decl->init());
          --depth_;
        }
        break;
      }

      case Node::Kind::AssignStmt: {
        const auto* assign = static_cast<const AssignStmt*>(stmt);
        line("AssignStmt '" + assign->name() + "'");
        ++depth_;
        writeExpr(assign->value());
        --depth_;
        break;
      }

      case Node::Kind::ExprStmt: {
        const auto* exprStmt = static_cast<const ExprStmt*>(stmt);
        line("ExprStmt");
        ++depth_;
        writeExpr(exprStmt->expr());
        --depth_;
        break;
      }

      case Node::Kind::IfStmt: {
        const auto* ifStmt = static_cast<const IfStmt*>(stmt);
        line("IfStmt");
        ++depth_;
        writeLabelled("cond:", ifStmt->cond(), /*isExpr=*/true);
        writeLabelled("then:", ifStmt->thenBlock(), /*isExpr=*/false);
        if (ifStmt->elseBranch() != nullptr) {
          writeLabelled("else:", ifStmt->elseBranch(), /*isExpr=*/false);
        }
        --depth_;
        break;
      }

      case Node::Kind::WhileStmt: {
        const auto* loop = static_cast<const WhileStmt*>(stmt);
        line("WhileStmt");
        ++depth_;
        writeLabelled("cond:", loop->cond(), /*isExpr=*/true);
        writeLabelled("body:", loop->body(), /*isExpr=*/false);
        --depth_;
        break;
      }

      case Node::Kind::ReturnStmt: {
        const auto* ret = static_cast<const ReturnStmt*>(stmt);
        line("ReturnStmt");
        if (ret->value() != nullptr) {
          ++depth_;
          writeExpr(ret->value());
          --depth_;
        }
        break;
      }

      default:
        line("<unexpected-stmt>");
        break;
    }
  }

  /// Suffix showing the resolved type. Empty before semantic analysis, so a
  /// pre-sema dump is unchanged and a post-sema dump is self-describing.
  static std::string typeSuffix(const Expr& expr) {
    return expr.type() != nullptr ? " : " + std::string(expr.type()->name()) : std::string();
  }

  void writeExpr(const Expr* expr) {
    if (expr == nullptr) {
      line("<null-expr>");
      return;
    }
    const std::string suffix = typeSuffix(*expr);

    switch (expr->kind()) {
      case Node::Kind::IntLiteralExpr: {
        const auto* lit = static_cast<const IntLiteralExpr*>(expr);
        line("IntLiteral " + std::to_string(lit->value()) + suffix);
        break;
      }

      case Node::Kind::FloatLiteralExpr: {
        const auto* lit = static_cast<const FloatLiteralExpr*>(expr);
        line("FloatLiteral " + formatDouble(lit->value()) + suffix);
        break;
      }

      case Node::Kind::BoolLiteralExpr: {
        const auto* lit = static_cast<const BoolLiteralExpr*>(expr);
        line((lit->value() ? "BoolLiteral true" : "BoolLiteral false") + suffix);
        break;
      }

      case Node::Kind::VarRefExpr: {
        const auto* ref = static_cast<const VarRefExpr*>(expr);
        line("VarRef '" + ref->name() + "'" + suffix);
        break;
      }

      case Node::Kind::UnaryExpr: {
        const auto* unary = static_cast<const UnaryExpr*>(expr);
        line(std::string("UnaryExpr '") + std::string(toString(unary->op())) + "'" + suffix);
        ++depth_;
        writeExpr(unary->operand());
        --depth_;
        break;
      }

      case Node::Kind::BinaryExpr: {
        const auto* binary = static_cast<const BinaryExpr*>(expr);
        line(std::string("BinaryExpr '") + std::string(toString(binary->op())) + "'" + suffix);
        ++depth_;
        writeExpr(binary->lhs());
        writeExpr(binary->rhs());
        --depth_;
        break;
      }

      case Node::Kind::CallExpr: {
        const auto* call = static_cast<const CallExpr*>(expr);
        line("CallExpr '" + call->callee() + "'" + suffix);
        ++depth_;
        for (const ExprPtr& arg : call->args()) {
          writeExpr(arg.get());
        }
        --depth_;
        break;
      }

      default:
        line("<unexpected-expr>");
        break;
    }
  }

  std::ostream& out_;
  int depth_ = 0;
};

}  // namespace

void printTokens(const std::vector<Token>& tokens, std::ostream& out) {
  for (const Token& token : tokens) {
    std::ostringstream position;
    position << token.loc.line << ':' << token.loc.col;

    out << std::setw(8) << std::right << position.str() << "  " << std::setw(15) << std::left
        << toString(token.kind) << " '" << token.lexeme << '\'';

    if (token.is(TokenKind::IntLiteral)) {
      out << "  value=" << token.intValue;
    } else if (token.is(TokenKind::FloatLiteral)) {
      out << "  value=" << formatDouble(token.floatValue);
    }
    out << '\n';
  }
}

void printAST(const Program& program, std::ostream& out) {
  AstWriter writer(out);
  writer.writeProgram(program);
}

}  // namespace optiforge
