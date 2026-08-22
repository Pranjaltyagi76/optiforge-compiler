#pragma once

#include <string>
#include <string_view>

#include "optiforge/frontend/AST.h"
#include "optiforge/frontend/Symbol.h"
#include "optiforge/frontend/Type.h"

namespace optiforge {

class DiagnosticEngine;

/// Semantic analysis: turns a parsed AST into a *typed* AST.
///
/// Runs in two passes over the program so that functions may call each other in
/// any order, including mutual recursion (requirement FE-27):
///
///   1. Collect every function signature into the global scope.
///   2. Check each body against those signatures.
///
/// Analysis never stops at the first error. A subexpression that fails to type
/// yields a null type, and every consumer treats null as "already reported",
/// so one run reports many independent problems without cascading.
class Sema {
public:
  Sema(DiagnosticEngine& diags, SymbolTable& symbols);

  /// Analyzes and annotates `program` in place. Returns false if any error was
  /// reported.
  ///
  /// `requireEntryPoint` controls the LANG-46 check for a well-formed `main`;
  /// tests that analyze a fragment rather than a whole program pass false.
  bool analyze(Program& program, bool requireEntryPoint = true);

private:
  // --- Pass 1 ---
  void declareBuiltins();
  void collectSignatures(Program& program);
  void checkEntryPoint(const Program& program);

  // --- Pass 2 ---
  void analyzeFunction(FunctionDecl& function);
  void analyzeStmt(Stmt& stmt);
  void analyzeBlock(Block& block, bool ownScope);
  void analyzeVarDecl(VarDeclStmt& decl);
  void analyzeAssign(AssignStmt& assign);
  void analyzeIf(IfStmt& stmt);
  void analyzeWhile(WhileStmt& stmt);
  void analyzeFor(ForStmt& stmt);
  /// How many loops enclose the statement being analyzed. `break` and
  /// `continue` outside every loop have nothing to jump to, and the
  /// frontend is the only place that can say so.
  int loopDepth_ = 0;
  void analyzeReturn(ReturnStmt& stmt);

  /// Analyzes `expr`, records its type on the node, and returns that type.
  /// Null means the expression failed to type and a diagnostic was issued.
  const Type* analyzeExpr(Expr& expr);
  const Type* analyzeBinary(BinaryExpr& expr);
  const Type* analyzeUnary(UnaryExpr& expr);
  const Type* analyzeCall(CallExpr& expr);
  const Type* analyzeVarRef(VarRefExpr& expr);
  const Type* analyzeIndex(IndexExpr& expr);

  /// Checks an expression used as an `if`/`while` condition. Requires exactly
  /// `bool`: there is no int-to-bool conversion (FE-29, LANG-32).
  void requireBoolCondition(Expr& cond, std::string_view construct);

  /// True when every path through `stmt` reaches a return (LANG-47).
  static bool returnsOnAllPaths(const Stmt* stmt);

  void error(SourceRange range, const std::string& message);
  void warning(SourceRange range, const std::string& message);
  void note(SourceRange range, const std::string& message);

  DiagnosticEngine& diags_;
  SymbolTable& symbols_;
  Symbol* currentFunction_ = nullptr;
  bool hadError_ = false;
};

}  // namespace optiforge
