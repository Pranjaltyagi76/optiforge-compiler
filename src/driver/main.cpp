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
#include "optiforge/frontend/Token.h"
#include "optiforge/support/Diagnostic.h"
#include "optiforge/support/SourceManager.h"

using namespace optiforge;

namespace {

/// Reports that a pipeline stage exists in the CLI but not yet in the compiler.
ExitCode notImplemented(DiagnosticEngine& diags, EmitStage stage, int phase) {
  diags.reportGlobal(DiagSeverity::Error,
                     "--emit=" + std::string(toString(stage)) +
                         " is not implemented yet (roadmap Phase " + std::to_string(phase) +
                         ")");
  return ExitCode::InternalError;
}

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

  if (opts.emit == EmitStage::Ast) {
    printAST(*program, std::cout);
    return ExitCode::Success;
  }

  // --- Later stages ---
  switch (opts.emit) {
    case EmitStage::Ir:
    case EmitStage::Cfg:
      return notImplemented(diags, opts.emit, 3);
    case EmitStage::Asm:
    case EmitStage::Obj:
    case EmitStage::Executable:
      return notImplemented(diags, opts.emit, 4);
    case EmitStage::Tokens:
    case EmitStage::Ast:
      break;  // handled above
  }
  return ExitCode::InternalError;
}

}  // namespace

int main(int argc, char** argv) {
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

  const ExitCode result = runCompilation(opts, sources, diags);
  diags.printSummary();
  return toInt(result);
}
