#include "optiforge/frontend/Sema.h"

#include <string>
#include <utility>
#include <vector>

#include "optiforge/support/Diagnostic.h"

namespace optiforge {

namespace {

std::string quoted(std::string_view text) { return "'" + std::string(text) + "'"; }

std::string typeName(const Type* type) {
  return type != nullptr ? std::string(type->name()) : std::string("<error>");
}

/// True for operators that compare and therefore yield bool.
bool isComparison(BinaryOp op) {
  switch (op) {
    case BinaryOp::Eq:
    case BinaryOp::Ne:
    case BinaryOp::Lt:
    case BinaryOp::Gt:
    case BinaryOp::Le:
    case BinaryOp::Ge:
      return true;
    default:
      return false;
  }
}

/// Equality works on bool as well as on numbers; ordering does not.
bool isEquality(BinaryOp op) { return op == BinaryOp::Eq || op == BinaryOp::Ne; }

bool isLogical(BinaryOp op) { return op == BinaryOp::And || op == BinaryOp::Or; }

}  // namespace

Sema::Sema(DiagnosticEngine& diags, SymbolTable& symbols) : diags_(diags), symbols_(symbols) {}

void Sema::error(SourceRange range, const std::string& message) {
  hadError_ = true;
  diags_.report(range, DiagSeverity::Error, message);
}

void Sema::warning(SourceRange range, const std::string& message) {
  diags_.report(range, DiagSeverity::Warning, message);
}

void Sema::note(SourceRange range, const std::string& message) {
  diags_.report(range, DiagSeverity::Note, message);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

bool Sema::analyze(Program& program, bool requireEntryPoint) {
  declareBuiltins();
  collectSignatures(program);

  if (requireEntryPoint) {
    checkEntryPoint(program);
  }

  for (FunctionPtr& function : program.functions()) {
    analyzeFunction(*function);
  }

  return !hadError_;
}

// ---------------------------------------------------------------------------
// Pass 1: signatures
// ---------------------------------------------------------------------------

void Sema::declareBuiltins() {
  // Provided by libofrt and linked into every compiled program. Declaring them
  // here is what makes `print_int(x);` resolve without an import system.
  const struct {
    const char* name;
    const Type* paramType;
  } builtins[] = {
      {"print_int", Type::getInt()},
      {"print_float", Type::getFloat()},
      {"print_bool", Type::getBool()},
  };

  for (const auto& builtin : builtins) {
    Symbol symbol;
    symbol.kind = Symbol::Kind::Function;
    symbol.name = builtin.name;
    symbol.type = Type::getVoid();
    symbol.paramTypes = {builtin.paramType};
    symbol.paramNames = {"value"};
    symbol.isBuiltin = true;
    symbols_.declare(std::move(symbol));
  }
}

void Sema::collectSignatures(Program& program) {
  for (FunctionPtr& function : program.functions()) {
    if (Symbol* previous = symbols_.lookup(function->name())) {
      if (previous->isBuiltin) {
        error(function->nameRange(),
              "redefinition of built-in function " + quoted(function->name()));
      } else {
        error(function->nameRange(), "redefinition of function " + quoted(function->name()));
        note(previous->declRange, "previous definition is here");
      }
      continue;
    }

    Symbol symbol;
    symbol.kind = Symbol::Kind::Function;
    symbol.name = function->name();
    symbol.type = typeFromSpec(function->returnType());
    symbol.declRange = function->nameRange();

    for (ParamPtr& param : function->params()) {
      const Type* paramType = typeFromSpec(param->declaredType());
      if (paramType->isVoid()) {
        error(param->range(),
              "parameter " + quoted(param->name()) + " cannot have type 'void'");
      }
      symbol.paramTypes.push_back(paramType);
      symbol.paramNames.push_back(param->name());
    }

    function->setSymbol(symbols_.declare(std::move(symbol)));
  }
}

void Sema::checkEntryPoint(const Program& program) {
  Symbol* main = symbols_.lookup("main");
  if (main == nullptr || !main->isFunction() || main->isBuiltin) {
    // Anchor at the start of the file: there is no better location for the
    // absence of something.
    SourceRange at = program.range();
    at.end = at.begin;
    error(at, "program has no 'main' function");
    return;
  }

  if (!main->paramTypes.empty() || main->type != Type::getInt()) {
    error(main->declRange, "'main' must take no parameters and return 'int'");
  }
}

// ---------------------------------------------------------------------------
// Pass 2: bodies
// ---------------------------------------------------------------------------

void Sema::analyzeFunction(FunctionDecl& function) {
  Symbol* symbol = function.symbol();
  if (symbol == nullptr) {
    return;  // duplicate definition; already reported
  }

  currentFunction_ = symbol;
  symbols_.pushScope();

  for (ParamPtr& param : function.params()) {
    const Type* paramType = typeFromSpec(param->declaredType());

    Symbol paramSymbol;
    paramSymbol.kind = Symbol::Kind::Parameter;
    paramSymbol.name = param->name();
    paramSymbol.type = paramType;
    paramSymbol.declRange = param->range();

    Symbol* declared = symbols_.declare(std::move(paramSymbol));
    if (declared == nullptr) {
      error(param->range(), "redeclaration of parameter " + quoted(param->name()));
    }
    param->setSymbol(declared);
  }

  if (Block* body = function.body()) {
    // The body shares the parameter scope: a local named after a parameter is a
    // redeclaration, not shadowing.
    analyzeBlock(*body, /*ownScope=*/false);

    const bool needsReturn = !typeFromSpec(function.returnType())->isVoid();
    if (needsReturn && !returnsOnAllPaths(body)) {
      error(function.nameRange(), "not all control paths in function " +
                                      quoted(function.name()) + " return a value");
    }
  }

  symbols_.popScope();
  currentFunction_ = nullptr;
}

void Sema::analyzeBlock(Block& block, bool ownScope) {
  if (ownScope) {
    symbols_.pushScope();
  }
  for (StmtPtr& statement : block.statements()) {
    analyzeStmt(*statement);
  }
  if (ownScope) {
    symbols_.popScope();
  }
}

void Sema::analyzeStmt(Stmt& stmt) {
  switch (stmt.kind()) {
    case Node::Kind::Block:
      analyzeBlock(static_cast<Block&>(stmt), /*ownScope=*/true);
      break;
    case Node::Kind::VarDeclStmt:
      analyzeVarDecl(static_cast<VarDeclStmt&>(stmt));
      break;
    case Node::Kind::AssignStmt:
      analyzeAssign(static_cast<AssignStmt&>(stmt));
      break;
    case Node::Kind::ExprStmt: {
      auto& exprStmt = static_cast<ExprStmt&>(stmt);
      if (Expr* inner = exprStmt.expr()) {
        analyzeExpr(*inner);
      }
      break;
    }
    case Node::Kind::IfStmt:
      analyzeIf(static_cast<IfStmt&>(stmt));
      break;
    case Node::Kind::WhileStmt:
      analyzeWhile(static_cast<WhileStmt&>(stmt));
      break;
    case Node::Kind::ReturnStmt:
      analyzeReturn(static_cast<ReturnStmt&>(stmt));
      break;
    default:
      break;
  }
}

void Sema::analyzeVarDecl(VarDeclStmt& decl) {
  const Type* declaredType = typeFromSpec(decl.declaredType());

  if (declaredType->isVoid()) {
    error(decl.range(), "variable " + quoted(decl.name()) + " cannot have type 'void'");
  }

  // The initializer is analyzed *before* the name is declared, so
  // `int x = x;` reports "undeclared variable" rather than silently
  // self-referencing.
  const Type* initType = nullptr;
  if (Expr* init = decl.init()) {
    initType = analyzeExpr(*init);
    if (initType != nullptr && !isAssignable(declaredType, initType)) {
      error(init->range(), "cannot initialize a variable of type " +
                               quoted(declaredType->name()) + " with a value of type " +
                               quoted(initType->name()));
    }
  }

  if (Symbol* shadowed = symbols_.lookupOuter(decl.name())) {
    if (shadowed->isVariableLike()) {
      warning(decl.nameRange(),
              "declaration of " + quoted(decl.name()) + " shadows an outer declaration");
      note(shadowed->declRange, "previous declaration is here");
    }
  }

  Symbol symbol;
  symbol.kind = Symbol::Kind::Variable;
  symbol.name = decl.name();
  symbol.type = declaredType;
  symbol.declRange = decl.nameRange();

  Symbol* declared = symbols_.declare(std::move(symbol));
  if (declared == nullptr) {
    Symbol* previous = symbols_.lookupLocal(decl.name());
    error(decl.nameRange(), "redeclaration of " + quoted(decl.name()));
    if (previous != nullptr) {
      note(previous->declRange, "previous declaration is here");
    }
  }
  decl.setSymbol(declared);
}

void Sema::analyzeAssign(AssignStmt& assign) {
  const Type* valueType = nullptr;
  if (Expr* value = assign.value()) {
    valueType = analyzeExpr(*value);
  }

  Symbol* target = symbols_.lookup(assign.name());
  if (target == nullptr) {
    error(assign.nameRange(), "use of undeclared variable " + quoted(assign.name()));
    return;
  }
  if (target->isFunction()) {
    error(assign.nameRange(), "cannot assign to function " + quoted(assign.name()));
    return;
  }

  assign.setSymbol(target);

  if (valueType != nullptr && !isAssignable(target->type, valueType)) {
    error(assign.value()->range(), "cannot assign a value of type " +
                                       quoted(valueType->name()) + " to variable " +
                                       quoted(assign.name()) + " of type " +
                                       quoted(typeName(target->type)));
  }
}

void Sema::analyzeIf(IfStmt& stmt) {
  if (Expr* cond = stmt.cond()) {
    requireBoolCondition(*cond, "if");
  }
  if (Block* thenBlock = stmt.thenBlock()) {
    analyzeBlock(*thenBlock, /*ownScope=*/true);
  }
  if (Stmt* elseBranch = stmt.elseBranch()) {
    analyzeStmt(*elseBranch);
  }
}

void Sema::analyzeWhile(WhileStmt& stmt) {
  if (Expr* cond = stmt.cond()) {
    requireBoolCondition(*cond, "while");
  }
  if (Block* body = stmt.body()) {
    analyzeBlock(*body, /*ownScope=*/true);
  }
}

void Sema::analyzeReturn(ReturnStmt& stmt) {
  const Type* expected = currentFunction_ != nullptr ? currentFunction_->type : nullptr;
  const std::string functionName =
      currentFunction_ != nullptr ? currentFunction_->name : std::string("<unknown>");

  Expr* value = stmt.value();

  if (value == nullptr) {
    if (expected != nullptr && !expected->isVoid()) {
      error(stmt.range(), "non-void function " + quoted(functionName) +
                              " must return a value of type " + quoted(expected->name()));
    }
    return;
  }

  const Type* valueType = analyzeExpr(*value);

  if (expected != nullptr && expected->isVoid()) {
    error(value->range(),
          "void function " + quoted(functionName) + " cannot return a value");
    return;
  }

  if (valueType != nullptr && !isAssignable(expected, valueType)) {
    error(value->range(), "cannot return a value of type " + quoted(valueType->name()) +
                              " from a function returning " + quoted(typeName(expected)));
  }
}

void Sema::requireBoolCondition(Expr& cond, std::string_view construct) {
  const Type* type = analyzeExpr(cond);
  if (type == nullptr || type->isBool()) {
    return;
  }

  error(cond.range(), "condition of " + quoted(construct) + " must have type 'bool', but has type " +
                          quoted(type->name()));
  if (type->isNumeric()) {
    // The single most likely mistake, given there is no int-to-bool conversion.
    note(cond.range(), "there is no implicit conversion to 'bool'; write an explicit "
                       "comparison such as '!= 0'");
  }
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

const Type* Sema::analyzeExpr(Expr& expr) {
  const Type* type = nullptr;

  switch (expr.kind()) {
    case Node::Kind::IntLiteralExpr:
      type = Type::getInt();
      break;
    case Node::Kind::FloatLiteralExpr:
      type = Type::getFloat();
      break;
    case Node::Kind::BoolLiteralExpr:
      type = Type::getBool();
      break;
    case Node::Kind::VarRefExpr:
      type = analyzeVarRef(static_cast<VarRefExpr&>(expr));
      break;
    case Node::Kind::UnaryExpr:
      type = analyzeUnary(static_cast<UnaryExpr&>(expr));
      break;
    case Node::Kind::BinaryExpr:
      type = analyzeBinary(static_cast<BinaryExpr&>(expr));
      break;
    case Node::Kind::CallExpr:
      type = analyzeCall(static_cast<CallExpr&>(expr));
      break;
    default:
      break;
  }

  expr.setType(type);
  return type;
}

const Type* Sema::analyzeVarRef(VarRefExpr& expr) {
  Symbol* symbol = symbols_.lookup(expr.name());
  if (symbol == nullptr) {
    error(expr.range(), "use of undeclared variable " + quoted(expr.name()));
    return nullptr;
  }
  if (symbol->isFunction()) {
    error(expr.range(), "function " + quoted(expr.name()) +
                            " cannot be used as a value; did you mean to call it?");
    return nullptr;
  }
  expr.setSymbol(symbol);
  return symbol->type;
}

const Type* Sema::analyzeUnary(UnaryExpr& expr) {
  Expr* operand = expr.operand();
  if (operand == nullptr) {
    return nullptr;
  }
  const Type* operandType = analyzeExpr(*operand);
  if (operandType == nullptr) {
    return nullptr;
  }

  switch (expr.op()) {
    case UnaryOp::Neg:
      if (!operandType->isNumeric()) {
        error(expr.range(), "operator '-' requires a numeric operand, but the operand has "
                            "type " +
                                quoted(operandType->name()));
        return nullptr;
      }
      return operandType;

    case UnaryOp::Not:
      if (!operandType->isBool()) {
        error(expr.range(), "operator '!' requires a 'bool' operand, but the operand has type " +
                                quoted(operandType->name()));
        return nullptr;
      }
      return Type::getBool();
  }
  return nullptr;
}

const Type* Sema::analyzeBinary(BinaryExpr& expr) {
  Expr* lhsExpr = expr.lhs();
  Expr* rhsExpr = expr.rhs();
  if (lhsExpr == nullptr || rhsExpr == nullptr) {
    return nullptr;
  }

  // Both sides are analyzed even when the first fails, so errors on the right
  // are still reported in this run.
  const Type* lhs = analyzeExpr(*lhsExpr);
  const Type* rhs = analyzeExpr(*rhsExpr);
  if (lhs == nullptr || rhs == nullptr) {
    return nullptr;
  }

  const std::string spelling = std::string(toString(expr.op()));
  const auto operandError = [&]() {
    error(expr.range(), "invalid operands to binary operator '" + spelling + "' (" +
                            quoted(lhs->name()) + " and " + quoted(rhs->name()) + ")");
  };

  if (isLogical(expr.op())) {
    if (!lhs->isBool() || !rhs->isBool()) {
      operandError();
      return nullptr;
    }
    return Type::getBool();
  }

  if (isComparison(expr.op())) {
    if (isEquality(expr.op()) && lhs->isBool() && rhs->isBool()) {
      return Type::getBool();
    }
    if (promoteArithmetic(lhs, rhs) == nullptr) {
      operandError();
      return nullptr;
    }
    return Type::getBool();
  }

  if (expr.op() == BinaryOp::Mod) {
    // LANG-26: '%' is defined only for integers.
    if (!lhs->isInt() || !rhs->isInt()) {
      error(expr.range(), "operator '%' requires integer operands, but the operands have "
                          "types " +
                              quoted(lhs->name()) + " and " + quoted(rhs->name()));
      return nullptr;
    }
    return Type::getInt();
  }

  const Type* result = promoteArithmetic(lhs, rhs);
  if (result == nullptr) {
    operandError();
    return nullptr;
  }
  return result;
}

const Type* Sema::analyzeCall(CallExpr& expr) {
  // Arguments are analyzed first and unconditionally, so a type error inside an
  // argument is reported even when the callee itself is unknown.
  std::vector<const Type*> argTypes;
  argTypes.reserve(expr.args().size());
  for (ExprPtr& arg : expr.args()) {
    argTypes.push_back(arg != nullptr ? analyzeExpr(*arg) : nullptr);
  }

  Symbol* callee = symbols_.lookup(expr.callee());
  if (callee == nullptr) {
    error(expr.range(), "use of undeclared function " + quoted(expr.callee()));
    return nullptr;
  }
  if (!callee->isFunction()) {
    error(expr.range(), quoted(expr.callee()) + " is not a function");
    note(callee->declRange, "declared here");
    return nullptr;
  }

  expr.setSymbol(callee);

  if (argTypes.size() != callee->paramTypes.size()) {
    error(expr.range(), "function " + quoted(expr.callee()) + " expects " +
                            std::to_string(callee->paramTypes.size()) + " argument" +
                            (callee->paramTypes.size() == 1 ? "" : "s") + ", but " +
                            std::to_string(argTypes.size()) + " " +
                            (argTypes.size() == 1 ? "was" : "were") + " provided");
    return callee->type;
  }

  for (std::size_t i = 0; i < argTypes.size(); ++i) {
    if (argTypes[i] == nullptr) {
      continue;  // already reported
    }
    if (!isAssignable(callee->paramTypes[i], argTypes[i])) {
      error(expr.args()[i]->range(),
            "cannot pass a value of type " + quoted(argTypes[i]->name()) + " as parameter " +
                quoted(callee->paramNames[i]) + " of type " +
                quoted(typeName(callee->paramTypes[i])));
    }
  }

  return callee->type;
}

// ---------------------------------------------------------------------------
// Control-flow analysis
// ---------------------------------------------------------------------------

bool Sema::returnsOnAllPaths(const Stmt* stmt) {
  if (stmt == nullptr) {
    return false;
  }

  switch (stmt->kind()) {
    case Node::Kind::ReturnStmt:
      return true;

    case Node::Kind::Block: {
      const auto* block = static_cast<const Block*>(stmt);
      for (const StmtPtr& child : block->statements()) {
        if (returnsOnAllPaths(child.get())) {
          return true;
        }
      }
      return false;
    }

    case Node::Kind::IfStmt: {
      const auto* ifStmt = static_cast<const IfStmt*>(stmt);
      // Without an else there is always a path that falls through.
      return ifStmt->elseBranch() != nullptr && returnsOnAllPaths(ifStmt->thenBlock()) &&
             returnsOnAllPaths(ifStmt->elseBranch());
    }

    case Node::Kind::WhileStmt:
      // The condition may be false on entry, so a loop never guarantees a
      // return -- not even `while (true)`, which this language cannot prove.
      return false;

    default:
      return false;
  }
}

}  // namespace optiforge
