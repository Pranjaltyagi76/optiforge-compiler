#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "optiforge/frontend/AST.h"
#include "optiforge/frontend/Symbol.h"
#include "optiforge/ir/IRBuilder.h"
#include "optiforge/ir/Module.h"

namespace optiforge {

class DiagnosticEngine;

/// Lowers a typed AST into IR.
///
/// This is the only module that knows both the frontend and the IR, which is
/// what keeps the two independent of each other (architectural_design.md
/// section 3). It assumes semantic analysis has succeeded: every expression
/// carries a resolved type and every name a resolved symbol.
class IRGen {
public:
  IRGen(DiagnosticEngine& diags, const std::string& sourceName, std::uint64_t sourceHash);

  /// Lowers the whole program. The returned module has been pruned of
  /// unreachable blocks but not yet verified; the caller runs the verifier.
  std::unique_ptr<ir::Module> run(const Program& program);

private:
  // --- Declarations ---
  void declareBuiltins();
  void declareFunctions(const Program& program);
  void lowerFunction(const FunctionDecl& decl);

  // --- Statements ---
  void lowerStmt(const Stmt& stmt);
  void lowerBlock(const Block& block);
  void lowerVarDecl(const VarDeclStmt& decl);
  void lowerAssign(const AssignStmt& assign);
  void lowerIf(const IfStmt& stmt);
  void lowerWhile(const WhileStmt& stmt);
  void lowerReturn(const ReturnStmt& stmt);

  // --- Expressions ---
  ir::Value* lowerExpr(const Expr& expr);
  ir::Value* lowerBinary(const BinaryExpr& expr);
  ir::Value* lowerShortCircuit(const BinaryExpr& expr);
  ir::Value* lowerUnary(const UnaryExpr& expr);
  ir::Value* lowerCall(const CallExpr& expr);

  /// Inserts an int-to-float conversion when the value's type does not already
  /// match. Semantic analysis has already decided the conversion is legal
  /// (FE-29); this is where it becomes an instruction.
  ir::Value* convert(ir::Value* value, const ir::Type* target);

  /// Maps a frontend type onto its IR counterpart.
  static const ir::Type* lowerType(TypeSpec spec);
  static const ir::Type* lowerType(const Type* type);

  DiagnosticEngine& diags_;
  std::unique_ptr<ir::Module> module_;
  std::unique_ptr<ir::IRBuilder> builder_;
  ir::Function* currentFunction_ = nullptr;

  /// Stack slot holding each local or parameter. Every variable lives in memory
  /// until mem2reg promotes it in Phase 6 (ADR-02).
  std::unordered_map<const Symbol*, ir::Value*> slots_;
};

}  // namespace optiforge
