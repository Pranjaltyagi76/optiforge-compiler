#pragma once

#include <iosfwd>
#include <string>
#include <string_view>

namespace optiforge {

/// Process exit codes (System_design.md §17.3).
enum class ExitCode : int {
  Success = 0,
  CompileError = 1,   // the user's program is invalid
  UsageError = 2,     // the command line is invalid
  EnvironmentError = 3,  // missing file, assembler not found
  InternalError = 70,    // compiler bug, or a stage that is not implemented yet
};

constexpr int toInt(ExitCode code) { return static_cast<int>(code); }

/// The stage after which compilation stops and its result is dumped.
enum class EmitStage {
  Executable,  // default: run the whole pipeline
  Tokens,
  Ast,
  Ir,
  Cfg,
  Asm,
  Obj,
};

std::string_view toString(EmitStage stage);

/// Parses the argument of --emit=. Returns false if the name is unknown.
bool parseEmitStage(std::string_view name, EmitStage& out);

struct Options {
  std::string inputPath;
  std::string outputPath;
  int optLevel = 0;
  EmitStage emit = EmitStage::Executable;
  bool warningsAsErrors = false;
  std::string runtimeDir;
  bool keepTemps = false;
  bool showHelp = false;
  bool showVersion = false;
};

/// Parses argv into `out`. Returns false on a malformed command line, having
/// written an explanatory message to `err`.
///
/// Parsing does not validate that the input file exists; that is the driver's
/// job, so that "no such file" is an environment error rather than a usage one.
bool parseOptions(int argc, const char* const* argv, Options& out, std::ostream& err);

void printHelp(std::ostream& out);
void printVersion(std::ostream& out);

}  // namespace optiforge
