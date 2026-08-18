#pragma once

#include <iosfwd>
#include <string_view>

#include "optiforge/support/SourceLocation.h"

namespace optiforge {

class SourceManager;

enum class DiagSeverity {
  Note,
  Warning,
  Error,
  Fatal,
};

std::string_view toString(DiagSeverity severity);

/// The single place diagnostics are formatted and counted.
///
/// No other module writes to stderr (architectural_design.md §7.1). Output goes
/// to an injected stream so tests can capture it without touching the process.
///
/// Rendered form:
///
///     path/file.of:7:5: error: use of undeclared variable 'y'
///         y = x + 5;
///         ^~~~
class DiagnosticEngine {
public:
  DiagnosticEngine(const SourceManager& sources, std::ostream& out);

  /// Diagnostic anchored at a single position.
  void report(SourceLocation loc, DiagSeverity severity, std::string_view message);

  /// Diagnostic underlining a span. Multi-line spans fall back to a caret at
  /// the start, since underlining across lines is not meaningful here.
  void report(SourceRange range, DiagSeverity severity, std::string_view message);

  /// Diagnostic with no source position, such as a driver or environment error.
  void reportGlobal(DiagSeverity severity, std::string_view message);

  void error(SourceLocation loc, std::string_view m) { report(loc, DiagSeverity::Error, m); }
  void warning(SourceLocation loc, std::string_view m) { report(loc, DiagSeverity::Warning, m); }
  void note(SourceLocation loc, std::string_view m) { report(loc, DiagSeverity::Note, m); }

  unsigned errorCount() const { return errors_; }
  unsigned warningCount() const { return warnings_; }
  bool hadError() const { return errors_ > 0; }

  /// Promotes warnings to errors (-Werror). Affects diagnostics reported after
  /// this call.
  void setWarningsAsErrors(bool value) { warningsAsErrors_ = value; }
  bool warningsAsErrors() const { return warningsAsErrors_; }

  /// Prints the trailing "N errors generated." line, if anything was reported.
  /// Returns true when at least one error was seen.
  bool printSummary();

private:
  /// Applies the -Werror promotion and updates the counters. Returns the
  /// severity the diagnostic should actually be printed as.
  DiagSeverity classify(DiagSeverity severity);

  void emitHeader(DiagSeverity severity, std::string_view message, const SourceLocation* loc);
  void emitSnippet(SourceRange range);

  const SourceManager& sources_;
  std::ostream& out_;
  unsigned errors_ = 0;
  unsigned warnings_ = 0;
  bool warningsAsErrors_ = false;
};

}  // namespace optiforge
