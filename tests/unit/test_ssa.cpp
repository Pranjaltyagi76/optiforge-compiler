#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "TestHarness.h"
#include "optiforge/analysis/AnalysisManager.h"
#include "optiforge/analysis/SSAVerifier.h"
#include "optiforge/frontend/Lexer.h"
#include "optiforge/frontend/Parser.h"
#include "optiforge/frontend/Sema.h"
#include "optiforge/frontend/Symbol.h"
#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Module.h"
#include "optiforge/ir/Printer.h"
#include "optiforge/ir/Verifier.h"
#include "optiforge/irgen/IRGen.h"
#include "optiforge/support/Diagnostic.h"
#include "optiforge/support/SourceManager.h"
#include "optiforge/transforms/SSA.h"

using namespace optiforge;

namespace {

struct Compiled {
  SourceManager sm;
  std::ostringstream diagOut;
  SymbolTable symbols;
  std::unique_ptr<Program> ast;
  std::unique_ptr<ir::Module> module;
  analysis::AnalysisManager manager;

  ir::Function& requireFunction(const std::string& name) const {
    ir::Function* found = module == nullptr ? nullptr : module->findFunction(name);
    if (found == nullptr) {
      std::cerr << "test fixture failed to compile '" << name << "':\n" << diagOut.str();
      std::abort();
    }
    return *found;
  }

  std::string ir() const {
    std::ostringstream out;
    if (module != nullptr) {
      ir::printModule(*module, out);
    }
    return out.str();
  }

  bool structurallyValid() const {
    ir::Verifier verifier;
    return module != nullptr && verifier.verify(*module);
  }

  std::string structuralErrors() const {
    ir::Verifier verifier;
    if (module != nullptr) {
      verifier.verify(*module);
    }
    std::ostringstream out;
    verifier.printErrors(out);
    return out.str();
  }

  std::vector<std::string> ssaErrors() {
    return module == nullptr ? std::vector<std::string>{}
                             : analysis::verifySSA(*module, manager);
  }
};

std::unique_ptr<Compiled> compile(std::string source) {
  auto result = std::make_unique<Compiled>();
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
  IRGen irgen(diags, "t.of", result->sm.contentHash(file));
  result->module = irgen.run(*result->ast);
  return result;
}

/// Compiles and promotes to SSA.
std::unique_ptr<Compiled> toSSA(std::string source) {
  auto c = compile(std::move(source));
  if (c->module != nullptr) {
    transforms::promoteMemoryToRegisters(*c->module, c->manager);
  }
  return c;
}

std::size_t countOf(const std::string& haystack, const std::string& needle) {
  std::size_t count = 0;
  for (std::size_t i = haystack.find(needle); i != std::string::npos;
       i = haystack.find(needle, i + needle.size())) {
    ++count;
  }
  return count;
}

std::size_t opcodeCount(const ir::Function& function, ir::Opcode opcode) {
  std::size_t count = 0;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block->instructions()) {
      if (instruction->opcode() == opcode) {
        ++count;
      }
    }
  }
  return count;
}

}  // namespace

// ---------------------------------------------------------------------------
// mem2reg
// ---------------------------------------------------------------------------

TEST("a straight-line function loses all of its memory traffic") {
  auto c = toSSA("fn f(int a) -> int { int x = a + 1; return x * 2; }");
  const ir::Function& fn = c->requireFunction("f");
  CHECK_EQ(opcodeCount(fn, ir::Opcode::Alloca), std::size_t{0});
  CHECK_EQ(opcodeCount(fn, ir::Opcode::Load), std::size_t{0});
  CHECK_EQ(opcodeCount(fn, ir::Opcode::Store), std::size_t{0});
  CHECK(c->structurallyValid());
  CHECK(c->ssaErrors().empty());
}

TEST("no phi is needed where control flow does not join") {
  auto c = toSSA("fn f(int a) -> int { int x = a; return x; }");
  CHECK_EQ(opcodeCount(c->requireFunction("f"), ir::Opcode::Phi), std::size_t{0});
}

TEST("an if that assigns on both paths gets a phi at the merge") {
  auto c = toSSA(
      "fn f(int n) -> int { int x = 0; if (n > 0) { x = 1; } else { x = 2; } return x; }");
  const ir::Function& fn = c->requireFunction("f");
  CHECK_EQ(opcodeCount(fn, ir::Opcode::Phi), std::size_t{1});
  CHECK(c->ir().find("phi i64 [ 1, if.then.1 ], [ 2, if.else.2 ]") != std::string::npos);
}

TEST("a loop header gets a phi for every variable the loop updates") {
  auto c = toSSA(
      "fn f(int n) -> int { int t = 0; while (n > 0) { t = t + n; n = n - 1; } return t; }");
  const ir::Function& fn = c->requireFunction("f");
  // One for t, one for n.
  CHECK_EQ(opcodeCount(fn, ir::Opcode::Phi), std::size_t{2});
  CHECK(c->ssaErrors().empty());
}

TEST("phi operands name the predecessor they arrive from") {
  auto c = toSSA(
      "fn f(int n) -> int { int t = 0; while (n > 0) { t = t + 1; n = n - 1; } return t; }");
  const std::string text = c->ir();
  CHECK(text.find(", entry ]") != std::string::npos);
  CHECK(text.find(", while.body.2 ]") != std::string::npos);
}

TEST("a variable untouched by the loop needs no phi") {
  auto c = toSSA(
      "fn f(int n) -> int { int k = 7; while (n > 0) { n = n - 1; } return k; }");
  const ir::Function& fn = c->requireFunction("f");
  // Only n changes, so only n gets a phi.
  CHECK_EQ(opcodeCount(fn, ir::Opcode::Phi), std::size_t{1});
}

TEST("nested loops place phis at both headers") {
  auto c = toSSA(
      "fn f(int n, int m) -> int {\n"
      "  int t = 0;\n"
      "  while (n > 0) { int j = m; while (j > 0) { t = t + 1; j = j - 1; } n = n - 1; }\n"
      "  return t;\n"
      "}\n");
  CHECK(opcodeCount(c->requireFunction("f"), ir::Opcode::Phi) >= std::size_t{3});
  CHECK(c->ssaErrors().empty());
}

TEST("parameters are promoted like any other slot") {
  auto c = toSSA("fn f(int a) -> int { a = a + 1; return a; }");
  const ir::Function& fn = c->requireFunction("f");
  CHECK_EQ(opcodeCount(fn, ir::Opcode::Alloca), std::size_t{0});
  // The argument itself is used directly.
  CHECK(c->ir().find("%a") != std::string::npos);
}

TEST("short-circuit lowering is fully promoted") {
  auto c = toSSA("fn f(int a, int b) -> bool { return a > 0 && b > 0; }");
  const ir::Function& fn = c->requireFunction("f");
  CHECK_EQ(opcodeCount(fn, ir::Opcode::Alloca), std::size_t{0});
  CHECK_EQ(opcodeCount(fn, ir::Opcode::Phi), std::size_t{1});
  CHECK(c->ssaErrors().empty());
}

TEST("promotion reports how many slots it removed") {
  auto c = compile("fn f(int a) -> int { int x = a; int y = x; return y; }");
  const std::size_t promoted =
      transforms::promoteMemoryToRegisters(c->requireFunction("f"), c->manager);
  // The parameter's slot plus x and y.
  CHECK_EQ(promoted, std::size_t{3});
}

TEST("running mem2reg twice changes nothing the second time") {
  auto c = toSSA("fn f(int n) -> int { int t = 0; while (n > 0) { t = t + 1; n = n - 1; } return t; }");
  const std::string first = c->ir();
  transforms::promoteMemoryToRegisters(*c->module, c->manager);
  CHECK_EQ(c->ir(), first);
}

TEST("every promoted function still passes both verifiers") {
  const char* programs[] = {
      "fn f() -> int { return 1; }",
      "fn f(int n) -> int { if (n > 0) { return 1; } return 0; }",
      "fn f(int n) -> int { if (n > 0) { return 1; } else { return 2; } }",
      "fn f(int n) -> int { int t = 0; while (n > 0) { t = t + n; n = n - 1; } return t; }",
      "fn f(int a, int b) -> bool { return a > 0 || b > 0; }",
      "fn f(int i) -> float { float x = i; return x * 2 + i; }",
      "fn f(int n) -> int { int a = 1; int b = 2; while (n > 0) { int t = a; a = b; b = t; n = n - 1; } return a; }",
      "fn f(int n) -> int { while (n > 0) { if (n > 5) { n = n - 2; } else { n = n - 1; } } return n; }",
      "fn f() -> int { int x; return x; }",
  };

  for (const char* source : programs) {
    auto c = toSSA(source);
    if (!c->structurallyValid()) {
      ::optiforge::test::reportFailure(__FILE__, __LINE__,
                                       std::string("structurally invalid: ") + source +
                                           "\n" + c->structuralErrors());
    }
    const std::vector<std::string> errors = c->ssaErrors();
    if (!errors.empty()) {
      std::string detail;
      for (const std::string& error : errors) {
        detail += "  " + error + "\n";
      }
      ::optiforge::test::reportFailure(__FILE__, __LINE__,
                                       std::string("invalid SSA: ") + source + "\n" + detail);
    }
  }
}

// ---------------------------------------------------------------------------
// Critical edges
// ---------------------------------------------------------------------------

TEST("short-circuit lowering contains a critical edge, and it is split") {
  // entry has two successors and and.end has two predecessors, so the direct
  // edge between them is critical: a copy placed in entry would also run on
  // the path through and.rhs.
  auto c = toSSA("fn f(int a, int b) -> bool { return a > 0 && b > 0; }");
  ir::Function& fn = c->requireFunction("f");
  CHECK_EQ(transforms::splitCriticalEdges(fn), std::size_t{1});
  CHECK(c->structurallyValid());
  CHECK(c->ir().find("crit.edge") != std::string::npos);
}

TEST("splitting is idempotent") {
  auto c = toSSA("fn f(int a, int b) -> bool { return a > 0 && b > 0; }");
  ir::Function& fn = c->requireFunction("f");
  transforms::splitCriticalEdges(fn);
  CHECK_EQ(transforms::splitCriticalEdges(fn), std::size_t{0});
}

TEST("a simple loop has no critical edges") {
  auto c = toSSA("fn f(int n) -> int { while (n > 0) { n = n - 1; } return n; }");
  CHECK_EQ(transforms::splitCriticalEdges(c->requireFunction("f")), std::size_t{0});
}

TEST("splitting keeps phi incoming blocks consistent") {
  auto c = toSSA("fn f(int a, int b) -> bool { return a > 0 && b > 0; }");
  ir::Function& fn = c->requireFunction("f");
  transforms::splitCriticalEdges(fn);
  // If the phi still named the old predecessor its arity check would fail.
  CHECK(c->ssaErrors().empty());
}

// ---------------------------------------------------------------------------
// SSA destruction
// ---------------------------------------------------------------------------

TEST("destruction removes every phi") {
  auto c = toSSA(
      "fn f(int n) -> int { int t = 0; while (n > 0) { t = t + 1; n = n - 1; } return t; }");
  ir::Function& fn = c->requireFunction("f");
  const std::size_t phis = opcodeCount(fn, ir::Opcode::Phi);
  CHECK(phis > 0);

  CHECK_EQ(transforms::destroySSA(fn), phis);
  CHECK_EQ(opcodeCount(fn, ir::Opcode::Phi), std::size_t{0});
  CHECK(c->structurallyValid());
}

TEST("destruction on a function with no phis does nothing") {
  auto c = toSSA("fn f(int a) -> int { return a + 1; }");
  CHECK_EQ(transforms::destroySSA(c->requireFunction("f")), std::size_t{0});
}

TEST("every copy from a phi shares that phi's location") {
  auto c = toSSA(
      "fn f(int n) -> int { int t = 0; while (n > 0) { t = t + 1; n = n - 1; } return t; }");
  ir::Function& fn = c->requireFunction("f");
  transforms::destroySSA(fn);

  // Each copy must point at a coalescing root, or the value would land in a
  // slot nobody reads.
  std::size_t copies = 0;
  for (const auto& block : fn.blocks()) {
    for (const auto& instruction : block->instructions()) {
      if (instruction->opcode() != ir::Opcode::Copy) {
        continue;
      }
      ++copies;
      CHECK(instruction->slotAlias() != nullptr);
    }
  }
  CHECK(copies > 0);
}

TEST("the swap problem produces a cycle-breaking temporary") {
  // At the header, phi(a) takes b and phi(b) takes a. Emitting those copies in
  // sequence would leave both holding the same value.
  auto c = toSSA(
      "fn f(int n) -> int { int a = 1; int b = 2;"
      " while (n > 0) { int t = a; a = b; b = t; n = n - 1; } return a; }");
  ir::Function& fn = c->requireFunction("f");
  transforms::destroySSA(fn);
  CHECK(c->structurallyValid());

  // The cycle is broken by a copy that is *not* coalesced onto a root.
  bool foundTemporary = false;
  for (const auto& block : fn.blocks()) {
    for (const auto& instruction : block->instructions()) {
      if (instruction->opcode() == ir::Opcode::Copy &&
          instruction->slotAlias() == nullptr) {
        foundTemporary = true;
      }
    }
  }
  CHECK(foundTemporary);
}

TEST("construction then destruction leaves valid IR for every shape") {
  const char* programs[] = {
      "fn f(int n) -> int { if (n > 0) { return 1; } else { return 2; } }",
      "fn f(int n) -> int { int t = 0; while (n > 0) { t = t + n; n = n - 1; } return t; }",
      "fn f(int a, int b) -> bool { return a > 0 && b > 0; }",
      "fn f(int a, int b) -> bool { return a > 0 || b > 0; }",
      "fn f(int n) -> int { int a = 1; int b = 2; while (n > 0) { int t = a; a = b; b = t; n = n - 1; } return a + b; }",
      "fn f(int n, int m) -> int { int t = 0; while (n > 0) { int j = m; while (j > 0) { t = t + 1; j = j - 1; } n = n - 1; } return t; }",
  };

  for (const char* source : programs) {
    auto c = toSSA(source);
    transforms::destroySSA(*c->module);
    if (!c->structurallyValid()) {
      ::optiforge::test::reportFailure(__FILE__, __LINE__,
                                       std::string("invalid after destruction: ") + source +
                                           "\n" + c->structuralErrors());
    }
    CHECK_EQ(opcodeCount(c->requireFunction("f"), ir::Opcode::Phi), std::size_t{0});
  }
}

// ---------------------------------------------------------------------------
// SSA verifier
// ---------------------------------------------------------------------------

TEST("the SSA verifier accepts correctly promoted IR") {
  auto c = toSSA(
      "fn f(int n) -> int { int t = 0; while (n > 0) { t = t + n; n = n - 1; } return t; }");
  CHECK(c->ssaErrors().empty());
}

TEST("the structural verifier rejects a phi with the wrong arity") {
  // The SSA verifier runs once, at the end of the pipeline. Phi arity is the
  // invariant a CFG-editing pass is most likely to break, so the structural
  // verifier checks it too -- that is what lets --verify-each name the pass
  // responsible instead of blaming whatever ran last.
  auto c = toSSA(
      "fn f(int n) -> int { int t = 0; while (n > 0) { t = t + 1; n = n - 1; } return t; }");
  ir::Function& fn = c->requireFunction("f");

  for (const auto& block : fn.blocks()) {
    bool done = false;
    for (const auto& instruction : block->instructions()) {
      if (instruction->opcode() == ir::Opcode::Phi) {
        instruction->addIncoming(c->module->getInt(0), fn.entry());
        done = true;
        break;
      }
    }
    if (done) {
      break;
    }
  }

  ir::Verifier verifier;
  CHECK(!verifier.verify(fn));
}

TEST("the SSA verifier rejects a phi with the wrong arity") {
  auto c = toSSA(
      "fn f(int n) -> int { int t = 0; while (n > 0) { t = t + 1; n = n - 1; } return t; }");
  ir::Function& fn = c->requireFunction("f");

  // Corrupt one phi the way a buggy pass would.
  for (const auto& block : fn.blocks()) {
    for (const auto& instruction : block->instructions()) {
      if (instruction->opcode() == ir::Opcode::Phi) {
        instruction->addIncoming(c->module->getInt(0), fn.entry());
        goto corrupted;
      }
    }
  }
corrupted:
  c->manager.invalidateAll();
  const std::vector<std::string> errors = c->ssaErrors();
  CHECK(!errors.empty());
  bool mentionsArity = false;
  for (const std::string& error : errors) {
    if (error.find("incoming edge") != std::string::npos) {
      mentionsArity = true;
    }
  }
  CHECK(mentionsArity);
}

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------

TEST("promotion is deterministic") {
  const char* source =
      "fn f(int n) -> int { int t = 0; while (n > 0) { t = t + n; n = n - 1; } return t; }";
  CHECK_EQ(toSSA(source)->ir(), toSSA(source)->ir());
}

TEST("destruction is deterministic") {
  const char* source =
      "fn f(int n) -> int { int a = 1; int b = 2;"
      " while (n > 0) { int t = a; a = b; b = t; n = n - 1; } return a; }";
  const auto run = [&]() {
    auto c = toSSA(source);
    transforms::destroySSA(*c->module);
    return c->ir();
  };
  CHECK_EQ(run(), run());
}

TEST("promotion removes memory traffic rather than adding it") {
  const char* source =
      "fn f(int n) -> int { int t = 0; while (n > 0) { t = t + n; n = n - 1; } return t; }";
  auto before = compile(source);
  const std::size_t loadsBefore =
      opcodeCount(before->requireFunction("f"), ir::Opcode::Load) +
      opcodeCount(before->requireFunction("f"), ir::Opcode::Store);

  auto after = toSSA(source);
  const std::size_t loadsAfter =
      opcodeCount(after->requireFunction("f"), ir::Opcode::Load) +
      opcodeCount(after->requireFunction("f"), ir::Opcode::Store);

  CHECK(loadsBefore > 0);
  CHECK_EQ(loadsAfter, std::size_t{0});
  (void)countOf;
}
