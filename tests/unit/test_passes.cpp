#include <algorithm>
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
#include "optiforge/passes/Pass.h"
#include "optiforge/support/Diagnostic.h"
#include "optiforge/support/SourceManager.h"
#include "optiforge/transforms/SSA.h"

using namespace optiforge;

namespace optiforge::transforms {
void anchorScalarPasses();
void anchorSCCP();
void anchorGVN();
void anchorLICM();
void anchorSimplifyCFG();
void anchorInline();
}  // namespace optiforge::transforms

namespace {

/// Forces the transform translation units to be linked, and with them their
/// pass registrations.
struct KeepRegistrations {
  KeepRegistrations() {
    transforms::anchorScalarPasses();
    transforms::anchorSCCP();
    transforms::anchorGVN();
    transforms::anchorLICM();
    transforms::anchorSimplifyCFG();
    transforms::anchorInline();
  }
};
const KeepRegistrations kKeep;

struct Optimized {
  SourceManager sm;
  std::ostringstream diagOut;
  SymbolTable symbols;
  std::unique_ptr<Program> ast;
  std::unique_ptr<ir::Module> module;
  analysis::AnalysisManager manager;

  ir::Function& requireFunction(const std::string& name) const {
    ir::Function* found = module == nullptr ? nullptr : module->findFunction(name);
    if (found == nullptr) {
      std::cerr << "test fixture failed for '" << name << "':\n" << diagOut.str();
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

  bool valid() const {
    ir::Verifier verifier;
    return module != nullptr && verifier.verify(*module);
  }

  bool has(const std::string& needle) const {
    return ir().find(needle) != std::string::npos;
  }
};

/// Compiles, promotes to SSA, then runs one named pass to a fixed point.
std::unique_ptr<Optimized> runPass(const std::string& passName, std::string source) {
  auto result = std::make_unique<Optimized>();
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
  transforms::promoteMemoryToRegisters(*result->module, result->manager);
  result->manager.invalidateAll();

  if (!passName.empty()) {
    passes::PassManager pipeline;
    if (auto pass = passes::PassRegistry::instance().create(passName)) {
      pipeline.add(std::move(pass));
    }
    pipeline.run(*result->module, result->manager);
  }
  return result;
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

std::size_t instructionCount(const ir::Function& function) {
  std::size_t count = 0;
  for (const auto& block : function.blocks()) {
    count += block->instructions().size();
  }
  return count;
}

}  // namespace

// ---------------------------------------------------------------------------
// Registry and pipelines
// ---------------------------------------------------------------------------

TEST("every pipeline entry names a registered pass") {
  // A typo in a pipeline would silently drop the pass rather than fail.
  for (int level : {0, 1, 2}) {
    for (const std::string& name : passes::pipelineFor(level)) {
      if (!passes::PassRegistry::instance().contains(name)) {
        ::optiforge::test::reportFailure(__FILE__, __LINE__,
                                         "pipeline names unregistered pass: " + name);
      }
    }
  }
}

TEST("-O0 runs no passes at all") {
  CHECK(passes::pipelineFor(0).empty());
}

TEST("-O2 is a superset of -O1") {
  const std::vector<std::string> one = passes::pipelineFor(1);
  const std::vector<std::string> two = passes::pipelineFor(2);
  for (const std::string& name : one) {
    CHECK(std::find(two.begin(), two.end(), name) != two.end());
  }
  CHECK(two.size() > one.size());
}

TEST("a pass can be disabled by name") {
  passes::PassManager withInline;
  passes::buildPipeline(withInline, 2, {});
  passes::PassManager without;
  passes::buildPipeline(without, 2, {"inline"});
  CHECK(without.statistics().size() < withInline.statistics().size());
}

TEST("the registry lists passes in a deterministic order") {
  CHECK(passes::PassRegistry::instance().names() ==
        passes::PassRegistry::instance().names());
}

// ---------------------------------------------------------------------------
// Constant folding
// ---------------------------------------------------------------------------

TEST("constant arithmetic is evaluated") {
  auto r = runPass("constant-folding", "fn f() -> int { return 2 * 3 + 4; }");
  CHECK(r->has("ret i64 10"));
}

TEST("algebraic identities need only one constant") {
  CHECK(runPass("constant-folding", "fn f(int a) -> int { return a + 0; }")
            ->has("ret i64 %a"));
  CHECK(runPass("constant-folding", "fn f(int a) -> int { return a * 1; }")
            ->has("ret i64 %a"));
  CHECK(runPass("constant-folding", "fn f(int a) -> int { return a * 0; }")
            ->has("ret i64 0"));
  CHECK(runPass("constant-folding", "fn f(int a) -> int { return a - a; }")
            ->has("ret i64 0"));
}

TEST("division by zero is left for the target to trap on") {
  auto r = runPass("constant-folding", "fn f() -> int { int z = 0; return 1 / z; }");
  CHECK(r->valid());
  // Folding it would produce a value the hardware never yields.
  CHECK(!r->has("ret i64 0"));
}

TEST("comparing a value with itself folds without knowing the value") {
  CHECK(runPass("constant-folding", "fn f(int a) -> bool { return a == a; }")
            ->has("ret i1 true"));
  CHECK(runPass("constant-folding", "fn f(int a) -> bool { return a < a; }")
            ->has("ret i1 false"));
}

// ---------------------------------------------------------------------------
// SCCP
// ---------------------------------------------------------------------------

TEST("sccp propagates a constant through a phi") {
  auto r = runPass("sccp",
                   "fn f(int n) -> int { int x = 5; if (n > 0) { x = 5; } return x; }");
  CHECK(r->valid());
  CHECK(r->has("ret i64 5"));
}

TEST("sccp folds a branch whose condition is known") {
  auto r = runPass("sccp",
                   "fn f() -> int { int x = 1; if (x > 0) { return 7; } return 9; }");
  CHECK(r->valid());
  // The false arm is unreachable, so no conditional branch survives.
  CHECK_EQ(opcodeCount(r->requireFunction("f"), ir::Opcode::CondBr), std::size_t{0});
}

TEST("sccp keeps a genuinely unknown value unknown") {
  auto r = runPass("sccp", "fn f(int n) -> int { if (n > 0) { return 1; } return 2; }");
  CHECK(r->valid());
  CHECK_EQ(opcodeCount(r->requireFunction("f"), ir::Opcode::CondBr), std::size_t{1});
}

// ---------------------------------------------------------------------------
// Copy propagation
// ---------------------------------------------------------------------------

TEST("a phi whose operands all agree is not a choice") {
  auto r = runPass("copy-propagation",
                   "fn f(int n) -> int { int x = 3; if (n > 0) { x = 3; } return x; }");
  CHECK(r->valid());
  CHECK_EQ(opcodeCount(r->requireFunction("f"), ir::Opcode::Phi), std::size_t{0});
}

// ---------------------------------------------------------------------------
// DCE
// ---------------------------------------------------------------------------

TEST("an unused computation is removed") {
  auto r = runPass("dce", "fn f(int a) -> int { int unused = a * a * a; return a; }");
  CHECK(r->valid());
  CHECK_EQ(opcodeCount(r->requireFunction("f"), ir::Opcode::Mul), std::size_t{0});
}

TEST("a call is kept even when its result is unused") {
  // Nothing here proves a call is pure, so removing it could remove output.
  auto r = runPass("dce", "fn g(int a) -> int { return a; } "
                          "fn f(int a) -> int { g(a); return a; }");
  CHECK_EQ(opcodeCount(r->requireFunction("f"), ir::Opcode::Call), std::size_t{1});
}

TEST("a division is kept because it may trap") {
  auto r = runPass("dce", "fn f(int a, int b) -> int { int q = a / b; return a; }");
  CHECK_EQ(opcodeCount(r->requireFunction("f"), ir::Opcode::SDiv), std::size_t{1});
}

// ---------------------------------------------------------------------------
// GVN
// ---------------------------------------------------------------------------

TEST("a repeated computation is computed once") {
  auto r = runPass("gvn", "fn f(int a, int b) -> int { return (a + b) + (a + b); }");
  CHECK(r->valid());
  // Two adds remain: the shared subexpression, and the one combining it.
  CHECK_EQ(opcodeCount(r->requireFunction("f"), ir::Opcode::Add), std::size_t{2});
}

TEST("commutative operands are normalized before comparing") {
  auto r = runPass("gvn", "fn f(int a, int b) -> int { return (a + b) + (b + a); }");
  CHECK_EQ(opcodeCount(r->requireFunction("f"), ir::Opcode::Add), std::size_t{2});
}

TEST("subtraction is not treated as commutative") {
  auto r = runPass("gvn", "fn f(int a, int b) -> int { return (a - b) + (b - a); }");
  CHECK_EQ(opcodeCount(r->requireFunction("f"), ir::Opcode::Sub), std::size_t{2});
}

TEST("a computation in a sibling block is not reused") {
  // Neither arm dominates the other, so the value from one is not available in
  // the other.
  auto r = runPass("gvn",
                   "fn f(int n, int a, int b) -> int { int x = 0;"
                   " if (n > 0) { x = a + b; } else { x = a + b; } return x; }");
  CHECK(r->valid());
  CHECK_EQ(opcodeCount(r->requireFunction("f"), ir::Opcode::Add), std::size_t{2});
}

// ---------------------------------------------------------------------------
// Strength reduction
// ---------------------------------------------------------------------------

TEST("multiplication by a power of two becomes a shift") {
  auto r = runPass("strength-reduction", "fn f(int a) -> int { return a * 8; }");
  CHECK(r->valid());
  CHECK(r->has("shl i64 %a, 3"));
  CHECK_EQ(opcodeCount(r->requireFunction("f"), ir::Opcode::Mul), std::size_t{0});
}

TEST("multiplication by a non-power of two is left alone") {
  auto r = runPass("strength-reduction", "fn f(int a) -> int { return a * 7; }");
  CHECK_EQ(opcodeCount(r->requireFunction("f"), ir::Opcode::Mul), std::size_t{1});
}

TEST("division by a power of two is deliberately not reduced") {
  // An arithmetic shift rounds toward negative infinity while signed division
  // rounds toward zero, so the naive rewrite miscompiles every negative
  // dividend. Left alone rather than done nearly right.
  auto r = runPass("strength-reduction", "fn f(int a) -> int { return a / 8; }");
  CHECK_EQ(opcodeCount(r->requireFunction("f"), ir::Opcode::SDiv), std::size_t{1});
  CHECK_EQ(opcodeCount(r->requireFunction("f"), ir::Opcode::AShr), std::size_t{0});
}

// ---------------------------------------------------------------------------
// LICM
// ---------------------------------------------------------------------------

TEST("an invariant computation is hoisted into the preheader") {
  auto r = runPass("licm",
                   "fn f(int n, int a, int b) -> int { int t = 0;"
                   " while (n > 0) { t = t + (a * b); n = n - 1; } return t; }");
  CHECK(r->valid());

  // The multiply must no longer be inside the loop body.
  const ir::Function& fn = r->requireFunction("f");
  for (const auto& block : fn.blocks()) {
    if (block->label().rfind("while.body", 0) != 0) {
      continue;
    }
    for (const auto& instruction : block->instructions()) {
      CHECK(instruction->opcode() != ir::Opcode::Mul);
    }
  }
}

TEST("a computation that varies with the loop stays inside it") {
  auto r = runPass("licm",
                   "fn f(int n) -> int { int t = 0;"
                   " while (n > 0) { t = t + (n * 2); n = n - 1; } return t; }");
  CHECK(r->valid());
  CHECK_EQ(opcodeCount(r->requireFunction("f"), ir::Opcode::Mul), std::size_t{1});
}

TEST("a division is not hoisted, because hoisting a trap changes behaviour") {
  auto r = runPass("licm",
                   "fn f(int n, int a, int b) -> int { int t = 0;"
                   " while (n > 0) { t = t + (a / b); n = n - 1; } return t; }");
  CHECK(r->valid());
  const ir::Function& fn = r->requireFunction("f");
  bool insideLoop = false;
  for (const auto& block : fn.blocks()) {
    if (block->label().rfind("while.body", 0) != 0) {
      continue;
    }
    for (const auto& instruction : block->instructions()) {
      if (instruction->opcode() == ir::Opcode::SDiv) {
        insideLoop = true;
      }
    }
  }
  CHECK(insideLoop);
}

// ---------------------------------------------------------------------------
// SimplifyCFG
// ---------------------------------------------------------------------------

TEST("a constant branch is folded and its dead arm removed") {
  auto r = runPass("simplify-cfg",
                   "fn f() -> int { int t = 1; if (true) { t = 2; } return t; }");
  CHECK(r->valid());
  CHECK_EQ(opcodeCount(r->requireFunction("f"), ir::Opcode::CondBr), std::size_t{0});
}

// ---------------------------------------------------------------------------
// Inlining
// ---------------------------------------------------------------------------

TEST("a small single-block callee is inlined") {
  auto r = runPass("inline", "fn add(int a, int b) -> int { return a + b; } "
                             "fn f() -> int { return add(2, 3); }");
  CHECK(r->valid());
  CHECK_EQ(opcodeCount(r->requireFunction("f"), ir::Opcode::Call), std::size_t{0});
}

TEST("a recursive function is not inlined into itself") {
  auto r = runPass("inline",
                   "fn f(int n) -> int { if (n < 1) { return 0; } return f(n - 1); }");
  CHECK(r->valid());
  CHECK_EQ(opcodeCount(r->requireFunction("f"), ir::Opcode::Call), std::size_t{1});
}

TEST("a multi-block callee is left alone") {
  auto r = runPass("inline",
                   "fn g(int n) -> int { if (n > 0) { return 1; } return 2; } "
                   "fn f() -> int { return g(1); }");
  CHECK(r->valid());
  CHECK_EQ(opcodeCount(r->requireFunction("f"), ir::Opcode::Call), std::size_t{1});
}

TEST("a runtime builtin has no body and is not inlined") {
  auto r = runPass("inline", "fn f() { print_int(1); }");
  CHECK(r->valid());
  CHECK_EQ(opcodeCount(r->requireFunction("f"), ir::Opcode::Call), std::size_t{1});
}

TEST("a function nothing calls any more is removed") {
  auto r = runPass("inline", "fn add(int a, int b) -> int { return a + b; } "
                             "fn main() -> int { return add(2, 3); }");
  CHECK_EQ(transforms::removeUnusedFunctions(*r->module), std::size_t{1});
  CHECK(r->module->findFunction("add") == nullptr);
  CHECK(r->module->findFunction("main") != nullptr);
}

TEST("main is never removed even with no callers") {
  auto r = runPass("", "fn main() -> int { return 0; }");
  CHECK_EQ(transforms::removeUnusedFunctions(*r->module), std::size_t{0});
}

// ---------------------------------------------------------------------------
// The pipeline as a whole
// ---------------------------------------------------------------------------

TEST("the full pipeline leaves valid SSA for every shape") {
  const char* programs[] = {
      "fn f() -> int { return 1; }",
      "fn f(int n) -> int { if (n > 0) { return 1; } else { return 2; } }",
      "fn f(int n) -> int { int t = 0; while (n > 0) { t = t + n; n = n - 1; } return t; }",
      "fn f(int a, int b) -> bool { return a > 0 && b > 0; }",
      "fn g(int a) -> int { return a * 2; } fn f() -> int { return g(21); }",
      "fn f(int n, int a, int b) -> int { int t = 0; while (n > 0) { t = t + a * b; n = n - 1; } return t; }",
      "fn f(int n) -> int { int a = 1; int b = 2; while (n > 0) { int t = a; a = b; b = t; n = n - 1; } return a; }",
  };

  for (const char* source : programs) {
    auto r = runPass("", source);
    passes::PassManager pipeline;
    passes::buildPipeline(pipeline, 2, {});
    pipeline.run(*r->module, r->manager);

    if (!r->valid()) {
      ::optiforge::test::reportFailure(__FILE__, __LINE__,
                                       std::string("invalid IR after -O2: ") + source);
    }
    const std::vector<std::string> ssaErrors =
        analysis::verifySSA(*r->module, r->manager);
    if (!ssaErrors.empty()) {
      ::optiforge::test::reportFailure(
          __FILE__, __LINE__,
          std::string("invalid SSA after -O2: ") + source + "\n  " + ssaErrors.front());
    }
  }
}

TEST("the pipeline converges rather than oscillating") {
  auto r = runPass("", "fn f(int n) -> int { int t = 0;"
                       " while (n > 0) { t = t + (n * 4); n = n - 1; } return t; }");
  passes::PassManager pipeline;
  passes::buildPipeline(pipeline, 2, {});
  pipeline.run(*r->module, r->manager);
  // Two passes undoing each other's work would run to the cap.
  CHECK(pipeline.iterations() < passes::PassManager::kMaxIterations);
}

TEST("the pipeline reduces instruction count") {
  const char* source =
      "fn f(int n) -> int { int limit = 10 * 4; int t = 0;"
      " while (n > 0) { int s = limit * 2; t = t + s; int dead = n * n; n = n - 1; }"
      " return t; }";

  auto before = runPass("", source);
  const std::size_t sizeBefore = instructionCount(before->requireFunction("f"));

  auto after = runPass("", source);
  passes::PassManager pipeline;
  passes::buildPipeline(pipeline, 2, {});
  pipeline.run(*after->module, after->manager);
  const std::size_t sizeAfter = instructionCount(after->requireFunction("f"));

  CHECK(sizeAfter < sizeBefore);
}

TEST("every pass reports whether it changed anything") {
  // A pass that always claims a change would run the pipeline to its cap.
  auto r = runPass("", "fn f() -> int { return 1; }");
  passes::PassManager pipeline;
  passes::buildPipeline(pipeline, 2, {});
  pipeline.run(*r->module, r->manager);

  for (const passes::PassStatistics& statistic : pipeline.statistics()) {
    CHECK(statistic.changed <= statistic.runs);
  }
}

TEST("optimization is deterministic") {
  const char* source =
      "fn f(int n, int a, int b) -> int { int t = 0;"
      " while (n > 0) { t = t + a * b + n * 8; n = n - 1; } return t; }";
  const auto run = [&]() {
    auto r = runPass("", source);
    passes::PassManager pipeline;
    passes::buildPipeline(pipeline, 2, {});
    pipeline.run(*r->module, r->manager);
    return r->ir();
  };
  CHECK_EQ(run(), run());
}
