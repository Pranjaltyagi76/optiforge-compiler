#pragma once

#include <iosfwd>
#include <string>
#include <vector>
#include <string_view>

#include "optiforge/support/PgoControls.h"

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
  Analysis,
  Asm,
  Obj,
};

std::string_view toString(EmitStage stage);

/// Which register allocator the backend should use (ADR-08).
///
/// Named here rather than taken from the backend so the option surface does not
/// drag a backend header into the driver's interface. The driver maps it to
/// `backend::RegAllocKind` at the one place it constructs the code generator.
enum class RegAllocChoice {
  Naive,  ///< Phase 4: every value in a frame slot. The bisection tool.
  Graph,  ///< Phase 8: Chaitin-Briggs graph colouring. The default.
};

std::string_view toString(RegAllocChoice choice);

/// Parses the argument of --regalloc=. Returns false if the name is unknown.
bool parseRegAllocChoice(std::string_view name, RegAllocChoice& out);

/// Parses the argument of --emit=. Returns false if the name is unknown.
bool parseEmitStage(std::string_view name, EmitStage& out);

struct Options {
  std::string inputPath;
  std::string outputPath;
  int optLevel = 0;
  EmitStage emit = EmitStage::Executable;
  RegAllocChoice regalloc = RegAllocChoice::Graph;
  bool warningsAsErrors = false;
  std::string runtimeDir;
  bool keepTemps = false;
  bool printAfterAll = false;
  bool verifyEach = false;
  bool printRegAlloc = false;

  // --- Profiling (Phase 9) ---
  /// Build an instrumented binary that writes a `.prof` when it exits.
  bool profile = false;
  /// Also accumulate per-function wall time. Opt-in, because unlike a counter
  /// increment this puts a real call in the hot path.
  bool profileTime = false;
  /// Path the instrumented binary writes to unless $OPTIFORGE_PROFILE_OUT says
  /// otherwise. Defaults to the output's stem plus `.prof`.
  std::string profileOut;

  // --- Profile consumption (Phase 10) ---
  /// Profile to compile against. Loading it never affects correctness: a
  /// missing, corrupt or stale file produces a warning and an ordinary build.
  std::string useProfile;
  /// Print a hot-path report for this profile and exit. Needs no input file.
  std::string profileReport;
  /// Cumulative share of executions that defines "hot" (PGO-04).
  double hotThreshold = 80.0;
  /// Explain every profile-guided decision on stderr (PGO-13).
  bool pgoRemarks = false;
  /// Which profile-guided decisions may consult the profile (Phase 12, G-05).
  /// Everything is on unless `--disable-pgo=` turns one off; a disabled
  /// decision takes the same path an unprofiled build takes.
  PgoControls pgo;
  std::string printAfter;
  std::vector<std::string> disabledPasses;
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
