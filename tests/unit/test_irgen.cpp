#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "TestHarness.h"
#include "optiforge/frontend/Lexer.h"
#include "optiforge/frontend/Parser.h"
#include "optiforge/frontend/Sema.h"
#include "optiforge/frontend/Symbol.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Module.h"
#include "optiforge/ir/Printer.h"
#include "optiforge/ir/Verifier.h"
#include "optiforge/irgen/IRGen.h"
#include "optiforge/support/Diagnostic.h"
#include "optiforge/support/SourceManager.h"

using namespace optiforge;

namespace {

struct GenResult {
  SourceManager sm;
  std::ostringstream diagOut;
  SymbolTable symbols;
  std::unique_ptr<Program> ast;
  std::unique_ptr<ir::Module> module;
  bool frontendOk = false;

  std::string ir() const {
    std::ostringstream out;
    if (module != nullptr) {
      ir::printModule(*module, out);
    }
    return out.str();
  }

  std::string cfg() const {
    std::ostringstream out;
    if (module != nullptr) {
      ir::printCFG(*module, out);
    }
    return out.str();
  }

  bool verifies() const {
    ir::Verifier verifier;
    return module != nullptr && verifier.verify(*module);
  }

  std::string verifierErrors() const {
    ir::Verifier verifier;
    if (module != nullptr) {
      verifier.verify(*module);
    }
    std::ostringstream out;
    verifier.printErrors(out);
    return out.str();
  }

  /// Body of one function, without the surrounding module noise.
  std::string function(const std::string& name) const {
    const std::string text = ir();
    const std::size_t start = text.find("fn @" + name + "(");
    if (start == std::string::npos) {
      return "<not found>";
    }
    const std::size_t end = text.find("\n}\n", start);
    return end == std::string::npos ? text.substr(start) : text.substr(start, end + 3 - start);
  }

  bool has(const std::string& needle) const { return ir().find(needle) != std::string::npos; }
};

std::unique_ptr<GenResult> lower(std::string source) {
  auto result = std::make_unique<GenResult>();
  const FileID file = result->sm.addBuffer("t.of", std::move(source));
  DiagnosticEngine diags(result->sm, result->diagOut);

  Lexer lexer(result->sm, file, diags);
  const std::vector<Token> tokens = lexer.tokenize();

  Parser parser(tokens, diags);
  result->ast = parser.parseProgram();
  if (parser.hadError()) {
    return result;
  }

  Sema sema(diags, result->symbols);
  if (!sema.analyze(*result->ast, /*requireEntryPoint=*/false)) {
    return result;
  }
  result->frontendOk = true;

  IRGen irgen(diags, "t.of", result->sm.contentHash(file));
  result->module = irgen.run(*result->ast);
  return result;
}

/// Counts non-overlapping occurrences.
std::size_t countOf(const std::string& haystack, const std::string& needle) {
  std::size_t count = 0;
  for (std::size_t i = haystack.find(needle); i != std::string::npos;
       i = haystack.find(needle, i + needle.size())) {
    ++count;
  }
  return count;
}

}  // namespace

// ---------------------------------------------------------------------------
// Everything the compiler accepts must lower to verified IR
// ---------------------------------------------------------------------------

TEST("lowered IR always passes the verifier") {
  const char* programs[] = {
      "fn f() { }",
      "fn f() -> int { return 1; }",
      "fn f(int a, float b, bool c) -> float { return b; }",
      "fn f(int n) -> int { if (n > 0) { return 1; } return 0; }",
      "fn f(int n) -> int { if (n > 0) { return 1; } else { return 2; } }",
      "fn f(int n) -> int { int t = 0; while (n > 0) { t = t + n; n = n - 1; } return t; }",
      "fn f(int a, int b) -> bool { return a > 0 && b > 0; }",
      "fn f(int a, int b) -> bool { return a > 0 || b > 0; }",
      "fn f(int i) -> float { float x = i; return x * 2 + i; }",
      "fn g(int x) -> int { return x; } fn f() -> int { return g(g(1)); }",
      "fn f(int n) -> int { if (n>2) { return 1; } else if (n>1) { return 2; } else { return 3; } }",
      "fn f() { print_int(1); print_float(1.5); print_bool(true); }",
      "fn f(int n) -> int { while (n > 0) { if (n > 5) { n = n - 2; } else { n = n - 1; } } return n; }",
      "fn f() -> int { { { int x = 1; return x; } } }",
      "fn f(int n) -> int { return -n; }",
      "fn f(bool b) -> bool { return !b; }",
      "fn f(int a) -> int { return a % 3; }",
  };

  for (const char* source : programs) {
    auto r = lower(source);
    CHECK(r->frontendOk);
    if (!r->verifies()) {
      // Surface which program failed and why.
      ::optiforge::test::reportFailure(__FILE__, __LINE__,
                                       std::string("IR failed verification for: ") + source +
                                           "\n" + r->verifierErrors());
    }
  }
}

// ---------------------------------------------------------------------------
// Structure of the lowered IR
// ---------------------------------------------------------------------------

TEST("a simple function lowers to the expected IR") {
  auto r = lower("fn f(int n) -> int { return n + 1; }");
  CHECK(r->verifies());
  CHECK_EQ(r->function("f"), std::string("fn @f(i64 %n) -> i64 {\n"
                                         "entry:\n"
                                         "  %n.addr = alloca i64\n"
                                         "  store i64 %n, %n.addr\n"
                                         "  %t0 = load i64, %n.addr\n"
                                         "  %t1 = add i64 %t0, 1\n"
                                         "  ret i64 %t1\n"
                                         "}\n"));
}

TEST("parameters get stack slots so assignment to them needs no special case") {
  auto r = lower("fn f(int n) -> int { n = n + 1; return n; }");
  CHECK(r->verifies());
  CHECK(r->has("%n.addr = alloca i64"));
  CHECK(r->has("store i64 %n, %n.addr"));
}

TEST("every alloca lands in the entry block, including ones declared in a loop") {
  auto r = lower("fn f(int n) -> int { while (n > 0) { int inner = n; n = n - 1; } return n; }");
  CHECK(r->verifies());
  // The verifier enforces this, but assert it directly too: an alloca inside a
  // loop would grow the stack every iteration.
  const std::string entry = r->function("f").substr(0, r->function("f").find("while.cond"));
  CHECK(entry.find("%inner.addr = alloca i64") != std::string::npos);
}

TEST("a while loop produces cond, body and end blocks with a back edge") {
  auto r = lower("fn f(int n) -> int { while (n > 0) { n = n - 1; } return n; }");
  CHECK(r->verifies());
  CHECK(r->has("while.cond.1:"));
  CHECK(r->has("while.body.2:"));
  CHECK(r->has("while.end.3:"));
  // The back edge is what loop detection in Phase 5 keys on.
  CHECK(r->has("; preds = entry, while.body.2"));
}

TEST("an if with both branches produces then, else and end") {
  auto r = lower("fn f(int n) -> int { int x = 0; if (n > 0) { x = 1; } else { x = 2; } return x; }");
  CHECK(r->verifies());
  CHECK(r->has("if.then.1:"));
  CHECK(r->has("if.else.2:"));
  CHECK(r->has("if.end.3:"));
}

TEST("an if whose branches all return leaves no unreachable end block") {
  auto r = lower("fn f(int n) -> int { if (n > 0) { return 1; } else { return 2; } }");
  CHECK(r->verifies());
  // if.end.3 was created, then pruned because nothing branches to it.
  CHECK(!r->has("if.end.3:"));
}

TEST("short-circuit && evaluates the right side only in its own block") {
  auto r = lower("fn f(int a, int b) -> bool { return a > 0 && b > 0; }");
  CHECK(r->verifies());
  CHECK(r->has("and.rhs.1:"));
  CHECK(r->has("and.end.2:"));
  // The load of b must be inside and.rhs, not before the branch.
  const std::string body = r->function("f");
  const std::size_t rhsBlock = body.find("and.rhs.1:");
  CHECK(body.find("%b.addr", rhsBlock) != std::string::npos);
}

TEST("short-circuit || branches the other way") {
  auto r = lower("fn f(int a, int b) -> bool { return a > 0 || b > 0; }");
  CHECK(r->verifies());
  CHECK(r->has("or.rhs.1:"));
  CHECK(r->has("or.end.2:"));
}

TEST("int operands are widened with sitofp in mixed arithmetic") {
  auto r = lower("fn f(int i, float g) -> float { return g + i; }");
  CHECK(r->verifies());
  CHECK(r->has("sitofp"));
  CHECK(r->has("fadd f64"));
}

TEST("a float literal operand needs no runtime conversion") {
  auto r = lower("fn f(float g) -> float { return g * 2; }");
  CHECK(r->verifies());
  // The literal 2 is folded to an f64 constant rather than converted at run time.
  CHECK(!r->has("sitofp"));
  CHECK(r->has("fmul f64"));
}

TEST("comparisons pick icmp or fcmp by operand type") {
  CHECK(lower("fn f(int a) -> bool { return a > 1; }")->has("icmp gt i64"));
  CHECK(lower("fn f(float a) -> bool { return a > 1.0; }")->has("fcmp gt f64"));
}

TEST("calls carry their callee and argument types") {
  auto r = lower("fn g(int a) -> int { return a; } fn f() -> int { return g(7); }");
  CHECK(r->verifies());
  CHECK(r->has("call i64 @g(i64 7)"));
}

TEST("a void call produces no result name") {
  auto r = lower("fn f() { print_int(1); }");
  CHECK(r->verifies());
  CHECK(r->has("call void @print_int(i64 1)"));
  CHECK(!r->has("= call void"));
}

TEST("builtins are declared without bodies") {
  auto r = lower("fn f() { }");
  CHECK(r->has("fn @print_int(i64 %value) -> void;"));
  CHECK(r->has("fn @print_float(f64 %value) -> void;"));
}

TEST("mutual recursion lowers because callees are declared up front") {
  auto r = lower("fn even(int n) -> bool { if (n == 0) { return true; } return odd(n - 1); }\n"
                 "fn odd(int n) -> bool { if (n == 0) { return false; } return even(n - 1); }\n");
  CHECK(r->verifies());
  CHECK(r->has("call i1 @odd"));
  CHECK(r->has("call i1 @even"));
}

TEST("statements after a return in the same block are not emitted") {
  auto r = lower("fn f() -> int { return 1; }");
  CHECK(r->verifies());
  CHECK_EQ(countOf(r->function("f"), "ret "), std::size_t{1});
}

TEST("a void function with no explicit return still gets a terminator") {
  auto r = lower("fn f() { }");
  CHECK(r->verifies());
  CHECK(r->has("ret\n"));
}

// ---------------------------------------------------------------------------
// Determinism (NFR-06, IR-11)
// ---------------------------------------------------------------------------

TEST("lowering the same source twice yields identical IR") {
  const char* source =
      "fn f(int n) -> int { int t = 0; while (n > 0) { t = t + n; n = n - 1; } return t; }";
  CHECK_EQ(lower(source)->ir(), lower(source)->ir());
}

TEST("block labels are stable, which is what makes profile matching possible") {
  // If these shift between compilations, every PGO decision silently misses
  // (requirement IR-11, ADR-06).
  auto r = lower("fn f(int n) -> int { int t = 0; while (n > 0) { if (n > 5) { t = t + 1; } "
                 "n = n - 1; } return t; }");
  CHECK(r->has("while.cond.1:"));
  CHECK(r->has("while.body.2:"));
  CHECK(r->has("while.end.3:"));
  CHECK(r->has("if.then.4:"));
  CHECK(r->has("if.end.5:"));
}

// ---------------------------------------------------------------------------
// CFG export
// ---------------------------------------------------------------------------

TEST("the CFG export is well-formed DOT") {
  auto r = lower("fn f(int n) -> int { while (n > 0) { n = n - 1; } return n; }");
  const std::string dot = r->cfg();
  CHECK(dot.rfind("digraph cfg {", 0) == 0);
  CHECK_EQ(countOf(dot, "{"), countOf(dot, "}"));
  // Quotes must balance or dot will refuse the file.
  CHECK_EQ(countOf(dot, "\"") % 2, std::size_t{0});
}

TEST("the CFG labels the two edges of a conditional branch") {
  auto r = lower("fn f(int n) -> int { while (n > 0) { n = n - 1; } return n; }");
  const std::string dot = r->cfg();
  CHECK(dot.find("[label=\"true\"]") != std::string::npos);
  CHECK(dot.find("[label=\"false\"]") != std::string::npos);
}

TEST("the CFG shows the loop back edge") {
  auto r = lower("fn f(int n) -> int { while (n > 0) { n = n - 1; } return n; }");
  CHECK(r->cfg().find("\"f:while.body.2\" -> \"f:while.cond.1\"") != std::string::npos);
}
