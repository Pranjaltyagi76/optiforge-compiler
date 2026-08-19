#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "TestHarness.h"
#include "optiforge/analysis/AnalysisManager.h"
#include "optiforge/backend/CodeGen.h"
#include "optiforge/backend/TargetInfo.h"
#include "optiforge/frontend/Lexer.h"
#include "optiforge/frontend/Parser.h"
#include "optiforge/frontend/Sema.h"
#include "optiforge/frontend/Symbol.h"
#include "optiforge/ir/Module.h"
#include "optiforge/irgen/IRGen.h"
#include "optiforge/support/Diagnostic.h"
#include "optiforge/support/SourceManager.h"

using namespace optiforge;
using namespace optiforge::backend;

namespace {

/// Compiles a source fragment all the way to assembly text.
///
/// Defaults to the naive allocator, because that is what the assertions in this
/// file are about: the instruction selector and the ABI, both of which are
/// easiest to read when every value is in a known frame slot. The graph
/// allocator has its own file.
std::string assemblyFor(std::string source,
                        RegAllocKind allocator = RegAllocKind::Naive) {
  SourceManager sm;
  std::ostringstream diagOut;
  const FileID file = sm.addBuffer("t.of", std::move(source));
  DiagnosticEngine diags(sm, diagOut);

  Lexer lexer(sm, file, diags);
  const std::vector<Token> tokens = lexer.tokenize();

  Parser parser(tokens, diags);
  std::unique_ptr<Program> ast = parser.parseProgram();
  if (parser.hadError()) {
    return "<parse error>\n" + diagOut.str();
  }

  SymbolTable symbols;
  Sema sema(diags, symbols);
  if (!sema.analyze(*ast, /*requireEntryPoint=*/false)) {
    return "<sema error>\n" + diagOut.str();
  }

  IRGen irgen(diags, "t.of", sm.contentHash(file));
  const std::unique_ptr<ir::Module> module = irgen.run(*ast);

  analysis::AnalysisManager analyses;
  CodeGen codegen(x86_64WindowsTarget(), allocator);
  const MModule machine = codegen.run(*module, analyses);

  std::ostringstream out;
  printAssembly(machine, out);
  return out.str();
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

std::size_t countOf(const std::string& haystack, const std::string& needle) {
  std::size_t count = 0;
  for (std::size_t i = haystack.find(needle); i != std::string::npos;
       i = haystack.find(needle, i + needle.size())) {
    ++count;
  }
  return count;
}

/// Frame size from the prologue's `subq $N, %rsp`.
long frameSizeOf(const std::string& assembly) {
  const std::size_t at = assembly.find("subq       $");
  if (at == std::string::npos) {
    return -1;
  }
  return std::strtol(assembly.c_str() + at + 12, nullptr, 10);
}

}  // namespace

// ---------------------------------------------------------------------------
// ABI facts (ADR-10)
// ---------------------------------------------------------------------------

TEST("the target reports the Microsoft x64 argument registers") {
  const TargetInfo& target = x86_64WindowsTarget();
  CHECK_EQ(target.integerArgRegister(0), MReg::RCX);
  CHECK_EQ(target.integerArgRegister(1), MReg::RDX);
  CHECK_EQ(target.integerArgRegister(2), MReg::R8);
  CHECK_EQ(target.integerArgRegister(3), MReg::R9);
  CHECK_EQ(target.integerArgRegister(4), MReg::None);  // fifth goes on the stack
  CHECK_EQ(target.maxRegisterArgs(), 4u);
  CHECK_EQ(target.shadowSpaceBytes(), 32);
}

TEST("float arguments use the same four positions as integer ones") {
  const TargetInfo& target = x86_64WindowsTarget();
  CHECK_EQ(target.floatArgRegister(0), MReg::XMM0);
  CHECK_EQ(target.floatArgRegister(3), MReg::XMM3);
  CHECK_EQ(target.floatArgRegister(4), MReg::None);
}

TEST("scratch registers are never argument registers") {
  // Otherwise staging argument i would clobber argument j while a call is
  // being set up.
  const TargetInfo& target = x86_64WindowsTarget();
  for (unsigned i = 0; i < target.maxRegisterArgs(); ++i) {
    CHECK(target.scratchInt0() != target.integerArgRegister(i));
    CHECK(target.scratchInt1() != target.integerArgRegister(i));
    CHECK(target.scratchFloat0() != target.floatArgRegister(i));
    CHECK(target.scratchFloat1() != target.floatArgRegister(i));
  }
}

TEST("64-bit PE/COFF applies no underscore prefix") {
  CHECK_EQ(x86_64WindowsTarget().symbolName("main"), std::string("main"));
}

// ---------------------------------------------------------------------------
// Prologue, frame and epilogue
// ---------------------------------------------------------------------------

TEST("every function gets a standard prologue and epilogue") {
  const std::string assembly = assemblyFor("fn f() -> int { return 1; }");
  CHECK(contains(assembly, "pushq      %rbp"));
  CHECK(contains(assembly, "movq       %rsp, %rbp"));
  CHECK(contains(assembly, "popq       %rbp"));
  CHECK(contains(assembly, "ret"));
}

TEST("the frame is always a multiple of 16 bytes") {
  // Microsoft x64 requires rsp to be 16-byte aligned at every call. rsp is
  // 0 mod 16 after `push rbp`, so the frame must preserve that.
  for (const char* source : {
           "fn f() -> int { return 1; }",
           "fn f(int a) -> int { int b = a; return b; }",
           "fn g(int a, int b, int c, int d, int e) -> int { return e; } "
           "fn f() -> int { return g(1,2,3,4,5); }",
       }) {
    const long frame = frameSizeOf(assemblyFor(source));
    CHECK(frame >= 0);
    CHECK_EQ(frame % 16, 0L);
  }
}

TEST("the frame always reserves shadow space") {
  // 32 bytes, required even when the callee takes no arguments at all.
  const long frame = frameSizeOf(assemblyFor("fn f() -> int { return 1; }"));
  CHECK(frame >= 32);
}

TEST("a sixth argument is passed on the stack above the shadow space") {
  const std::string assembly = assemblyFor(
      "fn g(int a, int b, int c, int d, int e, int f) -> int { return f; }\n"
      "fn caller() -> int { return g(1, 2, 3, 4, 5, 6); }\n");
  // Arguments 4 and 5 land at rsp+32 and rsp+40.
  CHECK(contains(assembly, "32(%rsp)"));
  CHECK(contains(assembly, "40(%rsp)"));
}

TEST("an incoming stack argument is read from above the saved frame") {
  const std::string assembly =
      assemblyFor("fn g(int a, int b, int c, int d, int e) -> int { return e; }");
  // rbp + 16 (return address and saved rbp) + 32 (shadow space) = 48.
  CHECK(contains(assembly, "48(%rbp)"));
}

// ---------------------------------------------------------------------------
// Instruction selection
// ---------------------------------------------------------------------------

TEST("integer arithmetic selects the expected instructions") {
  CHECK(contains(assemblyFor("fn f(int a, int b) -> int { return a + b; }"), "addq"));
  CHECK(contains(assemblyFor("fn f(int a, int b) -> int { return a - b; }"), "subq"));
  CHECK(contains(assemblyFor("fn f(int a, int b) -> int { return a * b; }"), "imulq"));
}

TEST("division sign-extends into rdx:rax before idiv") {
  // idiv divides rdx:rax, so omitting cqto would divide by whatever rdx held.
  const std::string assembly = assemblyFor("fn f(int a, int b) -> int { return a / b; }");
  CHECK(contains(assembly, "cqto"));
  CHECK(contains(assembly, "idivq"));
  const std::size_t cqto = assembly.find("cqto");
  const std::size_t idiv = assembly.find("idivq");
  CHECK(cqto < idiv);
}

TEST("remainder takes its result from rdx, not rax") {
  const std::string assembly = assemblyFor("fn f(int a, int b) -> int { return a % b; }");
  CHECK(contains(assembly, "idivq"));
  const std::size_t idiv = assembly.find("idivq");
  CHECK(assembly.find("%rdx,", idiv) != std::string::npos);
}

TEST("float arithmetic uses the SSE scalar-double forms") {
  CHECK(contains(assemblyFor("fn f(float a, float b) -> float { return a + b; }"), "addsd"));
  CHECK(contains(assemblyFor("fn f(float a, float b) -> float { return a * b; }"), "mulsd"));
  CHECK(contains(assemblyFor("fn f(float a, float b) -> float { return a / b; }"), "divsd"));
}

TEST("float negation flips the sign bit rather than subtracting from zero") {
  // 0.0 - x would turn -0.0 into +0.0.
  const std::string assembly = assemblyFor("fn f(float x) -> float { return -x; }");
  CHECK(contains(assembly, "xorpd"));
  CHECK(contains(assembly, ".LCnegmask"));
  // xorpd reads 16 bytes, so the mask must be 16-byte aligned.
  CHECK(contains(assembly, ".align\t16"));
}

TEST("integer comparison uses the signed setcc family") {
  CHECK(contains(assemblyFor("fn f(int a, int b) -> bool { return a < b; }"), "setl"));
  CHECK(contains(assemblyFor("fn f(int a, int b) -> bool { return a > b; }"), "setg"));
  CHECK(contains(assemblyFor("fn f(int a, int b) -> bool { return a <= b; }"), "setle"));
}

TEST("float comparison is false on NaN, which comisd's flags alone are not") {
  // comisd sets ZF, PF and CF for an unordered pair, so the below family --
  // setb, setbe -- reports true when either operand is NaN. IEEE-754 says every
  // ordered predicate is false there, so `<` and `<=` compare the operands the
  // other way round and read the above family instead.
  const std::string less = assemblyFor("fn f(float a, float b) -> bool { return a < b; }");
  CHECK(contains(less, "comisd"));
  CHECK(contains(less, "seta"));
  CHECK(!contains(less, "setb"));
  CHECK(contains(assemblyFor("fn f(float a, float b) -> bool { return a <= b; }"), "setae"));
  CHECK(contains(assemblyFor("fn f(float a, float b) -> bool { return a > b; }"), "seta"));
  CHECK(contains(assemblyFor("fn f(float a, float b) -> bool { return a >= b; }"), "setae"));

  // Equality is the one pair the flags cannot express alone: ZF means "equal"
  // and "unordered" both, so the parity flag has to be consulted as well.
  const std::string equal = assemblyFor("fn f(float a, float b) -> bool { return a == b; }");
  CHECK(contains(equal, "sete"));
  CHECK(contains(equal, "setnp"));
  CHECK(contains(equal, "andb"));

  const std::string unequal =
      assemblyFor("fn f(float a, float b) -> bool { return a != b; }");
  CHECK(contains(unequal, "setne"));
  CHECK(contains(unequal, "setp"));
  CHECK(contains(unequal, "orb"));
}

TEST("setcc names an 8-bit register and movzb widens only its source") {
  // `movzbq %rax, %rax` is an operand-size mismatch the assembler rejects.
  const std::string assembly = assemblyFor("fn f(int a, int b) -> bool { return a < b; }");
  CHECK(contains(assembly, "setl       %al"));
  CHECK(contains(assembly, "movzbq     %al, %rax"));
}

TEST("int to float conversion uses cvtsi2sd") {
  CHECK(contains(assemblyFor("fn f(int i) -> float { float g = i; return g; }"),
                 "cvtsi2sdq"));
}

// ---------------------------------------------------------------------------
// Memory and control flow
// ---------------------------------------------------------------------------

TEST("a slot is addressed directly rather than through its computed address") {
  const std::string assembly = assemblyFor("fn f(int a) -> int { return a; }");
  // A load from a known alloca becomes disp(%rbp); an leaq would mean the
  // address was materialized first.
  CHECK(!contains(assembly, "leaq"));
}

TEST("conditional branches test the condition and take both edges") {
  const std::string assembly =
      assemblyFor("fn f(int a) -> int { if (a > 0) { return 1; } return 0; }");
  CHECK(contains(assembly, "testq"));
  CHECK(contains(assembly, "jne"));
  CHECK(contains(assembly, "jmp"));
}

TEST("block labels are unique per function") {
  const std::string assembly = assemblyFor(
      "fn a(int n) -> int { if (n > 0) { return 1; } return 0; }\n"
      "fn b(int n) -> int { if (n > 0) { return 1; } return 0; }\n");
  // Both functions have an if.then.1; qualifying by function keeps them apart.
  CHECK(contains(assembly, ".L_a_if_then_1"));
  CHECK(contains(assembly, ".L_b_if_then_1"));
}

TEST("labels contain no characters the assembler rejects") {
  const std::string assembly =
      assemblyFor("fn f(int n) -> int { while (n > 0) { n = n - 1; } return n; }");
  // IR labels like "while.cond.1" contain dots, which are not valid here.
  CHECK(!contains(assembly, ".L_f_while.cond"));
  CHECK(contains(assembly, ".L_f_while_cond_1"));
}

TEST("calls emit the callee symbol and store the result") {
  const std::string assembly =
      assemblyFor("fn g(int a) -> int { return a; } fn f() -> int { return g(7); }");
  CHECK(contains(assembly, "call       g"));
  CHECK(contains(assembly, "movq       $7, %rcx"));
}

TEST("a float return travels in xmm0") {
  CHECK(contains(assemblyFor("fn f() -> float { return 1.5; }"), "%xmm0"));
}

TEST("declarations without bodies emit no code") {
  // print_int is resolved by the linker against libofrt.
  const std::string assembly = assemblyFor("fn f() { print_int(1); }");
  CHECK(contains(assembly, "call       print_int"));
  CHECK(!contains(assembly, "print_int:"));
}

// ---------------------------------------------------------------------------
// Output quality
// ---------------------------------------------------------------------------

TEST("float constants are emitted as exact bit patterns") {
  // A decimal rendering would depend on the assembler's float parsing.
  const std::string assembly = assemblyFor("fn f() -> float { return 3.14; }");
  CHECK(contains(assembly, ".quad\t0x40091eb851eb851f"));
}

TEST("identical float constants are emitted once") {
  const std::string assembly =
      assemblyFor("fn f(float x) -> float { return x * 2.5 + 2.5; }");
  CHECK_EQ(countOf(assembly, ".LCF0:"), std::size_t{1});
  CHECK(!contains(assembly, ".LCF1:"));
}

TEST("generated assembly is deterministic") {
  const char* source =
      "fn f(int n) -> int { int t = 0; while (n > 0) { t = t + n; n = n - 1; } return t; }";
  CHECK_EQ(assemblyFor(source), assemblyFor(source));
}

TEST("assembly carries IR provenance as comments") {
  // Requirement BE-09: the output should be readable against --emit=ir.
  const std::string assembly = assemblyFor("fn f(int a, int b) -> int { return a + b; }");
  CHECK(contains(assembly, "# %t"));
}
