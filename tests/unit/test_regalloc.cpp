#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "TestHarness.h"
#include "optiforge/analysis/AnalysisManager.h"
#include "optiforge/backend/CodeGen.h"
#include "optiforge/backend/RegAlloc.h"
#include "optiforge/backend/TargetInfo.h"
#include "optiforge/frontend/Lexer.h"
#include "optiforge/frontend/Parser.h"
#include "optiforge/frontend/Sema.h"
#include "optiforge/frontend/Symbol.h"
#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Instruction.h"
#include "optiforge/ir/Module.h"
#include "optiforge/irgen/IRGen.h"
#include "optiforge/passes/Pass.h"
#include "optiforge/support/Diagnostic.h"
#include "optiforge/support/SourceManager.h"
#include "optiforge/transforms/SSA.h"

using namespace optiforge;
using namespace optiforge::backend;

namespace {

/// Compiles a fragment through the whole pipeline the backend actually sees:
/// SSA construction, optimization, SSA destruction. Register allocation is only
/// meaningful on the IR that reaches it.
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
      std::cerr << "test fixture failed for '" << name << "':\n" << diagOut.str();
      std::abort();
    }
    return *found;
  }

  RegisterAssignment allocate(const std::string& name) {
    return allocateRegisters(requireFunction(name), manager, x86_64WindowsTarget());
  }

  std::vector<std::string> verify(const std::string& name,
                                  const RegisterAssignment& assignment) {
    return verifyAssignment(requireFunction(name), manager, x86_64WindowsTarget(),
                            assignment);
  }
};

std::unique_ptr<Compiled> compile(std::string source, int optLevel = 2) {
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

  if (optLevel > 0) {
    transforms::promoteMemoryToRegisters(*result->module, result->manager);
    passes::PassManager pipeline;
    passes::buildPipeline(pipeline, optLevel, {});
    pipeline.run(*result->module, result->manager);
  }
  transforms::destroySSA(*result->module);
  result->manager.invalidateAll();
  return result;
}

/// Assembly for one fragment, through the named allocator.
std::string assemblyFor(std::string source, RegAllocKind allocator, int optLevel = 2) {
  auto compiled = compile(std::move(source), optLevel);
  if (compiled->module == nullptr) {
    return "<compile error>\n" + compiled->diagOut.str();
  }
  CodeGen codegen(x86_64WindowsTarget(), allocator);
  const MModule machine = codegen.run(*compiled->module, compiled->manager);
  std::ostringstream out;
  printAssembly(machine, out);
  return out.str();
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

/// Instructions that touch the frame. The Phase-8 claim is that there are
/// fewer of these than Phase 4 emitted, so counting them is the measurement.
std::size_t frameAccesses(const std::string& assembly) {
  std::size_t count = 0;
  std::istringstream in(assembly);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] != '\t' || contains(line, ".") == false) {
      // fall through; the real filter is below
    }
    if (contains(line, "(%rbp)") && !contains(line, "leaq") &&
        !contains(line, "pushq") && !contains(line, "popq")) {
      ++count;
    }
  }
  return count;
}

const ir::Value* valueNamed(const ir::Function& function, const std::string& name) {
  for (const auto& argument : function.arguments()) {
    if (argument->name() == name) {
      return argument.get();
    }
  }
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block->instructions()) {
      if (instruction->name() == name) {
        return instruction.get();
      }
    }
  }
  return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// The allocation itself
// ---------------------------------------------------------------------------

TEST("a loop-carried value gets a register and never touches the frame") {
  // Milestone M5: the assembly keeps values in registers across a loop. Phase 4
  // reloaded the accumulator and the counter on every iteration.
  const char* source =
      "fn sum(int n) -> int { int total = 0; int i = 0;"
      " while (i < n) { total = total + i; i = i + 1; } return total; }";

  const std::string graph = assemblyFor(source, RegAllocKind::Graph);
  const std::string naive = assemblyFor(source, RegAllocKind::Naive);

  CHECK(frameAccesses(naive) > 0);
  CHECK_EQ(frameAccesses(graph), std::size_t{0});
}

TEST("every value in a small function is coloured") {
  auto c = compile("fn f(int a, int b) -> int { return a * b + a; }");
  const RegisterAssignment assignment = c->allocate("f");
  CHECK(assignment.candidates > 0);
  CHECK_EQ(assignment.spilled, std::size_t{0});
  CHECK_EQ(assignment.colored, assignment.candidates);
  CHECK(c->verify("f", assignment).empty());
}

TEST("two arguments live at once never share a register") {
  // An argument has no defining instruction, so the def-point rule that builds
  // interference never sees it. Two whole-function arguments had no edge
  // between them at all and were given the same register.
  auto c = compile("fn f(int a, int b) -> int { int t = 0;"
                   " while (t < a) { t = t + b; } return t + a + b; }");
  const RegisterAssignment assignment = c->allocate("f");

  const ir::Function& fn = c->requireFunction("f");
  MReg first = MReg::None;
  MReg second = MReg::None;
  const bool haveA = assignment.registerFor(valueNamed(fn, "a"), first);
  const bool haveB = assignment.registerFor(valueNamed(fn, "b"), second);
  CHECK(haveA);
  CHECK(haveB);
  CHECK(first != second);
  CHECK(c->verify("f", assignment).empty());
}

TEST("a value live across a call lands in a callee-saved register") {
  auto c = compile("fn callee(int x) -> int { return x + x + x + x + x + x + x + x; }"
                   "fn f(int n) -> int { int keep = n + 1; return callee(n) + keep; }",
                   /*optLevel=*/1);
  const RegisterAssignment assignment = c->allocate("f");
  const ir::Function& fn = c->requireFunction("f");

  const ir::Value* keep = valueNamed(fn, "t2");
  MReg reg = MReg::None;
  if (keep != nullptr && assignment.registerFor(keep, reg)) {
    CHECK(x86_64WindowsTarget().isCalleeSaved(reg));
  }
  // Whatever it decided, the rule itself has to hold everywhere.
  CHECK(c->verify("f", assignment).empty());
}

TEST("a function that uses no callee-saved register saves none") {
  auto c = compile("fn f(int a) -> int { return a + 1; }");
  const RegisterAssignment assignment = c->allocate("f");
  for (MReg reg : assignment.usedCalleeSaved) {
    CHECK(x86_64WindowsTarget().isCalleeSaved(reg));
  }
  const std::string assembly = assemblyFor("fn f(int a) -> int { return a + 1; }",
                                           RegAllocKind::Graph);
  CHECK(!contains(assembly, "callee-saved"));
}

TEST("more live values than registers spills rather than miscolouring") {
  const char* source =
      "fn f(int n) -> int {"
      " int a=n+1; int b=n+2; int c=n+3; int d=n+4; int e=n+5; int g=n+6;"
      " int h=n+7; int i=n+8; int j=n+9; int k=n+10; int l=n+11; int m=n+12;"
      " int t=0; int z=0;"
      " while (z < 3) { t = t+a+b+c+d+e+g+h+i+j+k+l+m; z = z+1; }"
      " return t; }";
  auto c = compile(source);
  const RegisterAssignment assignment = c->allocate("f");

  CHECK(assignment.spilled > 0);   // there are not enough registers to go round
  CHECK(assignment.colored > 0);   // but it did not give up either
  CHECK(assignment.maxPressure > x86_64WindowsTarget().allocatableIntRegisters().size());
  CHECK(c->verify("f", assignment).empty());
}

TEST("phi copies are coalesced onto one register") {
  // SSA destruction turns each phi into copies that share a location. They must
  // share a register too, or the phi's meaning is lost -- and the copies then
  // become self-moves the code generator drops entirely.
  auto c = compile("fn f(int n) -> int { int t = 0;"
                   " while (n > 0) { t = t + n; n = n - 1; } return t; }");
  const RegisterAssignment assignment = c->allocate("f");
  const ir::Function& fn = c->requireFunction("f");

  for (const auto& block : fn.blocks()) {
    for (const auto& instruction : block->instructions()) {
      const ir::Instruction* root = instruction->slotAlias();
      if (root == nullptr || root == instruction.get()) {
        continue;
      }
      MReg member = MReg::None;
      MReg rootReg = MReg::None;
      const bool haveMember = assignment.registerFor(instruction.get(), member);
      const bool haveRoot = assignment.registerFor(root, rootReg);
      CHECK_EQ(haveMember, haveRoot);
      if (haveMember) {
        CHECK(member == rootReg);
      }
    }
  }
  CHECK(c->verify("f", assignment).empty());
}

TEST("freeze gives up on a copy rather than spilling the node holding it") {
  // A move-related node is held back from simplify so its copy still has a
  // chance to be coalesced. When nothing simplifies and nothing coalesces, the
  // choice is to freeze one such node's moves or to spill something. Freezing
  // costs a copy in the generated code; spilling costs a memory access on every
  // use, so freeze goes first.
  const char* source =
      "fn f(int n) -> int {"
      " int a=n+1; int b=n+2; int c=n+3; int d=n+4; int e=n+5; int g=n+6;"
      " int h=n+7; int i=n+8; int j=n+9; int k=n+10; int l=n+11; int m=n+12;"
      " int t=0; int z=0;"
      " while (z < 4) { int u = t; t = u+a+b+c+d+e+g+h+i+j+k+l+m; z = z+1; }"
      " return t; }";
  auto c = compile(source);
  const RegisterAssignment assignment = c->allocate("f");

  CHECK(assignment.frozen > 0);
  CHECK(c->verify("f", assignment).empty());
}

TEST("coalescing and freezing are counted separately") {
  auto c = compile("fn f(int n) -> int { int t = 0;"
                   " while (n > 0) { t = t + n; n = n - 1; } return t; }");
  const RegisterAssignment assignment = c->allocate("f");
  // A loop with room to spare coalesces its phi copies and freezes nothing.
  CHECK(assignment.coalesced > 0);
  CHECK_EQ(assignment.frozen, std::size_t{0});
  CHECK_EQ(assignment.spilled, std::size_t{0});
}

TEST("allocation is deterministic") {
  const char* source =
      "fn f(int n, int a, int b) -> int { int t = 0;"
      " while (n > 0) { t = t + a * b + n; n = n - 1; } return t; }";
  CHECK_EQ(assemblyFor(source, RegAllocKind::Graph),
           assemblyFor(source, RegAllocKind::Graph));
}

// ---------------------------------------------------------------------------
// The verifier
// ---------------------------------------------------------------------------

TEST("the allocation verifier rejects two live values in one register") {
  auto c = compile("fn f(int a, int b) -> int { return a * b + a - b; }");
  RegisterAssignment assignment = c->allocate("f");
  CHECK(c->verify("f", assignment).empty());

  // Corrupt it the way a colouring bug would: force both arguments into one
  // register.
  const ir::Function& fn = c->requireFunction("f");
  const ir::Value* a = valueNamed(fn, "a");
  const ir::Value* b = valueNamed(fn, "b");
  CHECK(a != nullptr);
  CHECK(b != nullptr);
  assignment.assigned[a] = MReg::RBX;
  assignment.assigned[b] = MReg::RBX;

  const std::vector<std::string> errors = c->verify("f", assignment);
  CHECK(!errors.empty());
  bool mentionsBoth = false;
  for (const std::string& error : errors) {
    if (contains(error, "both live in")) {
      mentionsBoth = true;
    }
  }
  CHECK(mentionsBoth);
}

TEST("the allocation verifier rejects a caller-saved register across a call") {
  auto c = compile("fn callee(int x) -> int { return x + x + x + x + x + x + x + x; }"
                   "fn f(int n) -> int { int keep = n + 1; return callee(n) + keep; }",
                   /*optLevel=*/1);
  RegisterAssignment assignment = c->allocate("f");
  CHECK(c->verify("f", assignment).empty());

  // r11 is the one caller-saved register in the pool, so putting a value that
  // spans a call there is exactly the mistake the rule exists to catch.
  for (auto& [value, reg] : assignment.assigned) {
    (void)value;
    if (x86_64WindowsTarget().isCalleeSaved(reg)) {
      reg = MReg::R11;
    }
  }

  const std::vector<std::string> errors = c->verify("f", assignment);
  bool mentionsCall = false;
  for (const std::string& error : errors) {
    if (contains(error, "across a call")) {
      mentionsCall = true;
    }
  }
  CHECK(mentionsCall);
}

// ---------------------------------------------------------------------------
// Generated code
// ---------------------------------------------------------------------------

TEST("callee-saved registers are pushed and popped in matching order") {
  const std::string assembly = assemblyFor(
      "fn f(int a, int b, int c) -> int { int t = 0;"
      " while (t < a) { t = t + b * c; } return t + a + b + c; }",
      RegAllocKind::Graph);

  std::vector<std::string> pushes;
  std::vector<std::string> pops;
  std::istringstream in(assembly);
  std::string line;
  while (std::getline(in, line)) {
    if (contains(line, "pushq") && contains(line, "callee-saved")) {
      pushes.push_back(line.substr(line.find("%"), line.find("#") - line.find("%")));
    }
    if (contains(line, "popq") && contains(line, "callee-saved")) {
      pops.push_back(line.substr(line.find("%"), line.find("#") - line.find("%")));
    }
  }
  CHECK_EQ(pushes.size(), pops.size());
  CHECK(!pushes.empty());
  std::reverse(pops.begin(), pops.end());
  CHECK(pushes == pops);

  // rsp is restored from rbp rather than trusted, so the frame unwinds even if
  // something below moved it.
  CHECK(contains(assembly, "leaq"));
}

TEST("the naive allocator still puts every value in a frame slot") {
  // ADR-08: this stays working forever, because it is the tool that says
  // whether a miscompile is the graph allocator's fault.
  const std::string assembly = assemblyFor(
      "fn f(int n) -> int { int t = 0; while (n > 0) { t = t + n; n = n - 1; } return t; }",
      RegAllocKind::Naive);
  CHECK(frameAccesses(assembly) > 0);
  CHECK(!contains(assembly, "callee-saved"));
}
