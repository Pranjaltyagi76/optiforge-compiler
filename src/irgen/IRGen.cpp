#include "optiforge/irgen/IRGen.h"

#include <utility>
#include <vector>

#include "optiforge/frontend/Type.h"
#include "optiforge/support/Diagnostic.h"

namespace optiforge {

using ir::Opcode;
using ir::Predicate;

IRGen::IRGen(DiagnosticEngine& diags, const std::string& sourceName,
             std::uint64_t sourceHash)
    : diags_(diags), module_(std::make_unique<ir::Module>(sourceName)) {
  module_->setSourceHash(sourceHash);
  builder_ = std::make_unique<ir::IRBuilder>(*module_);
}

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

const ir::Type* IRGen::lowerType(TypeSpec spec) {
  switch (spec) {
    case TypeSpec::Int:
      return ir::Type::getI64();
    case TypeSpec::Float:
      return ir::Type::getF64();
    case TypeSpec::Bool:
      return ir::Type::getI1();
    case TypeSpec::Void:
      return ir::Type::getVoid();
  }
  return ir::Type::getVoid();
}

const ir::Type* IRGen::lowerType(const Type* type) {
  if (type == nullptr) {
    return ir::Type::getVoid();
  }
  switch (type->kind()) {
    case Type::Kind::Int:
      return ir::Type::getI64();
    case Type::Kind::Float:
      return ir::Type::getF64();
    case Type::Kind::Bool:
      return ir::Type::getI1();
    case Type::Kind::Void:
      return ir::Type::getVoid();
  }
  return ir::Type::getVoid();
}

ir::Value* IRGen::convert(ir::Value* value, const ir::Type* target) {
  if (value == nullptr || value->type() == target) {
    return value;
  }
  // int -> float is the only conversion the language allows, and sema has
  // already verified this one is legal.
  if (value->type()->isI64() && target->isF64()) {
    return builder_->createSIToFP(value);
  }
  return value;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

std::unique_ptr<ir::Module> IRGen::run(const Program& program) {
  declareBuiltins();
  declareFunctions(program);

  for (const FunctionPtr& function : program.functions()) {
    lowerFunction(*function);
  }

  for (const auto& function : module_->functions()) {
    if (!function->isDeclaration()) {
      function->pruneUnreachableBlocks();
    }
  }

  return std::move(module_);
}

void IRGen::declareBuiltins() {
  const struct {
    const char* name;
    const ir::Type* paramType;
  } builtins[] = {
      {"print_int", ir::Type::getI64()},
      {"print_float", ir::Type::getF64()},
      {"print_bool", ir::Type::getI1()},
  };

  for (const auto& builtin : builtins) {
    // No body: these are resolved at link time against libofrt.
    ir::Function* function = module_->createFunction(builtin.name, ir::Type::getVoid());
    function->addArgument(builtin.paramType, "value");
  }
}

void IRGen::declareFunctions(const Program& program) {
  // Declared up front so a call can be lowered before the callee's body is,
  // which is what makes forward references and mutual recursion work here as
  // they do in semantic analysis.
  for (const FunctionPtr& decl : program.functions()) {
    ir::Function* function =
        module_->createFunction(decl->name(), lowerType(decl->returnType()));
    for (const ParamPtr& param : decl->params()) {
      function->addArgument(lowerType(param->declaredType()), param->name());
    }
  }
}

// ---------------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------------

void IRGen::lowerFunction(const FunctionDecl& decl) {
  ir::Function* function = module_->findFunction(decl.name());
  if (function == nullptr || decl.body() == nullptr) {
    return;
  }

  currentFunction_ = function;
  slots_.clear();

  ir::BasicBlock* entry = function->createBlock("entry");
  builder_->setInsertPoint(entry);

  // Parameters get stack slots like any other variable, so an assignment to a
  // parameter needs no special case. mem2reg removes the traffic in Phase 6.
  for (std::size_t i = 0; i < decl.params().size(); ++i) {
    const ParamDecl& param = *decl.params()[i];
    const ir::Type* type = lowerType(param.declaredType());
    ir::Value* slot = builder_->createEntryAlloca(type, param.name());
    builder_->createStore(function->arguments()[i].get(), slot);
    if (param.symbol() != nullptr) {
      slots_[param.symbol()] = slot;
    }
  }

  lowerBlock(*decl.body());

  // Close any block left open. For a non-void function semantic analysis has
  // already proved every path returns, so this terminator sits in a block that
  // pruning will usually remove; it exists so the IR is well-formed either way.
  if (!builder_->atTerminatedBlock() && builder_->insertPoint() != nullptr) {
    if (function->returnType()->isVoid()) {
      builder_->createRetVoid();
    } else if (function->returnType()->isF64()) {
      builder_->createRet(module_->getFloat(0.0));
    } else if (function->returnType()->isI1()) {
      builder_->createRet(module_->getBool(false));
    } else {
      builder_->createRet(module_->getInt(0));
    }
  }

  currentFunction_ = nullptr;
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

void IRGen::lowerBlock(const Block& block) {
  for (const StmtPtr& statement : block.statements()) {
    // Everything after a terminator in the same block is unreachable; emitting
    // it would produce an instruction after the terminator, which the verifier
    // rejects.
    if (builder_->atTerminatedBlock()) {
      break;
    }
    lowerStmt(*statement);
  }
}

void IRGen::lowerStmt(const Stmt& stmt) {
  switch (stmt.kind()) {
    case Node::Kind::Block:
      lowerBlock(static_cast<const Block&>(stmt));
      break;
    case Node::Kind::VarDeclStmt:
      lowerVarDecl(static_cast<const VarDeclStmt&>(stmt));
      break;
    case Node::Kind::AssignStmt:
      lowerAssign(static_cast<const AssignStmt&>(stmt));
      break;
    case Node::Kind::ExprStmt: {
      const auto& exprStmt = static_cast<const ExprStmt&>(stmt);
      if (exprStmt.expr() != nullptr) {
        lowerExpr(*exprStmt.expr());
      }
      break;
    }
    case Node::Kind::IfStmt:
      lowerIf(static_cast<const IfStmt&>(stmt));
      break;
    case Node::Kind::WhileStmt:
      lowerWhile(static_cast<const WhileStmt&>(stmt));
      break;
    case Node::Kind::ReturnStmt:
      lowerReturn(static_cast<const ReturnStmt&>(stmt));
      break;
    default:
      break;
  }
}

void IRGen::lowerVarDecl(const VarDeclStmt& decl) {
  const ir::Type* type = lowerType(decl.declaredType());

  // The slot goes in the entry block wherever the declaration appears; the
  // builder owns that rule.
  ir::Value* slot = builder_->createEntryAlloca(type, decl.name());

  if (decl.symbol() != nullptr) {
    slots_[decl.symbol()] = slot;
  }

  if (decl.init() != nullptr) {
    ir::Value* value = convert(lowerExpr(*decl.init()), type);
    if (value != nullptr) {
      builder_->createStore(value, slot);
    }
  }
}

void IRGen::lowerAssign(const AssignStmt& assign) {
  ir::Value* value = lowerExpr(*assign.value());
  const auto it = slots_.find(assign.symbol());
  if (it == slots_.end() || value == nullptr) {
    return;
  }
  const ir::Type* target = lowerType(assign.symbol()->type);
  builder_->createStore(convert(value, target), it->second);
}

void IRGen::lowerIf(const IfStmt& stmt) {
  ir::Value* condition = lowerExpr(*stmt.cond());
  if (condition == nullptr) {
    return;
  }

  const bool hasElse = stmt.elseBranch() != nullptr;
  ir::BasicBlock* thenBlock = currentFunction_->createBlock("if.then");
  ir::BasicBlock* elseBlock = hasElse ? currentFunction_->createBlock("if.else") : nullptr;
  ir::BasicBlock* endBlock = currentFunction_->createBlock("if.end");

  builder_->createCondBr(condition, thenBlock, hasElse ? elseBlock : endBlock);

  builder_->setInsertPoint(thenBlock);
  lowerBlock(*stmt.thenBlock());
  if (!builder_->atTerminatedBlock()) {
    builder_->createBr(endBlock);
  }

  if (hasElse) {
    builder_->setInsertPoint(elseBlock);
    lowerStmt(*stmt.elseBranch());
    if (!builder_->atTerminatedBlock()) {
      builder_->createBr(endBlock);
    }
  }

  builder_->setInsertPoint(endBlock);
}

void IRGen::lowerWhile(const WhileStmt& stmt) {
  ir::BasicBlock* condBlock = currentFunction_->createBlock("while.cond");
  ir::BasicBlock* bodyBlock = currentFunction_->createBlock("while.body");
  ir::BasicBlock* endBlock = currentFunction_->createBlock("while.end");

  builder_->createBr(condBlock);

  builder_->setInsertPoint(condBlock);
  ir::Value* condition = lowerExpr(*stmt.cond());
  if (condition == nullptr) {
    return;
  }
  builder_->createCondBr(condition, bodyBlock, endBlock);

  builder_->setInsertPoint(bodyBlock);
  lowerBlock(*stmt.body());
  if (!builder_->atTerminatedBlock()) {
    // The back edge. Loop detection in Phase 5 finds loops precisely by looking
    // for this edge, so its shape matters well beyond Phase 3.
    builder_->createBr(condBlock);
  }

  builder_->setInsertPoint(endBlock);
}

void IRGen::lowerReturn(const ReturnStmt& stmt) {
  if (stmt.value() == nullptr) {
    builder_->createRetVoid();
    return;
  }
  ir::Value* value = convert(lowerExpr(*stmt.value()), currentFunction_->returnType());
  if (value != nullptr) {
    builder_->createRet(value);
  }
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

ir::Value* IRGen::lowerExpr(const Expr& expr) {
  switch (expr.kind()) {
    case Node::Kind::IntLiteralExpr:
      return module_->getInt(static_cast<const IntLiteralExpr&>(expr).value());

    case Node::Kind::FloatLiteralExpr:
      return module_->getFloat(static_cast<const FloatLiteralExpr&>(expr).value());

    case Node::Kind::BoolLiteralExpr:
      return module_->getBool(static_cast<const BoolLiteralExpr&>(expr).value());

    case Node::Kind::VarRefExpr: {
      const auto& ref = static_cast<const VarRefExpr&>(expr);
      const auto it = slots_.find(ref.symbol());
      if (it == slots_.end()) {
        return nullptr;
      }
      return builder_->createLoad(it->second, lowerType(ref.type()));
    }

    case Node::Kind::UnaryExpr:
      return lowerUnary(static_cast<const UnaryExpr&>(expr));

    case Node::Kind::BinaryExpr:
      return lowerBinary(static_cast<const BinaryExpr&>(expr));

    case Node::Kind::CallExpr:
      return lowerCall(static_cast<const CallExpr&>(expr));

    default:
      return nullptr;
  }
}

ir::Value* IRGen::lowerUnary(const UnaryExpr& expr) {
  ir::Value* operand = lowerExpr(*expr.operand());
  if (operand == nullptr) {
    return nullptr;
  }
  switch (expr.op()) {
    case UnaryOp::Neg:
      return builder_->createNeg(operand);
    case UnaryOp::Not:
      return builder_->createNot(operand);
  }
  return nullptr;
}

ir::Value* IRGen::lowerShortCircuit(const BinaryExpr& expr) {
  // `a && b` must not evaluate `b` when `a` is false, so it becomes control
  // flow rather than a single instruction. Without SSA (Phase 6) the result is
  // carried in a stack slot; mem2reg turns that into the phi node this shape
  // really wants.
  const bool isAnd = expr.op() == BinaryOp::And;

  ir::Value* slot = builder_->createEntryAlloca(ir::Type::getI1(), isAnd ? "and" : "or");

  ir::Value* lhs = lowerExpr(*expr.lhs());
  if (lhs == nullptr) {
    return nullptr;
  }
  builder_->createStore(lhs, slot);

  ir::BasicBlock* rhsBlock =
      currentFunction_->createBlock(isAnd ? "and.rhs" : "or.rhs");
  ir::BasicBlock* endBlock =
      currentFunction_->createBlock(isAnd ? "and.end" : "or.end");

  // For &&, evaluate the right side only when the left was true; for ||, only
  // when it was false.
  if (isAnd) {
    builder_->createCondBr(lhs, rhsBlock, endBlock);
  } else {
    builder_->createCondBr(lhs, endBlock, rhsBlock);
  }

  builder_->setInsertPoint(rhsBlock);
  ir::Value* rhs = lowerExpr(*expr.rhs());
  if (rhs != nullptr) {
    builder_->createStore(rhs, slot);
  }
  if (!builder_->atTerminatedBlock()) {
    builder_->createBr(endBlock);
  }

  builder_->setInsertPoint(endBlock);
  return builder_->createLoad(slot, ir::Type::getI1());
}

ir::Value* IRGen::lowerBinary(const BinaryExpr& expr) {
  if (expr.op() == BinaryOp::And || expr.op() == BinaryOp::Or) {
    return lowerShortCircuit(expr);
  }

  ir::Value* lhs = lowerExpr(*expr.lhs());
  ir::Value* rhs = lowerExpr(*expr.rhs());
  if (lhs == nullptr || rhs == nullptr) {
    return nullptr;
  }

  // Promote whichever side is narrower. Sema decided the result type; this is
  // where the widening becomes a real instruction.
  const bool isFloatOp = lhs->type()->isF64() || rhs->type()->isF64();
  if (isFloatOp) {
    lhs = convert(lhs, ir::Type::getF64());
    rhs = convert(rhs, ir::Type::getF64());
  }

  switch (expr.op()) {
    case BinaryOp::Add:
      return builder_->createBinary(isFloatOp ? Opcode::FAdd : Opcode::Add, lhs, rhs);
    case BinaryOp::Sub:
      return builder_->createBinary(isFloatOp ? Opcode::FSub : Opcode::Sub, lhs, rhs);
    case BinaryOp::Mul:
      return builder_->createBinary(isFloatOp ? Opcode::FMul : Opcode::Mul, lhs, rhs);
    case BinaryOp::Div:
      return builder_->createBinary(isFloatOp ? Opcode::FDiv : Opcode::SDiv, lhs, rhs);
    case BinaryOp::Mod:
      return builder_->createBinary(Opcode::SRem, lhs, rhs);

    case BinaryOp::Eq:
    case BinaryOp::Ne:
    case BinaryOp::Lt:
    case BinaryOp::Gt:
    case BinaryOp::Le:
    case BinaryOp::Ge: {
      Predicate predicate = Predicate::Eq;
      switch (expr.op()) {
        case BinaryOp::Eq: predicate = Predicate::Eq; break;
        case BinaryOp::Ne: predicate = Predicate::Ne; break;
        case BinaryOp::Lt: predicate = Predicate::Lt; break;
        case BinaryOp::Gt: predicate = Predicate::Gt; break;
        case BinaryOp::Le: predicate = Predicate::Le; break;
        default:           predicate = Predicate::Ge; break;
      }
      return builder_->createCmp(isFloatOp ? Opcode::FCmp : Opcode::ICmp, predicate, lhs, rhs);
    }

    default:
      return nullptr;
  }
}

ir::Value* IRGen::lowerCall(const CallExpr& expr) {
  ir::Function* callee = module_->findFunction(expr.callee());
  if (callee == nullptr) {
    return nullptr;
  }

  std::vector<ir::Value*> args;
  args.reserve(expr.args().size());
  for (std::size_t i = 0; i < expr.args().size(); ++i) {
    ir::Value* arg = lowerExpr(*expr.args()[i]);
    if (arg == nullptr) {
      return nullptr;
    }
    if (i < callee->arguments().size()) {
      arg = convert(arg, callee->arguments()[i]->type());
    }
    args.push_back(arg);
  }

  return builder_->createCall(callee, args);
}

}  // namespace optiforge
