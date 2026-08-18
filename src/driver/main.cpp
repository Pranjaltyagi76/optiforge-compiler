#include <iostream>
#include <optional>
#include <string>

#include "optiforge/driver/Options.h"
#include "optiforge/support/Diagnostic.h"
#include "optiforge/support/SourceManager.h"

using namespace optiforge;

namespace {

/// Runs the compilation pipeline. Phase 0 has a support layer and a CLI but no
/// front end yet, so this loads the input and reports honestly that there is
/// nothing further to do. Phase 1 replaces the body below with the lexer.
ExitCode runCompilation(const Options& opts, SourceManager& sources, DiagnosticEngine& diags) {
  const std::optional<FileID> file = sources.addFile(opts.inputPath);
  if (!file.has_value()) {
    diags.reportGlobal(DiagSeverity::Error,
                       "cannot open input file '" + opts.inputPath + "'");
    return ExitCode::EnvironmentError;
  }

  diags.reportGlobal(DiagSeverity::Error,
                     std::string("the compilation pipeline is not implemented yet (--emit=") +
                         std::string(toString(opts.emit)) +
                         " requires roadmap Phase 1); "
                         "only --help and --version are functional in Phase 0");
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
