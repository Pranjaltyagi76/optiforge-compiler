#include <algorithm>
#include <cstdio>
#include <iostream>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "TestHarness.h"
#include "optiforge/analysis/AnalysisManager.h"
#include "optiforge/analysis/BitSet.h"
#include "optiforge/analysis/Dominators.h"
#include "optiforge/analysis/Liveness.h"
#include "optiforge/analysis/LoopInfo.h"
#include "optiforge/frontend/Lexer.h"
#include "optiforge/frontend/Parser.h"
#include "optiforge/frontend/Sema.h"
#include "optiforge/frontend/Symbol.h"
#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Module.h"
#include "optiforge/ir/Verifier.h"
#include "optiforge/irgen/IRGen.h"
#include "optiforge/support/Diagnostic.h"
#include "optiforge/support/SourceManager.h"
#include "optiforge/transforms/SSA.h"

using namespace optiforge;
using namespace optiforge::analysis;

namespace {

/// Compiles a fragment to IR and keeps everything the analyses need alive.
struct Compiled {
  SourceManager sm;
  std::ostringstream diagOut;
  SymbolTable symbols;
  std::unique_ptr<Program> ast;
  std::unique_ptr<ir::Module> module;
  AnalysisManager manager;

  const ir::Function* function(const std::string& name) const {
    return module == nullptr ? nullptr : module->findFunction(name);
  }

  /// Same, but a missing function aborts with the compiler's diagnostics
  /// instead of dereferencing null. Being [[noreturn]] also tells the
  /// optimizer the dereference below is unreachable, which -O3 otherwise
  /// flags -- and it turns a broken fixture into a readable message rather
  /// than a segfault.
  const ir::Function& requireFunction(const std::string& name) const {
    const ir::Function* found = function(name);
    if (found == nullptr) {
      std::cerr << "test fixture failed to compile function '" << name << "':\n"
                << diagOut.str();
      std::abort();
    }
    return *found;
  }

  /// Never returns null: a test asking for a block that does not exist has a
  /// broken fixture, and aborting says so instead of segfaulting later.
  const ir::BasicBlock* block(const std::string& fn, const std::string& label) const {
    for (const auto& b : requireFunction(fn).blocks()) {
      if (b->label() == label) {
        return b.get();
      }
    }
    std::cerr << "test fixture has no block '" << label << "' in function '" << fn
              << "'\n";
    std::abort();
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

std::vector<std::string> labelsOf(const std::vector<const ir::BasicBlock*>& blocks) {
  std::vector<std::string> out;
  for (const ir::BasicBlock* block : blocks) {
    out.push_back(block == nullptr ? "<exit>" : block->label());
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// BitSet
// ---------------------------------------------------------------------------

TEST("BitSet set operations behave as sets") {
  BitSet a(8), b(8);
  a.set(1); a.set(3); a.set(5);
  b.set(3); b.set(5); b.set(7);

  BitSet u = a; u.unionWith(b);
  CHECK_EQ(u.count(), std::size_t{4});

  BitSet i = a; i.intersectWith(b);
  CHECK_EQ(i.count(), std::size_t{2});
  CHECK(i.test(3) && i.test(5));

  BitSet d = a; d.subtract(b);
  CHECK_EQ(d.count(), std::size_t{1});
  CHECK(d.test(1));
}

TEST("BitSet elements come out ascending, which keeps dumps deterministic") {
  BitSet a(8);
  a.set(5); a.set(0); a.set(3);
  const std::vector<std::size_t> expected{0, 3, 5};
  CHECK(a.elements() == expected);
}

TEST("out-of-range BitSet access is ignored rather than undefined") {
  BitSet a(4);
  a.set(99);
  CHECK(!a.test(99));
  CHECK(a.empty());
}

// ---------------------------------------------------------------------------
// AnalysisManager (ADR-03)
// ---------------------------------------------------------------------------

TEST("an analysis is computed once and then served from cache") {
  auto c = compile("fn f(int n) -> int { while (n > 0) { n = n - 1; } return n; }");
  const ir::Function& fn = c->requireFunction("f");

  CHECK_EQ(c->manager.computationCount(), 0u);
  c->manager.get<DominatorTreeAnalysis>(fn);
  CHECK_EQ(c->manager.computationCount(), 1u);

  c->manager.get<DominatorTreeAnalysis>(fn);
  c->manager.get<DominatorTreeAnalysis>(fn);
  CHECK_EQ(c->manager.computationCount(), 1u);
  CHECK_EQ(c->manager.cacheHitCount(), 2u);
}

TEST("different analyses of the same function are cached separately") {
  auto c = compile("fn f(int n) -> int { while (n > 0) { n = n - 1; } return n; }");
  const ir::Function& fn = c->requireFunction("f");
  c->manager.get<DominatorTreeAnalysis>(fn);
  c->manager.get<LivenessAnalysis>(fn);
  CHECK_EQ(c->manager.computationCount(), 2u);
}

TEST("an analysis may depend on another, which is then shared") {
  auto c = compile("fn f(int n) -> int { while (n > 0) { n = n - 1; } return n; }");
  const ir::Function& fn = c->requireFunction("f");
  // LoopAnalysis needs dominators; requesting it computes both.
  c->manager.get<LoopAnalysis>(fn);
  CHECK_EQ(c->manager.computationCount(), 2u);
  // Asking for dominators directly must now be a hit, not a recomputation.
  c->manager.get<DominatorTreeAnalysis>(fn);
  CHECK_EQ(c->manager.computationCount(), 2u);
}

TEST("getCached does not compute") {
  auto c = compile("fn f() -> int { return 1; }");
  const ir::Function& fn = c->requireFunction("f");
  CHECK_EQ(c->manager.getCached<DominatorTreeAnalysis>(fn), nullptr);
  CHECK_EQ(c->manager.computationCount(), 0u);
  c->manager.get<DominatorTreeAnalysis>(fn);
  CHECK(c->manager.getCached<DominatorTreeAnalysis>(fn) != nullptr);
}

TEST("invalidating one function drops its results") {
  auto c = compile("fn a() -> int { return 1; } fn b() -> int { return 2; }");
  const ir::Function& fa = c->requireFunction("a");
  const ir::Function& fb = c->requireFunction("b");
  c->manager.get<DominatorTreeAnalysis>(fa);
  c->manager.get<DominatorTreeAnalysis>(fb);

  c->manager.invalidate(fa);
  CHECK_EQ(c->manager.getCached<DominatorTreeAnalysis>(fa), nullptr);
  CHECK(c->manager.getCached<DominatorTreeAnalysis>(fb) != nullptr);
}

// ---------------------------------------------------------------------------
// Dominators
// ---------------------------------------------------------------------------

TEST("a straight-line function has a single-chain dominator tree") {
  auto c = compile("fn f() -> int { return 1; }");
  const ir::Function& fn = c->requireFunction("f");
  const DominatorTree& tree = c->manager.get<DominatorTreeAnalysis>(fn);
  CHECK_EQ(tree.immediateDominator(fn.entry()), nullptr);
  CHECK(tree.dominates(fn.entry(), fn.entry()));
}

TEST("an if-diamond makes the condition dominate all three arms") {
  auto c = compile(
      "fn f(int n) -> int { int x = 0; if (n > 0) { x = 1; } else { x = 2; } return x; }");
  const ir::Function& fn = c->requireFunction("f");
  const DominatorTree& tree = c->manager.get<DominatorTreeAnalysis>(fn);

  const ir::BasicBlock* entry = fn.entry();
  const ir::BasicBlock* then_ = c->block("f", "if.then.1");
  const ir::BasicBlock* else_ = c->block("f", "if.else.2");
  const ir::BasicBlock* end = c->block("f", "if.end.3");

  CHECK_EQ(tree.immediateDominator(then_), entry);
  CHECK_EQ(tree.immediateDominator(else_), entry);
  // The merge block is dominated by the branch, not by either arm.
  CHECK_EQ(tree.immediateDominator(end), entry);
  CHECK(!tree.dominates(then_, end));
  CHECK(!tree.dominates(else_, end));
}

TEST("dominance is reflexive and strict dominance is not") {
  auto c = compile("fn f(int n) -> int { if (n > 0) { return 1; } return 0; }");
  const ir::Function& fn = c->requireFunction("f");
  const DominatorTree& tree = c->manager.get<DominatorTreeAnalysis>(fn);
  CHECK(tree.dominates(fn.entry(), fn.entry()));
  CHECK(!tree.strictlyDominates(fn.entry(), fn.entry()));
}

TEST("a loop header dominates its body and its exit") {
  auto c = compile("fn f(int n) -> int { while (n > 0) { n = n - 1; } return n; }");
  const ir::Function& fn = c->requireFunction("f");
  const DominatorTree& tree = c->manager.get<DominatorTreeAnalysis>(fn);

  const ir::BasicBlock* cond = c->block("f", "while.cond.1");
  const ir::BasicBlock* body = c->block("f", "while.body.2");
  const ir::BasicBlock* end = c->block("f", "while.end.3");

  CHECK_EQ(tree.immediateDominator(cond), fn.entry());
  CHECK_EQ(tree.immediateDominator(body), cond);
  CHECK_EQ(tree.immediateDominator(end), cond);
  CHECK(tree.dominates(cond, body));
  CHECK(!tree.dominates(body, cond));  // the back edge is not dominance
}

TEST("post-dominators are rooted at a virtual exit") {
  auto c = compile("fn f(int n) -> int { if (n > 0) { return 1; } return 0; }");
  const ir::Function& fn = c->requireFunction("f");
  const DominatorTree& tree = c->manager.get<PostDominatorTreeAnalysis>(fn);
  CHECK(tree.isPostDominatorTree());
  // With two returns there is no real block that post-dominates both, which is
  // exactly why the virtual exit exists.
  CHECK_EQ(tree.immediateDominator(fn.entry()), nullptr);
}

TEST("the block after a loop post-dominates the loop") {
  auto c = compile("fn f(int n) -> int { while (n > 0) { n = n - 1; } return n; }");
  const ir::Function& fn = c->requireFunction("f");
  const DominatorTree& tree = c->manager.get<PostDominatorTreeAnalysis>(fn);
  const ir::BasicBlock* cond = c->block("f", "while.cond.1");
  const ir::BasicBlock* end = c->block("f", "while.end.3");
  CHECK(tree.dominates(end, cond));
}

// ---------------------------------------------------------------------------
// Dominance frontiers
// ---------------------------------------------------------------------------

TEST("the arms of an if have the merge block as their frontier") {
  auto c = compile(
      "fn f(int n) -> int { int x = 0; if (n > 0) { x = 1; } else { x = 2; } return x; }");
  const ir::Function& fn = c->requireFunction("f");
  const DominanceFrontier& df = c->manager.get<DominanceFrontierAnalysis>(fn);

  const std::vector<std::string> expected{"if.end.3"};
  CHECK(labelsOf(df.frontierOf(c->block("f", "if.then.1"))) == expected);
  CHECK(labelsOf(df.frontierOf(c->block("f", "if.else.2"))) == expected);
  // The entry dominates everything, so its dominance never stops.
  CHECK(df.frontierOf(fn.entry()).empty());
}

TEST("a loop header is its own dominance frontier") {
  // The back edge re-enters the header from a block the header dominates,
  // which is what puts a phi there in Phase 6.
  auto c = compile("fn f(int n) -> int { while (n > 0) { n = n - 1; } return n; }");
  const ir::Function& fn = c->requireFunction("f");
  const DominanceFrontier& df = c->manager.get<DominanceFrontierAnalysis>(fn);
  const std::vector<std::string> expected{"while.cond.1"};
  CHECK(labelsOf(df.frontierOf(c->block("f", "while.cond.1"))) == expected);
  CHECK(labelsOf(df.frontierOf(c->block("f", "while.body.2"))) == expected);
}

// ---------------------------------------------------------------------------
// Loops
// ---------------------------------------------------------------------------

TEST("a function with no loop reports none") {
  auto c = compile("fn f(int n) -> int { if (n > 0) { return 1; } return 0; }");
  CHECK(c->manager.get<LoopAnalysis>(c->requireFunction("f")).empty());
}

TEST("a while loop is detected with header, latch, body and exit") {
  auto c = compile("fn f(int n) -> int { while (n > 0) { n = n - 1; } return n; }");
  const LoopInfo& loops = c->manager.get<LoopAnalysis>(c->requireFunction("f"));

  CHECK_EQ(loops.allLoops().size(), std::size_t{1});
  const Loop& loop = *loops.topLevelLoops()[0];
  CHECK_EQ(loop.header()->label(), std::string("while.cond.1"));
  CHECK(labelsOf(loop.latches()) == std::vector<std::string>{"while.body.2"});
  CHECK(labelsOf(loop.blocks()) ==
        (std::vector<std::string>{"while.cond.1", "while.body.2"}));
  CHECK(labelsOf(loop.exits()) == std::vector<std::string>{"while.end.3"});
  CHECK_EQ(loop.depth(), 1u);
}

TEST("IRGen already produces a usable preheader") {
  // The entry block branches only to the loop header, which is exactly the
  // property LICM needs to have somewhere unambiguous to hoist to.
  auto c = compile("fn f(int n) -> int { while (n > 0) { n = n - 1; } return n; }");
  const LoopInfo& loops = c->manager.get<LoopAnalysis>(c->requireFunction("f"));
  const Loop& loop = *loops.topLevelLoops()[0];
  CHECK(loop.preheader() != nullptr);
  CHECK_EQ(loop.preheader()->label(), std::string("entry"));
}

TEST("nested loops are detected with correct depth and nesting") {
  auto c = compile(
      "fn f(int n, int m) -> int {\n"
      "  int t = 0;\n"
      "  while (n > 0) { int j = m; while (j > 0) { t = t + 1; j = j - 1; } n = n - 1; }\n"
      "  return t;\n"
      "}\n");
  const LoopInfo& loops = c->manager.get<LoopAnalysis>(c->requireFunction("f"));

  CHECK_EQ(loops.allLoops().size(), std::size_t{2});
  CHECK_EQ(loops.topLevelLoops().size(), std::size_t{1});

  const Loop& outer = *loops.topLevelLoops()[0];
  CHECK_EQ(outer.depth(), 1u);
  CHECK_EQ(outer.subLoops().size(), std::size_t{1});

  const Loop& inner = *outer.subLoops()[0];
  CHECK_EQ(inner.depth(), 2u);
  CHECK_EQ(inner.parent(), &outer);
  // The outer loop contains every block of the inner one.
  for (const ir::BasicBlock* block : inner.blocks()) {
    CHECK(outer.contains(block));
  }
}

TEST("loopFor returns the innermost containing loop") {
  auto c = compile(
      "fn f(int n, int m) -> int {\n"
      "  int t = 0;\n"
      "  while (n > 0) { int j = m; while (j > 0) { t = t + 1; j = j - 1; } n = n - 1; }\n"
      "  return t;\n"
      "}\n");
  const ir::Function& fn = c->requireFunction("f");
  const LoopInfo& loops = c->manager.get<LoopAnalysis>(fn);
  const Loop& inner = *loops.topLevelLoops()[0]->subLoops()[0];

  CHECK_EQ(loops.loopFor(inner.header()), &inner);
  CHECK_EQ(loops.depthOf(inner.header()), 2u);
  CHECK_EQ(loops.loopFor(fn.entry()), nullptr);
  CHECK_EQ(loops.depthOf(fn.entry()), 0u);
}

TEST("two sequential loops are separate top-level loops") {
  auto c = compile(
      "fn f(int n) -> int {\n"
      "  while (n > 0) { n = n - 1; }\n"
      "  while (n < 10) { n = n + 1; }\n"
      "  return n;\n"
      "}\n");
  const LoopInfo& loops = c->manager.get<LoopAnalysis>(c->requireFunction("f"));
  CHECK_EQ(loops.allLoops().size(), std::size_t{2});
  CHECK_EQ(loops.topLevelLoops().size(), std::size_t{2});
  CHECK_EQ(loops.topLevelLoops()[0]->depth(), 1u);
  CHECK_EQ(loops.topLevelLoops()[1]->depth(), 1u);
}

TEST("a loop containing an if keeps both arms inside the loop") {
  auto c = compile(
      "fn f(int n) -> int {\n"
      "  int t = 0;\n"
      "  while (n > 0) { if (n > 5) { t = t + 2; } else { t = t + 1; } n = n - 1; }\n"
      "  return t;\n"
      "}\n");
  const LoopInfo& loops = c->manager.get<LoopAnalysis>(c->requireFunction("f"));
  CHECK_EQ(loops.allLoops().size(), std::size_t{1});
  const Loop& loop = *loops.topLevelLoops()[0];
  CHECK(loop.contains(c->block("f", "if.then.4")));
  CHECK(loop.contains(c->block("f", "if.else.5")));
  CHECK(!loop.contains(c->block("f", "while.end.3")));
}

// ---------------------------------------------------------------------------
// Preheader insertion (a transform)
// ---------------------------------------------------------------------------

TEST("preheader insertion is a no-op when every loop already has one") {
  auto c = compile("fn f(int n) -> int { while (n > 0) { n = n - 1; } return n; }");
  auto* fn = const_cast<ir::Function*>(c->function("f"));
  CHECK_EQ(insertLoopPreheaders(*fn), std::size_t{0});

  ir::Verifier verifier;
  CHECK(verifier.verify(*fn));
}

TEST("inserting a preheader keeps the IR valid") {
  auto c = compile(
      "fn f(int n) -> int {\n"
      "  int t = 0;\n"
      "  while (n > 0) { if (n > 5) { t = t + 2; } else { t = t + 1; } n = n - 1; }\n"
      "  return t;\n"
      "}\n");
  auto* fn = const_cast<ir::Function*>(c->function("f"));
  insertLoopPreheaders(*fn);

  ir::Verifier verifier;
  if (!verifier.verify(*fn)) {
    std::ostringstream out;
    verifier.printErrors(out);
    ::optiforge::test::reportFailure(__FILE__, __LINE__, "IR invalid after insertion:\n" + out.str());
  }

  // Every loop must now have a preheader.
  const DominatorTree tree(*fn, false);
  const LoopInfo loops(*fn, tree);
  for (const auto& loop : loops.allLoops()) {
    CHECK(loop->preheader() != nullptr);
  }
}

// ---------------------------------------------------------------------------
// Liveness
// ---------------------------------------------------------------------------

TEST("a value used in the same block it is defined is not live across blocks") {
  auto c = compile("fn f(int a, int b) -> int { return a + b; }");
  const ir::Function& fn = c->requireFunction("f");
  const Liveness& live = c->manager.get<LivenessAnalysis>(fn);
  // Everything is computed and consumed inside entry.
  CHECK(live.liveOut(fn.entry()).empty());
}

TEST("a value used after a loop is live across it") {
  auto c = compile("fn f(int n) -> int { int t = 5; while (n > 0) { n = n - 1; } return t; }");
  const ir::Function& fn = c->requireFunction("f");
  const Liveness& live = c->manager.get<LivenessAnalysis>(fn);

  // The slot holding t must stay live through the loop, or a register
  // allocator would happily reuse it and corrupt the value.
  const ir::BasicBlock* cond = c->block("f", "while.cond.1");
  bool foundSlot = false;
  for (const ir::Value* value : live.liveIn(cond)) {
    if (value->hasName() && value->name() == "t.addr") {
      foundSlot = true;
    }
  }
  CHECK(foundSlot);
}

TEST("nothing is live out of a return block") {
  auto c = compile("fn f(int n) -> int { while (n > 0) { n = n - 1; } return n; }");
  const Liveness& live = c->manager.get<LivenessAnalysis>(c->requireFunction("f"));
  CHECK(live.liveOut(c->block("f", "while.end.3")).empty());
}

TEST("an argument is live into the entry block") {
  auto c = compile("fn f(int a) -> int { return a; }");
  const ir::Function& fn = c->requireFunction("f");
  const Liveness& live = c->manager.get<LivenessAnalysis>(fn);
  CHECK(live.isLiveIn(fn.entry(), fn.arguments()[0].get()));
}

TEST("liveness converges rather than iterating forever") {
  auto c = compile(
      "fn f(int n, int m) -> int {\n"
      "  int t = 0;\n"
      "  while (n > 0) { int j = m; while (j > 0) { t = t + 1; j = j - 1; } n = n - 1; }\n"
      "  return t;\n"
      "}\n");
  const Liveness& live = c->manager.get<LivenessAnalysis>(c->requireFunction("f"));
  CHECK(live.iterations() > 0u);
  CHECK(live.iterations() < 1000u);
}

// ---------------------------------------------------------------------------
// Reaching definitions
// ---------------------------------------------------------------------------

TEST("a store in a block reaches its own exit") {
  auto c = compile("fn f() -> int { int x = 1; return x; }");
  const ir::Function& fn = c->requireFunction("f");
  const ReachingDefinitions& reaching =
      c->manager.get<ReachingDefinitionsAnalysis>(fn);
  CHECK_EQ(reaching.reachingOut(fn.entry()).size(), std::size_t{1});
  CHECK(reaching.reachingIn(fn.entry()).empty());
}

TEST("a later store to the same slot kills the earlier one") {
  auto c = compile("fn f() -> int { int x = 1; x = 2; return x; }");
  const ir::Function& fn = c->requireFunction("f");
  const ReachingDefinitions& reaching =
      c->manager.get<ReachingDefinitionsAnalysis>(fn);
  CHECK_EQ(reaching.definitions().size(), std::size_t{2});
  // Only the second survives to the end of the block.
  CHECK_EQ(reaching.reachingOut(fn.entry()).size(), std::size_t{1});
}

TEST("stores to different slots coexist") {
  auto c = compile("fn f() -> int { int x = 1; int y = 2; return x + y; }");
  const ir::Function& fn = c->requireFunction("f");
  const ReachingDefinitions& reaching =
      c->manager.get<ReachingDefinitionsAnalysis>(fn);
  CHECK_EQ(reaching.reachingOut(fn.entry()).size(), std::size_t{2});
}

TEST("both the initial and the loop store reach the loop header") {
  auto c = compile("fn f(int n) -> int { int t = 0; while (n > 0) { t = t + 1; n = n - 1; } return t; }");
  const ReachingDefinitions& reaching =
      c->manager.get<ReachingDefinitionsAnalysis>(c->requireFunction("f"));
  // At the header, t may hold either the initial value or the loop's update --
  // which is precisely why a phi belongs there.
  std::size_t forT = 0;
  for (const ir::Instruction* store : reaching.reachingIn(c->block("f", "while.cond.1"))) {
    const ir::Value* slot = store->operand(1);
    if (slot->hasName() && slot->name() == "t.addr") {
      ++forT;
    }
  }
  CHECK_EQ(forT, std::size_t{2});
}

// ---------------------------------------------------------------------------
// Use-def
// ---------------------------------------------------------------------------

TEST("use-def links a value to the instructions reading it") {
  auto c = compile("fn f(int a) -> int { return a + a; }");
  const ir::Function& fn = c->requireFunction("f");
  const UseDefInfo& useDef = c->manager.get<UseDefAnalysis>(fn);

  // The load of a is read once, by the add, even though it appears twice as
  // an operand.
  const ir::Instruction* add = nullptr;
  for (const auto& instruction : c->block("f", "entry")->instructions()) {
    if (instruction->opcode() == ir::Opcode::Add) {
      add = instruction.get();
    }
  }
  CHECK(add != nullptr);
  CHECK_EQ(useDef.operandsOf(add).size(), std::size_t{2});
  CHECK_EQ(useDef.usersOf(add->operand(0)).size(), std::size_t{1});
}

TEST("a value with a result is its own definition") {
  auto c = compile("fn f(int a) -> int { return a + 1; }");
  const ir::Function& fn = c->requireFunction("f");
  const UseDefInfo& useDef = c->manager.get<UseDefAnalysis>(fn);
  for (const auto& instruction : c->block("f", "entry")->instructions()) {
    if (instruction->hasResult()) {
      CHECK_EQ(useDef.definitionOf(instruction.get()), instruction.get());
    }
  }
  // Arguments and constants have no defining instruction.
  CHECK_EQ(useDef.definitionOf(fn.arguments()[0].get()), nullptr);
}

// ---------------------------------------------------------------------------
// Robustness
// ---------------------------------------------------------------------------

TEST("analyses handle a single-block function") {
  auto c = compile("fn f() -> int { return 1; }");
  const ir::Function& fn = c->requireFunction("f");
  c->manager.get<DominatorTreeAnalysis>(fn);
  c->manager.get<PostDominatorTreeAnalysis>(fn);
  c->manager.get<DominanceFrontierAnalysis>(fn);
  c->manager.get<LoopAnalysis>(fn);
  c->manager.get<LivenessAnalysis>(fn);
  c->manager.get<ReachingDefinitionsAnalysis>(fn);
  CHECK_EQ(c->manager.computationCount(), 6u);
}

TEST("analyses handle deeply nested loops without blowing up") {
  std::string source = "fn f(int n) -> int {\n  int t = 0;\n";
  for (int i = 0; i < 8; ++i) {
    source += "  while (n > " + std::to_string(i) + ") { n = n - 1;\n";
  }
  source += "  t = t + 1;\n";
  for (int i = 0; i < 8; ++i) {
    source += "  }\n";
  }
  source += "  return t;\n}\n";

  auto c = compile(source);
  const LoopInfo& loops = c->manager.get<LoopAnalysis>(c->requireFunction("f"));
  CHECK_EQ(loops.allLoops().size(), std::size_t{8});
  CHECK_EQ(loops.topLevelLoops().size(), std::size_t{1});

  // Depths must run 1..8 with no gaps.
  std::vector<unsigned> depths;
  for (const auto& loop : loops.allLoops()) {
    depths.push_back(loop->depth());
  }
  std::sort(depths.begin(), depths.end());
  for (unsigned i = 0; i < 8; ++i) {
    CHECK_EQ(depths[i], i + 1);
  }
}

TEST("a phi operand is live out of the edge it arrives on, not into its own block") {
  // The classic register-allocator trap. A phi's operand is live at the end of
  // the predecessor it comes from -- not at the top of the block holding the
  // phi, and not at the top of the block that computes it. Adding phi operands
  // after the backward walk, as this once did, made a value live across the
  // whole block that defines it and produced interferences that are not real.
  auto c = compile("fn f(int n) -> int { int t = 0; if (n > 0) { t = n + 1; } return t; }");
  auto& fn = const_cast<ir::Function&>(c->requireFunction("f"));
  transforms::promoteMemoryToRegisters(fn, c->manager);
  c->manager.invalidateAll();

  // The add feeding the phi is the only instruction in the then-block.
  const ir::BasicBlock* then = c->block("f", "if.then.1");
  const ir::Value* add = nullptr;
  for (const auto& instruction : then->instructions()) {
    if (instruction->opcode() == ir::Opcode::Add) {
      add = instruction.get();
    }
  }
  CHECK(add != nullptr);

  const Liveness& live = c->manager.get<LivenessAnalysis>(fn);
  CHECK(live.isLiveOut(then, add));    // the phi reads it on this edge
  CHECK(!live.isLiveIn(then, add));    // but it is defined here, not before
  CHECK(!live.isLiveIn(c->block("f", "if.end.2"), add));  // the phi is not a use here
  CHECK(!live.isLiveOut(fn.entry(), add));
}

TEST("inserting a preheader merges the header's phi operands") {
  // Several edges into the header become one, so the values they carried have
  // to be merged a block earlier. Redirecting the edges and leaving the phis
  // naming the old predecessors leaves IR no verifier accepts.
  auto c = compile(
      "fn f(int n) -> int { int t = 0;"
      " while (n > 0) { t = t + 1; n = n - 1; } return t; }");
  auto& fn = const_cast<ir::Function&>(c->requireFunction("f"));
  transforms::promoteMemoryToRegisters(fn, c->manager);
  c->manager.invalidateAll();

  // Give the header a second entry edge so it no longer has a preheader: route
  // the entry block through a diamond that rejoins at the loop header itself.
  ir::BasicBlock* header = const_cast<ir::BasicBlock*>(c->block("f", "while.cond.1"));
  ir::BasicBlock* bypass = fn.createBlock("bypass");
  {
    auto branch = std::make_unique<ir::Instruction>(ir::Opcode::Br, ir::Type::getVoid());
    branch->addSuccessor(header);
    bypass->append(std::move(branch));
  }
  ir::Instruction* entryTerm = fn.entry()->terminator();
  auto condbr = std::make_unique<ir::Instruction>(ir::Opcode::CondBr, ir::Type::getVoid());
  condbr->addOperand(c->module->getBool(true));
  condbr->addSuccessor(header);
  condbr->addSuccessor(bypass);
  entryTerm->eraseFromParent();
  fn.entry()->append(std::move(condbr));

  // Both new edges need an entry in every header phi.
  for (const auto& instruction : header->instructions()) {
    if (instruction->opcode() != ir::Opcode::Phi) {
      break;
    }
    ir::Value* fromEntry = nullptr;
    for (std::size_t i = 0; i < instruction->incomingCount(); ++i) {
      if (instruction->incomingBlock(i) == fn.entry()) {
        fromEntry = instruction->operand(i);
      }
    }
    instruction->addIncoming(fromEntry, bypass);
  }
  fn.recomputePredecessors();

  ir::Verifier before;
  if (!before.verify(fn)) {
    std::ostringstream out;
    before.printErrors(out);
    ::optiforge::test::reportFailure(__FILE__, __LINE__,
                                     "fixture is not valid IR: " + out.str());
  }

  CHECK_EQ(insertLoopPreheaders(fn), std::size_t{1});

  ir::Verifier after;
  if (!after.verify(fn)) {
    std::ostringstream out;
    after.printErrors(out);
    ::optiforge::test::reportFailure(__FILE__, __LINE__,
                                     "IR invalid after preheader insertion: " + out.str());
  }

  const DominatorTree tree(fn, false);
  const LoopInfo loops(fn, tree);
  for (const auto& loop : loops.allLoops()) {
    CHECK(loop->preheader() != nullptr);
  }
}
