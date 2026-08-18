#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "optiforge/driver/Options.h"
#include "optiforge/frontend/AST.h"
#include "optiforge/frontend/ASTPrinter.h"
#include "optiforge/frontend/Lexer.h"
#include "optiforge/frontend/Parser.h"
#include "optiforge/frontend/Sema.h"
#include "optiforge/frontend/Symbol.h"
#include "optiforge/frontend/Token.h"
#include "optiforge/ir/Module.h"
#include "optiforge/ir/Printer.h"
#include "optiforge/ir/Verifier.h"
#include "optiforge/irgen/IRGen.h"
#include "optiforge/backend/CodeGen.h"
#include "optiforge/backend/TargetInfo.h"
#include "optiforge/driver/Toolchain.h"
#include "optiforge/support/Diagnostic.h"
#include "optiforge/support/SourceManager.h"

using namespace optiforge;

namespace {

ExitCode runCompilation(const Options& opts, SourceManager& sources, DiagnosticEngine& diags) {
  const std::optional<FileID> file = sources.addFile(opts.inputPath);
  if (!file.has_value()) {
    diags.reportGlobal(DiagSeverity::Error, "cannot open input file '" + opts.inputPath + "'");
    return ExitCode::EnvironmentError;
  }

  // --- Lexical analysis ---
  Lexer lexer(sources, *file, diags);
  const std::vector<Token> tokens = lexer.tokenize();

  if (opts.emit == EmitStage::Tokens) {
    // Dump what was scanned even when there were lexical errors: seeing the
    // token stream is exactly what makes such an error diagnosable.
    printTokens(tokens, std::cout);
    return diags.hadError() ? ExitCode::CompileError : ExitCode::Success;
  }

  if (diags.hadError()) {
    // Parsing a stream with error tokens produces noise, not information.
    return ExitCode::CompileError;
  }

  // --- Parsing ---
  Parser parser(tokens, diags);
  const std::unique_ptr<Program> program = parser.parseProgram();

  if (parser.hadError()) {
    return ExitCode::CompileError;
  }

  // --- Semantic analysis ---
  // The symbol table must outlive this scope's use of the AST: nodes hold raw
  // pointers to symbols it owns.
  SymbolTable symbols;
  Sema sema(diags, symbols);
  sema.analyze(*program);

  // Consult the engine rather than the pass's own flag: -Werror promotion
  // happens inside DiagnosticEngine, so a pass that reported only warnings
  // still believes it succeeded.
  if (diags.hadError()) {
    return ExitCode::CompileError;
  }

  if (opts.emit == EmitStage::Ast) {
    printAST(*program, std::cout);
    return ExitCode::Success;
  }

  // --- IR generation ---
  IRGen irgen(diags, opts.inputPath, sources.contentHash(*file));
  const std::unique_ptr<ir::Module> module = irgen.run(*program);

  ir::Verifier verifier;
  if (!verifier.verify(*module)) {
    // Invalid IR is a compiler bug, not a user error. Say so plainly and name
    // what is wrong rather than letting a later stage crash on it.
    diags.reportGlobal(DiagSeverity::Error,
                       "internal compiler error: generated IR failed verification");
    verifier.printErrors(std::cerr);
    return ExitCode::InternalError;
  }

  if (opts.emit == EmitStage::Ir) {
    ir::printModule(*module, std::cout);
    return ExitCode::Success;
  }
  if (opts.emit == EmitStage::Cfg) {
    ir::printCFG(*module, std::cout);
    return ExitCode::Success;
  }

  // --- Code generation ---
  backend::CodeGen codegen(backend::x86_64WindowsTarget());
  const backend::MModule machine = codegen.run(*module);

  if (opts.emit == EmitStage::Asm) {
    backend::printAssembly(machine, std::cout);
    return ExitCode::Success;
  }

  // --- Assemble and link ---
  const std::filesystem::path inputPath(opts.inputPath);
  const std::string stem = inputPath.stem().string();
  std::filesystem::path outputPath =
      opts.outputPath.empty() ? std::filesystem::path(stem + ".exe")
                              : std::filesystem::path(opts.outputPath);

  // Intermediates are named after the *output*, not the input: two inputs
  // compiled to different outputs in one directory would otherwise write to
  // the same .s and .o.
  const std::string tempStem = outputPath.stem().string();
  const std::filesystem::path asmPath = outputPath.parent_path() / (tempStem + ".s");
  const std::filesystem::path objectPath = outputPath.parent_path() / (tempStem + ".o");

  {
    std::ofstream asmFile(asmPath);
    if (!asmFile) {
      diags.reportGlobal(DiagSeverity::Error,
                         "cannot write assembly to '" + asmPath.string() + "'");
      return ExitCode::EnvironmentError;
    }
    backend::printAssembly(machine, asmFile);
  }

  Toolchain toolchain(diags, opts.runtimeDir);

  const auto cleanup = [&](bool keepObject) {
    if (opts.keepTemps) {
      return;
    }
    std::error_code ec;
    std::filesystem::remove(asmPath, ec);
    if (!keepObject) {
      std::filesystem::remove(objectPath, ec);
    }
  };

  if (!toolchain.assemble(asmPath.string(), objectPath.string())) {
    cleanup(false);
    return ExitCode::EnvironmentError;
  }

  if (opts.emit == EmitStage::Obj) {
    cleanup(/*keepObject=*/true);
    return ExitCode::Success;
  }

  if (!toolchain.hasRuntime()) {
    toolchain.reportMissingRuntime();
    cleanup(false);
    return ExitCode::EnvironmentError;
  }

  if (!toolchain.link(objectPath.string(), outputPath.string())) {
    cleanup(false);
    return ExitCode::EnvironmentError;
  }

  cleanup(false);
  return ExitCode::Success;
}

}  // namespace

int main(int argc, char** argv) {
  // Recorded so the runtime library can be found relative to this executable.
  Toolchain::setExecutablePath(argv[0]);

  Options opts;
  if (!parseOptions(argc, argv, opts, std::cerr)) {
    return toInt(ExitCode::UsageError);
  }

  if (opts.showHelp) {
    printHelp(std::cout);
    return toInt(ExitCode::Success);
  }

  if (opts.showVersion) {
    printVersion(std::cout);
    return toInt(ExitCode::Success);
  }

  if (opts.inputPath.empty()) {
    std::cerr << "optiforge: error: no input file\n"
                 "  try 'optiforge --help'\n";
    return toInt(ExitCode::UsageError);
  }

  SourceManager sources;
  DiagnosticEngine diags(sources, std::cerr);
  diags.setWarningsAsErrors(opts.warningsAsErrors);

  ExitCode result = runCompilation(opts, sources, diags);
  diags.printSummary();

  // Final backstop. The DiagnosticEngine is the single authority on whether
  // compilation failed (architectural_design.md 7.1); a stage that returns
  // Success while the engine holds an error -- a warning promoted by -Werror,
  // for instance -- must not produce a zero exit status (CLI-09).
  if (result == ExitCode::Success && diags.hadError()) {
    result = ExitCode::CompileError;
  }
  return toInt(result);
}
