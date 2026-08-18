#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "TestHarness.h"
#include "optiforge/frontend/ASTPrinter.h"
#include "optiforge/frontend/Lexer.h"
#include "optiforge/frontend/Parser.h"
#include "optiforge/frontend/Sema.h"
#include "optiforge/frontend/Symbol.h"
#include "optiforge/frontend/Type.h"
#include "optiforge/support/Diagnostic.h"
#include "optiforge/support/SourceManager.h"

using namespace optiforge;

namespace {

struct SemaResult {
  SourceManager sm;
  std::ostringstream diagOut;
  SymbolTable symbols;
  std::unique_ptr<Program> program;
  bool ok = false;
  bool parseError = false;
  unsigned errors = 0;
  unsigned warnings = 0;

  std::string diagnostics() const { return diagOut.str(); }

  bool says(std::string_view text) const {
    return diagOut.str().find(text) != std::string::npos;
  }

  std::string tree() const {
    std::ostringstream out;
    if (program != nullptr) {
      printAST(*program, out);
    }
    return out.str();
  }
};

/// Analyzes a fragment. `requireEntryPoint` is off by default so tests can use
/// a single function without carrying a `main` around.
std::unique_ptr<SemaResult> analyze(std::string source, bool requireEntryPoint = false) {
  auto result = std::make_unique<SemaResult>();
  const FileID file = result->sm.addBuffer("t.of", std::move(source));
  DiagnosticEngine diags(result->sm, result->diagOut);

  Lexer lexer(result->sm, file, diags);
  const std::vector<Token> tokens = lexer.tokenize();

  Parser parser(tokens, diags);
  result->program = parser.parseProgram();
  result->parseError = parser.hadError();

  Sema sema(diags, result->symbols);
  result->ok = sema.analyze(*result->program, requireEntryPoint);
  result->errors = diags.errorCount();
  result->warnings = diags.warningCount();
  return result;
}

/// Wraps an expression so only its typing is under test.
std::unique_ptr<SemaResult> typeOf(const std::string& declType, const std::string& expr) {
  return analyze("fn t() { " + declType + " v = " + expr + "; }");
}

}  // namespace

// ---------------------------------------------------------------------------
// Type system (FE-29)
// ---------------------------------------------------------------------------

TEST("types are interned, so identity is pointer equality") {
  CHECK_EQ(Type::getInt(), Type::getInt());
  CHECK(Type::getInt() != Type::getFloat());
  CHECK_EQ(typeFromSpec(TypeSpec::Bool), Type::getBool());
  CHECK_EQ(typeFromSpec(TypeSpec::Void), Type::getVoid());
}

TEST("int is 64-bit and bool is one byte") {
  CHECK_EQ(Type::getInt()->sizeInBytes(), 8u);
  CHECK_EQ(Type::getFloat()->sizeInBytes(), 8u);
  CHECK_EQ(Type::getBool()->sizeInBytes(), 1u);
  CHECK_EQ(Type::getVoid()->sizeInBytes(), 0u);
}

TEST("int to float is the only implicit conversion") {
  CHECK(isAssignable(Type::getFloat(), Type::getInt()));
  CHECK(isAssignable(Type::getInt(), Type::getInt()));
  CHECK(isAssignable(Type::getBool(), Type::getBool()));

  CHECK(!isAssignable(Type::getInt(), Type::getFloat()));
  CHECK(!isAssignable(Type::getBool(), Type::getInt()));
  CHECK(!isAssignable(Type::getInt(), Type::getBool()));
  CHECK(!isAssignable(Type::getFloat(), Type::getBool()));
}

TEST("arithmetic promotion prefers float and rejects non-numerics") {
  CHECK_EQ(promoteArithmetic(Type::getInt(), Type::getInt()), Type::getInt());
  CHECK_EQ(promoteArithmetic(Type::getInt(), Type::getFloat()), Type::getFloat());
  CHECK_EQ(promoteArithmetic(Type::getFloat(), Type::getInt()), Type::getFloat());
  CHECK_EQ(promoteArithmetic(Type::getBool(), Type::getInt()), nullptr);
  CHECK_EQ(promoteArithmetic(Type::getVoid(), Type::getInt()), nullptr);
}

// ---------------------------------------------------------------------------
// Scopes and symbol resolution (FE-20, FE-21, FE-22, FE-27)
// ---------------------------------------------------------------------------

TEST("a well-typed program analyzes clean") {
  auto r = analyze("fn add(int a, int b) -> int { return a + b; }\n"
                   "fn main() -> int { return add(1, 2); }\n",
                   /*requireEntryPoint=*/true);
  CHECK(r->ok);
  CHECK_EQ(r->errors, 0u);
  CHECK_EQ(r->warnings, 0u);
}

TEST("an undeclared variable is reported") {
  auto r = analyze("fn f() { y = 5; }");
  CHECK(!r->ok);
  CHECK(r->says("use of undeclared variable 'y'"));
}

TEST("an undeclared variable in an expression is reported") {
  auto r = analyze("fn f() -> int { return q + 1; }");
  CHECK(!r->ok);
  CHECK(r->says("use of undeclared variable 'q'"));
}

TEST("a variable is not visible before its own initializer completes") {
  // `int x = x;` must not resolve to the variable being declared.
  auto r = analyze("fn f() { int x = x; }");
  CHECK(!r->ok);
  CHECK(r->says("use of undeclared variable 'x'"));
}

TEST("redeclaration in the same scope is an error with a note") {
  auto r = analyze("fn f() { int x = 1; int x = 2; }");
  CHECK(!r->ok);
  CHECK(r->says("redeclaration of 'x'"));
  CHECK(r->says("previous declaration is here"));
}

TEST("a local may not redeclare a parameter") {
  // The body shares the parameter scope, so this is redeclaration rather than
  // shadowing.
  auto r = analyze("fn f(int a) { int a = 1; }");
  CHECK(!r->ok);
  CHECK(r->says("redeclaration of 'a'"));
}

TEST("shadowing an outer block is a warning, not an error") {
  auto r = analyze("fn f() { int b = 1; { int b = 2; } }");
  CHECK(r->ok);
  CHECK_EQ(r->errors, 0u);
  CHECK_EQ(r->warnings, 1u);
  CHECK(r->says("shadows an outer declaration"));
}

TEST("a variable declared in a block is not visible outside it") {
  auto r = analyze("fn f() { { int inner = 1; } inner = 2; }");
  CHECK(!r->ok);
  CHECK(r->says("use of undeclared variable 'inner'"));
}

TEST("mutual recursion resolves thanks to the signature pre-pass") {
  auto r = analyze("fn even(int n) -> bool { if (n == 0) { return true; } return odd(n - 1); }\n"
                   "fn odd(int n) -> bool { if (n == 0) { return false; } return even(n - 1); }\n");
  CHECK(r->ok);
  CHECK_EQ(r->errors, 0u);
}

TEST("a function may call one declared later in the file") {
  auto r = analyze("fn a() -> int { return b(); }\nfn b() -> int { return 1; }\n");
  CHECK(r->ok);
}

TEST("direct recursion resolves") {
  auto r = analyze("fn f(int n) -> int { if (n < 1) { return 0; } return f(n - 1); }");
  CHECK(r->ok);
}

TEST("redefinition of a function is reported with a note") {
  auto r = analyze("fn f() -> int { return 1; }\nfn f() -> int { return 2; }\n");
  CHECK(!r->ok);
  CHECK(r->says("redefinition of function 'f'"));
  CHECK(r->says("previous definition is here"));
}

TEST("redefining a builtin is reported distinctly") {
  auto r = analyze("fn print_int(int x) { }");
  CHECK(!r->ok);
  CHECK(r->says("redefinition of built-in function 'print_int'"));
}

// ---------------------------------------------------------------------------
// Declarations and assignment (FE-23)
// ---------------------------------------------------------------------------

TEST("an initializer must be assignable to the declared type") {
  CHECK(!analyze("fn f() { int x = 3.14; }")->ok);
  CHECK(!analyze("fn f() { bool b = 1; }")->ok);
  CHECK(!analyze("fn f() { int x = true; }")->ok);
  CHECK(analyze("fn f() { float x = 2; }")->ok);  // the permitted widening
  CHECK(analyze("fn f() { int x = 2; }")->ok);
}

TEST("the initializer diagnostic names both types") {
  auto r = analyze("fn f() { int x = 3.14; }");
  CHECK(r->says("cannot initialize a variable of type 'int' with a value of type 'float'"));
}

TEST("a declaration may omit its initializer") {
  auto r = analyze("fn f() { int x; }");
  CHECK(r->ok);
}

TEST("a variable cannot have type void") {
  auto r = analyze("fn f() { void v; }");
  CHECK(!r->ok);
  CHECK(r->says("variable 'v' cannot have type 'void'"));
}

TEST("a parameter cannot have type void") {
  auto r = analyze("fn f(void v) { }");
  CHECK(!r->ok);
  CHECK(r->says("parameter 'v' cannot have type 'void'"));
}

TEST("assignment checks the target type") {
  auto r = analyze("fn f() { int y = 0; y = 2.5; }");
  CHECK(!r->ok);
  CHECK(r->says("cannot assign a value of type 'float' to variable 'y' of type 'int'"));
}

TEST("assignment permits the int to float widening") {
  CHECK(analyze("fn f() { float y = 0.0; y = 2; }")->ok);
}

TEST("assigning to a function is rejected") {
  auto r = analyze("fn g() { } fn f() { g = 1; }");
  CHECK(!r->ok);
  CHECK(r->says("cannot assign to function 'g'"));
}

// ---------------------------------------------------------------------------
// Operators (LANG-26, FE-23)
// ---------------------------------------------------------------------------

TEST("arithmetic requires numeric operands") {
  auto r = analyze("fn f() { bool b = true; int x = 1 + b; }");
  CHECK(!r->ok);
  CHECK(r->says("invalid operands to binary operator '+' ('int' and 'bool')"));
}

TEST("mixed int and float arithmetic yields float") {
  auto r = typeOf("float", "1 + 2.0");
  CHECK(r->ok);
  CHECK(r->tree().find("BinaryExpr '+' : float") != std::string::npos);
}

TEST("int arithmetic stays int") {
  auto r = typeOf("int", "1 + 2");
  CHECK(r->ok);
  CHECK(r->tree().find("BinaryExpr '+' : int") != std::string::npos);
}

TEST("modulo requires integer operands") {
  auto r = analyze("fn f() { int m = 1.5 % 2; }");
  CHECK(!r->ok);
  CHECK(r->says("operator '%' requires integer operands"));
  CHECK(analyze("fn f() { int m = 5 % 2; }")->ok);
}

TEST("comparisons yield bool") {
  auto r = typeOf("bool", "1 < 2");
  CHECK(r->ok);
  CHECK(r->tree().find("BinaryExpr '<' : bool") != std::string::npos);
}

TEST("ordering comparisons reject bool operands") {
  auto r = analyze("fn f() { bool b = true < false; }");
  CHECK(!r->ok);
  CHECK(r->says("invalid operands to binary operator '<'"));
}

TEST("equality accepts two bools but ordering does not") {
  CHECK(analyze("fn f() { bool b = true == false; }")->ok);
  CHECK(analyze("fn f() { bool b = true != false; }")->ok);
  CHECK(!analyze("fn f() { bool b = true >= false; }")->ok);
}

TEST("logical operators require bool operands and yield bool") {
  CHECK(analyze("fn f() { bool b = true && false; }")->ok);
  auto r = analyze("fn f() { bool b = 1 && true; }");
  CHECK(!r->ok);
  CHECK(r->says("invalid operands to binary operator '&&'"));
}

TEST("unary minus requires a numeric operand") {
  CHECK(analyze("fn f() { int x = -1; }")->ok);
  CHECK(analyze("fn f() { float x = -1.5; }")->ok);
  auto r = analyze("fn f() { bool b = true; int x = -b; }");
  CHECK(!r->ok);
  CHECK(r->says("operator '-' requires a numeric operand"));
}

TEST("logical not requires a bool operand") {
  CHECK(analyze("fn f() { bool b = !true; }")->ok);
  auto r = analyze("fn f() { bool b = !1; }");
  CHECK(!r->ok);
  CHECK(r->says("operator '!' requires a 'bool' operand"));
}

// ---------------------------------------------------------------------------
// Conditions (LANG-32)
// ---------------------------------------------------------------------------

TEST("if and while conditions must be exactly bool") {
  auto ifResult = analyze("fn f() { int x = 1; if (x) { } }");
  CHECK(!ifResult->ok);
  CHECK(ifResult->says("condition of 'if' must have type 'bool', but has type 'int'"));

  auto whileResult = analyze("fn f() { int x = 1; while (x) { } }");
  CHECK(!whileResult->ok);
  CHECK(whileResult->says("condition of 'while' must have type 'bool'"));
}

TEST("a numeric condition suggests an explicit comparison") {
  auto r = analyze("fn f() { int x = 1; if (x) { } }");
  CHECK(r->says("no implicit conversion to 'bool'"));
  CHECK(r->says("!= 0"));
}

TEST("a comparison is an acceptable condition") {
  CHECK(analyze("fn f() { int x = 1; if (x != 0) { } }")->ok);
  CHECK(analyze("fn f() { while (true) { } }")->ok);
}

// ---------------------------------------------------------------------------
// Calls (FE-24, FE-25)
// ---------------------------------------------------------------------------

TEST("calling an undeclared function is reported") {
  auto r = analyze("fn f() { nope(1); }");
  CHECK(!r->ok);
  CHECK(r->says("use of undeclared function 'nope'"));
}

TEST("argument count is checked, with correct pluralization") {
  auto twoExpected = analyze("fn g(int a, int b) { } fn f() { g(1); }");
  CHECK(twoExpected->says("expects 2 arguments, but 1 was provided"));

  auto oneExpected = analyze("fn g(int a) { } fn f() { g(1, 2); }");
  CHECK(oneExpected->says("expects 1 argument, but 2 were provided"));
}

TEST("argument types are checked against parameters by name") {
  auto r = analyze("fn g(int a, int b) { } fn f() { g(1, true); }");
  CHECK(!r->ok);
  CHECK(r->says("cannot pass a value of type 'bool' as parameter 'b' of type 'int'"));
}

TEST("arguments accept the int to float widening") {
  CHECK(analyze("fn g(float x) { } fn f() { g(1); }")->ok);
}

TEST("a call expression takes the callee's return type") {
  auto r = analyze("fn g() -> float { return 1.0; } fn f() { float v = g(); }");
  CHECK(r->ok);
  CHECK(r->tree().find("CallExpr 'g' : float") != std::string::npos);
}

TEST("a void call cannot be used as a value") {
  auto r = analyze("fn g() { } fn f() { int v = g(); }");
  CHECK(!r->ok);
  CHECK(r->says("with a value of type 'void'"));
}

TEST("a function name used without a call is rejected") {
  auto r = analyze("fn g() -> int { return 1; } fn f() { int v = g; }");
  CHECK(!r->ok);
  CHECK(r->says("cannot be used as a value"));
}

TEST("a variable used as a function is rejected") {
  auto r = analyze("fn f() { int g = 1; g(); }");
  CHECK(!r->ok);
  CHECK(r->says("'g' is not a function"));
}

TEST("arguments are analyzed even when the callee is unknown") {
  // Otherwise a typo in the function name would hide real errors inside its
  // arguments until the name was fixed.
  auto r = analyze("fn f() { nope(1 + true); }");
  CHECK(r->says("invalid operands to binary operator '+'"));
  CHECK(r->says("use of undeclared function 'nope'"));
}

TEST("builtin print functions are available without declaration") {
  CHECK(analyze("fn f() { print_int(1); }")->ok);
  CHECK(analyze("fn f() { print_float(1.5); }")->ok);
  CHECK(analyze("fn f() { print_bool(true); }")->ok);
  CHECK(analyze("fn f() { print_int(1.5); }")->ok == false);
}

// ---------------------------------------------------------------------------
// Returns (FE-26, LANG-47)
// ---------------------------------------------------------------------------

TEST("a returned value must match the declared return type") {
  auto r = analyze("fn f() -> int { return 1.5; }");
  CHECK(!r->ok);
  CHECK(r->says("cannot return a value of type 'float' from a function returning 'int'"));
}

TEST("return accepts the int to float widening") {
  CHECK(analyze("fn f() -> float { return 1; }")->ok);
}

TEST("a non-void function must return a value") {
  auto r = analyze("fn f() -> int { return; }");
  CHECK(!r->ok);
  CHECK(r->says("must return a value of type 'int'"));
}

TEST("a void function may not return a value") {
  auto r = analyze("fn f() { return 5; }");
  CHECK(!r->ok);
  CHECK(r->says("void function 'f' cannot return a value"));
}

TEST("a void function may return with no value, or not at all") {
  CHECK(analyze("fn f() { return; }")->ok);
  CHECK(analyze("fn f() { }")->ok);
}

TEST("every path of a non-void function must return") {
  CHECK(!analyze("fn f() -> int { }")->ok);
  CHECK(!analyze("fn f(int n) -> int { if (n > 0) { return 1; } }")->ok);
  CHECK(analyze("fn f(int n) -> int { if (n > 0) { return 1; } return 0; }")->ok);
  CHECK(analyze("fn f(int n) -> int { if (n > 0) { return 1; } else { return 0; } }")->ok);
}

TEST("a loop never satisfies the all-paths-return requirement") {
  // The condition may be false on entry, and this language cannot prove
  // otherwise -- not even for `while (true)`.
  auto r = analyze("fn f() -> int { while (true) { return 1; } }");
  CHECK(!r->ok);
  CHECK(r->says("not all control paths"));
}

TEST("a return inside a nested block counts") {
  CHECK(analyze("fn f() -> int { { { return 1; } } }")->ok);
}

TEST("else-if chains are followed to the end") {
  CHECK(analyze("fn f(int n) -> int { if (n > 1) { return 1; } "
                "else if (n > 0) { return 2; } else { return 3; } }")
            ->ok);
  // Missing the final else leaves a path with no return.
  CHECK(!analyze("fn f(int n) -> int { if (n > 1) { return 1; } "
                 "else if (n > 0) { return 2; } }")
             ->ok);
}

// ---------------------------------------------------------------------------
// Entry point (LANG-46)
// ---------------------------------------------------------------------------

TEST("a program must define main") {
  auto r = analyze("fn f() -> int { return 1; }", /*requireEntryPoint=*/true);
  CHECK(!r->ok);
  CHECK(r->says("program has no 'main' function"));
}

TEST("main must take no parameters and return int") {
  CHECK(!analyze("fn main(int a) -> int { return 0; }", true)->ok);
  CHECK(!analyze("fn main() -> float { return 0.0; }", true)->ok);
  CHECK(!analyze("fn main() { }", true)->ok);
  CHECK(analyze("fn main() -> int { return 0; }", true)->ok);
}

// ---------------------------------------------------------------------------
// Annotation (FE-28)
// ---------------------------------------------------------------------------

TEST("every expression carries a resolved type after analysis") {
  auto r = analyze("fn f(int n) -> int { return n * 2 + 1; }");
  CHECK(r->ok);
  const std::string tree = r->tree();
  CHECK(tree.find("VarRef 'n' : int") != std::string::npos);
  CHECK(tree.find("IntLiteral 2 : int") != std::string::npos);
  CHECK(tree.find("BinaryExpr '*' : int") != std::string::npos);
  CHECK(tree.find("BinaryExpr '+' : int") != std::string::npos);
}

TEST("variable references are linked to their declaration") {
  auto r = analyze("fn f() -> int { int x = 1; return x; }");
  CHECK(r->ok);

  const FunctionDecl& fn = *r->program->functions()[0];
  const auto* body = fn.body();
  const auto* ret = static_cast<const ReturnStmt*>(body->statements()[1].get());
  const auto* ref = static_cast<const VarRefExpr*>(ret->value());

  CHECK(ref->symbol() != nullptr);
  CHECK_EQ(ref->symbol()->name, std::string("x"));
  CHECK_EQ(ref->symbol()->type, Type::getInt());

  const auto* decl = static_cast<const VarDeclStmt*>(body->statements()[0].get());
  // The reference must resolve to the *same* symbol object as the declaration.
  CHECK_EQ(ref->symbol(), decl->symbol());
}

TEST("calls are linked to the callee symbol") {
  auto r = analyze("fn g(int a) -> int { return a; } fn f() -> int { return g(1); }");
  CHECK(r->ok);

  const FunctionDecl& caller = *r->program->functions()[1];
  const auto* ret = static_cast<const ReturnStmt*>(caller.body()->statements()[0].get());
  const auto* call = static_cast<const CallExpr*>(ret->value());

  CHECK(call->symbol() != nullptr);
  CHECK_EQ(call->symbol()->name, std::string("g"));
  CHECK_EQ(call->symbol()->paramTypes.size(), std::size_t{1});
  CHECK_EQ(call->symbol(), r->program->functions()[0]->symbol());
}

// ---------------------------------------------------------------------------
// Error behaviour
// ---------------------------------------------------------------------------

TEST("independent semantic errors are all reported in one run") {
  auto r = analyze("fn f() {\n"
                   "  int a = 1.5;\n"
                   "  bool b = 2;\n"
                   "  undefined = 3;\n"
                   "}\n");
  CHECK_EQ(r->errors, 3u);
}

TEST("a failed subexpression does not cascade into its parent") {
  // One error for the unknown name, and nothing extra about the '+' whose
  // operand type could not be determined.
  auto r = analyze("fn f() -> int { return unknown + 1; }");
  CHECK_EQ(r->errors, 1u);
  CHECK(r->says("use of undeclared variable 'unknown'"));
}

TEST("analysis of a program with parse errors does not crash") {
  for (const char* src : {"fn f( { }", "fn f() { int = ; }", "fn", "", "}"}) {
    auto r = analyze(src);
    CHECK(r->program != nullptr);
  }
}
