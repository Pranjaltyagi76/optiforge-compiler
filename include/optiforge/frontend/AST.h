#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "optiforge/support/SourceLocation.h"

namespace optiforge {

class Type;
struct Symbol;

/// A type as *written* in the source. Purely syntactic; Phase 2 resolves these
/// to semantic types and records the result on each expression.
enum class TypeSpec : std::uint8_t { Int, Float, Bool, Void };

std::string_view toString(TypeSpec spec);

enum class BinaryOp : std::uint8_t {
  Add, Sub, Mul, Div, Mod,
  Eq, Ne, Lt, Gt, Le, Ge,
  And, Or,
};

enum class UnaryOp : std::uint8_t { Neg, Not };

/// Source spelling of an operator, used in AST dumps and diagnostics.
std::string_view toString(BinaryOp op);
std::string_view toString(UnaryOp op);

// ---------------------------------------------------------------------------
// Node hierarchy
//
// Ownership is via std::unique_ptr, as the project brief and roadmap Phase 1
// specify. architectural_design.md 7.2 anticipated arena allocation; that was
// deferred because unique_ptr imposes no trivially-destructible constraint and
// the AST is nowhere near large enough for allocation cost to matter against
// NFR-01. Revisit only if compile-time measurements (metric P-01) say so.
// ---------------------------------------------------------------------------

class Node {
public:
  enum class Kind : std::uint8_t {
    // Declarations
    Program,
    FunctionDecl,
    ParamDecl,
    // Statements
    Block,
    VarDeclStmt,
    AssignStmt,
    ExprStmt,
    IfStmt,
    WhileStmt,
    ReturnStmt,
    // Expressions
    BinaryExpr,
    UnaryExpr,
    CallExpr,
    VarRefExpr,
    IntLiteralExpr,
    FloatLiteralExpr,
    BoolLiteralExpr,
  };

  Node(Kind kind, SourceRange range) : kind_(kind), range_(range) {}
  virtual ~Node() = default;

  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;

  Kind kind() const { return kind_; }
  SourceRange range() const { return range_; }
  SourceLocation loc() const { return range_.begin; }
  void setRange(SourceRange range) { range_ = range; }

private:
  Kind kind_;
  SourceRange range_;
};

class Expr : public Node {
public:
  using Node::Node;

  /// Resolved type. Null until semantic analysis, and null afterwards only for
  /// subexpressions whose analysis failed.
  const Type* type() const { return type_; }
  void setType(const Type* type) { type_ = type; }

private:
  const Type* type_ = nullptr;
};

class Stmt : public Node {
public:
  using Node::Node;
};

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

// --- Expressions -----------------------------------------------------------

class IntLiteralExpr final : public Expr {
public:
  IntLiteralExpr(std::int64_t value, SourceRange range)
      : Expr(Kind::IntLiteralExpr, range), value_(value) {}
  std::int64_t value() const { return value_; }

private:
  std::int64_t value_;
};

class FloatLiteralExpr final : public Expr {
public:
  FloatLiteralExpr(double value, SourceRange range)
      : Expr(Kind::FloatLiteralExpr, range), value_(value) {}
  double value() const { return value_; }

private:
  double value_;
};

class BoolLiteralExpr final : public Expr {
public:
  BoolLiteralExpr(bool value, SourceRange range)
      : Expr(Kind::BoolLiteralExpr, range), value_(value) {}
  bool value() const { return value_; }

private:
  bool value_;
};

class VarRefExpr final : public Expr {
public:
  VarRefExpr(std::string name, SourceRange range)
      : Expr(Kind::VarRefExpr, range), name_(std::move(name)) {}
  const std::string& name() const { return name_; }

  /// Resolved declaration. Null until semantic analysis.
  Symbol* symbol() const { return symbol_; }
  void setSymbol(Symbol* symbol) { symbol_ = symbol; }

private:
  std::string name_;
  Symbol* symbol_ = nullptr;
};

class UnaryExpr final : public Expr {
public:
  UnaryExpr(UnaryOp op, ExprPtr operand, SourceRange range)
      : Expr(Kind::UnaryExpr, range), op_(op), operand_(std::move(operand)) {}
  UnaryOp op() const { return op_; }
  const Expr* operand() const { return operand_.get(); }
  Expr* operand() { return operand_.get(); }

private:
  UnaryOp op_;
  ExprPtr operand_;
};

class BinaryExpr final : public Expr {
public:
  BinaryExpr(BinaryOp op, ExprPtr lhs, ExprPtr rhs, SourceRange range)
      : Expr(Kind::BinaryExpr, range), op_(op), lhs_(std::move(lhs)), rhs_(std::move(rhs)) {}
  BinaryOp op() const { return op_; }
  const Expr* lhs() const { return lhs_.get(); }
  const Expr* rhs() const { return rhs_.get(); }
  Expr* lhs() { return lhs_.get(); }
  Expr* rhs() { return rhs_.get(); }

private:
  BinaryOp op_;
  ExprPtr lhs_;
  ExprPtr rhs_;
};

class CallExpr final : public Expr {
public:
  CallExpr(std::string callee, std::vector<ExprPtr> args, SourceRange range)
      : Expr(Kind::CallExpr, range), callee_(std::move(callee)), args_(std::move(args)) {}
  const std::string& callee() const { return callee_; }
  const std::vector<ExprPtr>& args() const { return args_; }
  std::vector<ExprPtr>& args() { return args_; }

  /// Resolved callee. Null until semantic analysis.
  Symbol* symbol() const { return symbol_; }
  void setSymbol(Symbol* symbol) { symbol_ = symbol; }

private:
  std::string callee_;
  std::vector<ExprPtr> args_;
  Symbol* symbol_ = nullptr;
};

// --- Statements ------------------------------------------------------------

class Block final : public Stmt {
public:
  Block(std::vector<StmtPtr> stmts, SourceRange range)
      : Stmt(Kind::Block, range), stmts_(std::move(stmts)) {}
  const std::vector<StmtPtr>& statements() const { return stmts_; }
  std::vector<StmtPtr>& statements() { return stmts_; }

private:
  std::vector<StmtPtr> stmts_;
};

using BlockPtr = std::unique_ptr<Block>;

class VarDeclStmt final : public Stmt {
public:
  VarDeclStmt(TypeSpec type, std::string name, SourceRange nameRange, ExprPtr init,
              SourceRange range)
      : Stmt(Kind::VarDeclStmt, range),
        type_(type),
        name_(std::move(name)),
        nameRange_(nameRange),
        init_(std::move(init)) {}

  TypeSpec declaredType() const { return type_; }
  const std::string& name() const { return name_; }
  SourceRange nameRange() const { return nameRange_; }
  /// Null when the declaration has no initializer.
  const Expr* init() const { return init_.get(); }
  Expr* init() { return init_.get(); }

  Symbol* symbol() const { return symbol_; }
  void setSymbol(Symbol* symbol) { symbol_ = symbol; }

private:
  TypeSpec type_;
  std::string name_;
  SourceRange nameRange_;
  ExprPtr init_;
  Symbol* symbol_ = nullptr;
};

class AssignStmt final : public Stmt {
public:
  AssignStmt(std::string name, SourceRange nameRange, ExprPtr value, SourceRange range)
      : Stmt(Kind::AssignStmt, range),
        name_(std::move(name)),
        nameRange_(nameRange),
        value_(std::move(value)) {}

  const std::string& name() const { return name_; }
  SourceRange nameRange() const { return nameRange_; }
  const Expr* value() const { return value_.get(); }
  Expr* value() { return value_.get(); }

  Symbol* symbol() const { return symbol_; }
  void setSymbol(Symbol* symbol) { symbol_ = symbol; }

private:
  std::string name_;
  SourceRange nameRange_;
  ExprPtr value_;
  Symbol* symbol_ = nullptr;
};

class ExprStmt final : public Stmt {
public:
  ExprStmt(ExprPtr expr, SourceRange range)
      : Stmt(Kind::ExprStmt, range), expr_(std::move(expr)) {}
  const Expr* expr() const { return expr_.get(); }
  Expr* expr() { return expr_.get(); }

private:
  ExprPtr expr_;
};

class IfStmt final : public Stmt {
public:
  IfStmt(ExprPtr cond, BlockPtr thenBlock, StmtPtr elseBranch, SourceRange range)
      : Stmt(Kind::IfStmt, range),
        cond_(std::move(cond)),
        then_(std::move(thenBlock)),
        else_(std::move(elseBranch)) {}

  const Expr* cond() const { return cond_.get(); }
  const Block* thenBlock() const { return then_.get(); }
  /// Null, a Block, or a nested IfStmt for `else if`.
  const Stmt* elseBranch() const { return else_.get(); }
  Expr* cond() { return cond_.get(); }
  Block* thenBlock() { return then_.get(); }
  Stmt* elseBranch() { return else_.get(); }

private:
  ExprPtr cond_;
  BlockPtr then_;
  StmtPtr else_;
};

class WhileStmt final : public Stmt {
public:
  WhileStmt(ExprPtr cond, BlockPtr body, SourceRange range)
      : Stmt(Kind::WhileStmt, range), cond_(std::move(cond)), body_(std::move(body)) {}

  const Expr* cond() const { return cond_.get(); }
  const Block* body() const { return body_.get(); }
  Expr* cond() { return cond_.get(); }
  Block* body() { return body_.get(); }

private:
  ExprPtr cond_;
  BlockPtr body_;
};

class ReturnStmt final : public Stmt {
public:
  ReturnStmt(ExprPtr value, SourceRange range)
      : Stmt(Kind::ReturnStmt, range), value_(std::move(value)) {}
  /// Null for a bare `return;`.
  const Expr* value() const { return value_.get(); }
  Expr* value() { return value_.get(); }

private:
  ExprPtr value_;
};

// --- Declarations ----------------------------------------------------------

class ParamDecl final : public Node {
public:
  ParamDecl(TypeSpec type, std::string name, SourceRange range)
      : Node(Kind::ParamDecl, range), type_(type), name_(std::move(name)) {}
  TypeSpec declaredType() const { return type_; }
  const std::string& name() const { return name_; }

  Symbol* symbol() const { return symbol_; }
  void setSymbol(Symbol* symbol) { symbol_ = symbol; }

private:
  TypeSpec type_;
  std::string name_;
  Symbol* symbol_ = nullptr;
};

using ParamPtr = std::unique_ptr<ParamDecl>;

class FunctionDecl final : public Node {
public:
  FunctionDecl(std::string name, SourceRange nameRange, std::vector<ParamPtr> params,
               TypeSpec returnType, BlockPtr body, SourceRange range)
      : Node(Kind::FunctionDecl, range),
        name_(std::move(name)),
        nameRange_(nameRange),
        params_(std::move(params)),
        returnType_(returnType),
        body_(std::move(body)) {}

  const std::string& name() const { return name_; }
  SourceRange nameRange() const { return nameRange_; }
  const std::vector<ParamPtr>& params() const { return params_; }
  std::vector<ParamPtr>& params() { return params_; }
  TypeSpec returnType() const { return returnType_; }
  const Block* body() const { return body_.get(); }
  Block* body() { return body_.get(); }

  Symbol* symbol() const { return symbol_; }
  void setSymbol(Symbol* symbol) { symbol_ = symbol; }

private:
  std::string name_;
  SourceRange nameRange_;
  std::vector<ParamPtr> params_;
  TypeSpec returnType_;
  BlockPtr body_;
  Symbol* symbol_ = nullptr;
};

using FunctionPtr = std::unique_ptr<FunctionDecl>;

class Program final : public Node {
public:
  Program(std::vector<FunctionPtr> functions, SourceRange range)
      : Node(Kind::Program, range), functions_(std::move(functions)) {}
  const std::vector<FunctionPtr>& functions() const { return functions_; }
  std::vector<FunctionPtr>& functions() { return functions_; }

private:
  std::vector<FunctionPtr> functions_;
};

}  // namespace optiforge
