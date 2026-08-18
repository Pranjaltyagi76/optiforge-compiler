#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "TestHarness.h"
#include "optiforge/frontend/ASTPrinter.h"
#include "optiforge/frontend/Lexer.h"
#include "optiforge/frontend/Parser.h"
#include "optiforge/support/Diagnostic.h"
#include "optiforge/support/SourceManager.h"

using namespace optiforge;

namespace {

struct ParseResult {
  SourceManager sm;
  std::ostringstream diagOut;
  std::unique_ptr<Program> program;
  bool parseError = false;
  unsigned errors = 0;

  std::string diagnostics() const { return diagOut.str(); }

  /// AST rendered by the same printer `--emit=ast` uses, so a test failure
  /// shows the exact tree the user would see.
  std::string tree() const {
    std::ostringstream out;
    if (program != nullptr) {
      printAST(*program, out);
    }
    return out.str();
  }
};

std::unique_ptr<ParseResult> parse(std::string source) {
  auto result = std::make_unique<ParseResult>();
  const FileID file = result->sm.addBuffer("t.of", std::move(source));
  DiagnosticEngine diags(result->sm, result->diagOut);

  Lexer lexer(result->sm, file, diags);
  const std::vector<Token> tokens = lexer.tokenize();

  Parser parser(tokens, diags);
  result->program = parser.parseProgram();
  result->parseError = parser.hadError();
  result->errors = diags.errorCount();
  return result;
}

/// Parses a single expression by wrapping it in a minimal function, then
/// returns just the expression subtree, de-indented.
std::string exprTree(const std::string& expression) {
  auto r = parse("fn t() -> int { int v = " + expression + "; }");
  if (r->parseError) {
    return "<parse error>\n" + r->diagnostics();
  }
  std::istringstream in(r->tree());
  std::string line;
  std::string out;
  bool collecting = false;
  while (std::getline(in, line)) {
    if (line.find("VarDeclStmt") != std::string::npos) {
      collecting = true;
      continue;
    }
    if (collecting) {
      // Strip the eight spaces of enclosing Program/Function/Block indentation.
      out += (line.size() > 8 ? line.substr(8) : line) + "\n";
    }
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Precedence and associativity
// ---------------------------------------------------------------------------

TEST("multiplication binds tighter than addition") {
  CHECK_EQ(exprTree("1 + 2 * 3"), std::string("BinaryExpr '+'\n"
                                              "  IntLiteral 1\n"
                                              "  BinaryExpr '*'\n"
                                              "    IntLiteral 2\n"
                                              "    IntLiteral 3\n"));
}

TEST("parentheses override precedence") {
  CHECK_EQ(exprTree("(1 + 2) * 3"), std::string("BinaryExpr '*'\n"
                                                "  BinaryExpr '+'\n"
                                                "    IntLiteral 1\n"
                                                "    IntLiteral 2\n"
                                                "  IntLiteral 3\n"));
}

TEST("subtraction is left-associative") {
  // Right-associativity here would silently compute 10-(4-3)=9 instead of 3.
  CHECK_EQ(exprTree("10 - 4 - 3"), std::string("BinaryExpr '-'\n"
                                               "  BinaryExpr '-'\n"
                                               "    IntLiteral 10\n"
                                               "    IntLiteral 4\n"
                                               "  IntLiteral 3\n"));
}

TEST("division and modulo share precedence with multiplication") {
  CHECK_EQ(exprTree("8 / 4 % 3"), std::string("BinaryExpr '%'\n"
                                              "  BinaryExpr '/'\n"
                                              "    IntLiteral 8\n"
                                              "    IntLiteral 4\n"
                                              "  IntLiteral 3\n"));
}

TEST("the full precedence ladder nests correctly") {
  // || < && < == < relational < additive < multiplicative
  CHECK_EQ(exprTree("1 < 2 && 3 == 4 || 5 + 6 * 7 > 8"),
           std::string("BinaryExpr '||'\n"
                       "  BinaryExpr '&&'\n"
                       "    BinaryExpr '<'\n"
                       "      IntLiteral 1\n"
                       "      IntLiteral 2\n"
                       "    BinaryExpr '=='\n"
                       "      IntLiteral 3\n"
                       "      IntLiteral 4\n"
                       "  BinaryExpr '>'\n"
                       "    BinaryExpr '+'\n"
                       "      IntLiteral 5\n"
                       "      BinaryExpr '*'\n"
                       "        IntLiteral 6\n"
                       "        IntLiteral 7\n"
                       "    IntLiteral 8\n"));
}

TEST("unary operators bind tighter than any binary operator") {
  CHECK_EQ(exprTree("-1 * 2"), std::string("BinaryExpr '*'\n"
                                           "  UnaryExpr '-'\n"
                                           "    IntLiteral 1\n"
                                           "  IntLiteral 2\n"));
}

TEST("unary operators stack") {
  CHECK_EQ(exprTree("!!true"), std::string("UnaryExpr '!'\n"
                                           "  UnaryExpr '!'\n"
                                           "    BoolLiteral true\n"));
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

TEST("calls parse with zero, one, and several arguments") {
  CHECK_EQ(exprTree("f()"), std::string("CallExpr 'f'\n"));
  CHECK_EQ(exprTree("f(1)"), std::string("CallExpr 'f'\n"
                                         "  IntLiteral 1\n"));
  CHECK_EQ(exprTree("f(1, 2, 3)"), std::string("CallExpr 'f'\n"
                                               "  IntLiteral 1\n"
                                               "  IntLiteral 2\n"
                                               "  IntLiteral 3\n"));
}

TEST("call arguments may themselves be expressions") {
  CHECK_EQ(exprTree("f(a + 1, g(b))"), std::string("CallExpr 'f'\n"
                                                   "  BinaryExpr '+'\n"
                                                   "    VarRef 'a'\n"
                                                   "    IntLiteral 1\n"
                                                   "  CallExpr 'g'\n"
                                                   "    VarRef 'b'\n"));
}

TEST("an identifier without parentheses is a variable reference") {
  CHECK_EQ(exprTree("x"), std::string("VarRef 'x'\n"));
}

TEST("literal kinds are distinguished") {
  CHECK_EQ(exprTree("1"), std::string("IntLiteral 1\n"));
  CHECK_EQ(exprTree("1.5"), std::string("FloatLiteral 1.5\n"));
  CHECK_EQ(exprTree("true"), std::string("BoolLiteral true\n"));
  CHECK_EQ(exprTree("false"), std::string("BoolLiteral false\n"));
}

// ---------------------------------------------------------------------------
// Declarations and statements
// ---------------------------------------------------------------------------

TEST("a function with no parameters and no return type is void") {
  auto r = parse("fn f() {}");
  CHECK(!r->parseError);
  CHECK_EQ(r->tree(), std::string("Program\n"
                                  "  FunctionDecl 'f' -> void\n"
                                  "    Block\n"));
}

TEST("parameters and return type are recorded") {
  auto r = parse("fn f(int a, float b, bool c) -> float { return b; }");
  CHECK(!r->parseError);
  CHECK_EQ(r->tree(), std::string("Program\n"
                                  "  FunctionDecl 'f' -> float\n"
                                  "    ParamDecl 'a' : int\n"
                                  "    ParamDecl 'b' : float\n"
                                  "    ParamDecl 'c' : bool\n"
                                  "    Block\n"
                                  "      ReturnStmt\n"
                                  "        VarRef 'b'\n"));
}

TEST("a declaration without an initializer has no child") {
  auto r = parse("fn f() { int x; }");
  CHECK(!r->parseError);
  CHECK(r->tree().find("VarDeclStmt 'x' : int\n") != std::string::npos);
  CHECK(r->tree().find("IntLiteral") == std::string::npos);
}

TEST("a bare return has no value child") {
  auto r = parse("fn f() { return; }");
  CHECK(!r->parseError);
  CHECK(r->tree().find("ReturnStmt\n") != std::string::npos);
}

TEST("assignment is a statement, distinguished by one token of lookahead") {
  auto r = parse("fn f() { x = 1; }");
  CHECK(!r->parseError);
  CHECK(r->tree().find("AssignStmt 'x'") != std::string::npos);
}

TEST("a call in statement position is an expression statement") {
  auto r = parse("fn f() { g(1); }");
  CHECK(!r->parseError);
  CHECK(r->tree().find("ExprStmt") != std::string::npos);
  CHECK(r->tree().find("CallExpr 'g'") != std::string::npos);
}

TEST("if without else has no else branch") {
  auto r = parse("fn f() { if (true) { return; } }");
  CHECK(!r->parseError);
  CHECK(r->tree().find("cond:") != std::string::npos);
  CHECK(r->tree().find("then:") != std::string::npos);
  CHECK(r->tree().find("else:") == std::string::npos);
}

TEST("else if chains as a nested IfStmt, not a wrapper block") {
  auto r = parse("fn f() { if (a) { } else if (b) { } else { } }");
  CHECK(!r->parseError);
  const std::string tree = r->tree();
  // Two IfStmt nodes, and the second is inside an else.
  std::size_t count = 0;
  for (std::size_t i = tree.find("IfStmt"); i != std::string::npos;
       i = tree.find("IfStmt", i + 1)) {
    ++count;
  }
  CHECK_EQ(count, std::size_t{2});
  CHECK(tree.find("else:") != std::string::npos);
}

TEST("while loops record condition and body") {
  auto r = parse("fn f() { while (i < n) { i = i + 1; } }");
  CHECK(!r->parseError);
  CHECK(r->tree().find("WhileStmt") != std::string::npos);
  CHECK(r->tree().find("body:") != std::string::npos);
}

TEST("nested blocks are preserved") {
  auto r = parse("fn f() { { { int x; } } }");
  CHECK(!r->parseError);
  std::size_t blocks = 0;
  const std::string tree = r->tree();
  for (std::size_t i = tree.find("Block"); i != std::string::npos;
       i = tree.find("Block", i + 1)) {
    ++blocks;
  }
  CHECK_EQ(blocks, std::size_t{3});
}

TEST("several functions parse into one program") {
  auto r = parse("fn a() {} fn b() {} fn c() {}");
  CHECK(!r->parseError);
  CHECK_EQ(r->program->functions().size(), std::size_t{3});
}

// ---------------------------------------------------------------------------
// Error reporting and recovery
// ---------------------------------------------------------------------------

TEST("independent errors are reported in one run, not just the first") {
  auto r = parse("fn f() {\n  int x = ;\n  int y = ;\n  int z = ;\n}\n");
  CHECK(r->parseError);
  CHECK_EQ(r->errors, 3u);
}

TEST("a missing semicolon is reported at the end of the previous token") {
  auto r = parse("fn f() {\n  int y = 5\n  int z = 6;\n}\n");
  CHECK(r->parseError);
  // The semicolon belongs at line 2 column 12, not wherever the next token is.
  CHECK(r->diagnostics().find("t.of:2:12: error: expected ';'") != std::string::npos);
}

TEST("one broken function header does not cascade over the rest of the file") {
  auto r = parse("fn broken( -> int {\n  return 1;\n}\n");
  CHECK(r->parseError);
  // Before top-level recovery was scoped to declarations, this produced one
  // error per leftover token.
  CHECK_EQ(r->errors, 1u);
}

TEST("a valid function after a broken one is still parsed") {
  auto r = parse("fn bad( { }\nfn good() -> int { return 7; }\n");
  CHECK(r->parseError);
  CHECK_EQ(r->program->functions().size(), std::size_t{1});
  CHECK_EQ(r->program->functions()[0]->name(), std::string("good"));
}

TEST("top-level junk is reported once, not per token") {
  auto r = parse("int x = 1; float y = 2;\n");
  CHECK(r->parseError);
  CHECK_EQ(r->errors, 1u);
}

TEST("statement recovery resumes at the next statement") {
  auto r = parse("fn f() {\n  int x = ;\n  int y = 2;\n  return y;\n}\n");
  CHECK(r->parseError);
  CHECK_EQ(r->errors, 1u);
}

// ---------------------------------------------------------------------------
// Robustness (CLI-10: malformed input must never hang or crash)
// ---------------------------------------------------------------------------

TEST("pathological input terminates instead of spinning") {
  const char* cases[] = {
      "",           "fn",        "fn f",      "fn f(",      "fn f()",
      "fn f() {",   "}",         "{",         ")",          "(((((",
      ")))))",      "fn f() { (((( }",        "fn f() { return return return; }",
      ";;;;;;",     "fn f() { if if if }",    "-",          "fn f() { 1 + }",
      "fn f() { x = = = 1; }",   "fn () {}",  "fn f() -> {}",
  };
  for (const char* src : cases) {
    auto r = parse(src);
    // The only requirement is that we get here at all: no hang, no crash.
    CHECK(r->program != nullptr);
  }
}

TEST("nesting beyond the limit reports cleanly instead of overflowing the stack") {
  // Measured before the depth guard existed: this crashed at roughly 2000
  // nested parentheses. CLI-10 requires a diagnostic, not a crash.
  struct Case {
    const char* prefix;
    const char* middle;
    const char* suffix;
    int count;
  };
  const Case cases[] = {
      {"fn f() -> int { return ", "1", "; }", 5000},   // parentheses
      {"fn f() { ", "", " }", 5000},                    // nested blocks
  };

  {
    std::string src = "fn f() -> int { return " + std::string(5000, '(') + "1" +
                      std::string(5000, ')') + "; }";
    auto r = parse(src);
    CHECK(r->parseError);
    CHECK(r->diagnostics().find("nests too deeply") != std::string::npos);
  }
  {
    std::string src = "fn f() {" + std::string(5000, '{') + std::string(5000, '}') + "}";
    auto r = parse(src);
    CHECK(r->parseError);
    CHECK(r->diagnostics().find("nests too deeply") != std::string::npos);
  }
  {
    std::string src = "fn f() -> bool { return " + std::string(5000, '!') + "true; }";
    auto r = parse(src);
    CHECK(r->parseError);
    CHECK(r->diagnostics().find("nests too deeply") != std::string::npos);
  }
  (void)cases;
}

TEST("hitting the nesting limit stops parsing rather than retrying per token") {
  // Reporting and continuing re-descended the full depth for every remaining
  // token, which turned a deep input into an effective hang.
  std::string src = "fn f() {" + std::string(20000, '{') + std::string(20000, '}') + "}";
  auto r = parse(src);
  CHECK(r->parseError);
  // Exactly one diagnostic: the bail-out must not fire once per token.
  CHECK_EQ(r->errors, 1u);
}

TEST("deeply nested expressions do not break the parser") {
  std::string source = "fn f() -> int { return ";
  source += std::string(200, '(');
  source += "1";
  source += std::string(200, ')');
  source += "; }";
  auto r = parse(source);
  CHECK(!r->parseError);
}
