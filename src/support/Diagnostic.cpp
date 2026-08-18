#include "optiforge/support/Diagnostic.h"

#include <ostream>
#include <string>

#include "optiforge/support/SourceManager.h"

namespace optiforge {

std::string_view toString(DiagSeverity severity) {
  switch (severity) {
    case DiagSeverity::Note:
      return "note";
    case DiagSeverity::Warning:
      return "warning";
    case DiagSeverity::Error:
      return "error";
    case DiagSeverity::Fatal:
      return "fatal error";
  }
  return "unknown";
}

DiagnosticEngine::DiagnosticEngine(const SourceManager& sources, std::ostream& out)
    : sources_(sources), out_(out) {}

void DiagnosticEngine::emitHeader(DiagSeverity severity, std::string_view message,
                                  const SourceLocation* loc) {
  if (loc != nullptr && loc->isValid()) {
    out_ << sources_.path(loc->file) << ':' << loc->line << ':' << loc->col << ": ";
  } else {
    out_ << "optiforge: ";
  }
  out_ << toString(severity) << ": " << message << '\n';
}

void DiagnosticEngine::emitSnippet(SourceRange range) {
  const SourceLocation begin = range.begin;
  if (!begin.isValid()) {
    return;
  }

  const std::string_view text = sources_.line(begin.file, begin.line);
  if (text.empty()) {
    // Nothing useful to show (empty line, or the line number is out of range).
    return;
  }

  out_ << text << '\n';

  // Indent the marker to the offending column. Copying tabs from the source
  // keeps the marker aligned regardless of how wide the reader renders a tab.
  const std::size_t caretIndex = begin.col - 1;
  const std::size_t indent = caretIndex < text.size() ? caretIndex : text.size();
  for (std::size_t i = 0; i < indent; ++i) {
    out_ << (text[i] == '\t' ? '\t' : ' ');
  }

  // Half-open range: columns [begin.col, end.col) are covered, so the width is
  // the column difference. A degenerate range underlines a single character.
  std::size_t width = 1;
  if (range.isSingleLine() && range.end.col > begin.col) {
    width = range.end.col - begin.col;
  }
  if (indent + width > text.size()) {
    width = text.size() > indent ? text.size() - indent : 1;
  }

  out_ << '^';
  for (std::size_t i = 1; i < width; ++i) {
    out_ << '~';
  }
  out_ << '\n';
}

DiagSeverity DiagnosticEngine::classify(DiagSeverity severity) {
  // -Werror changes both how a diagnostic is labelled and how it is counted; a
  // "warning" that fails the build but prints as a warning would be a lie.
  // Both effects are decided here so the two cannot drift apart.
  DiagSeverity effective = severity;
  if (warningsAsErrors_ && severity == DiagSeverity::Warning) {
    effective = DiagSeverity::Error;
  }

  switch (effective) {
    case DiagSeverity::Warning:
      ++warnings_;
      break;
    case DiagSeverity::Error:
    case DiagSeverity::Fatal:
      ++errors_;
      break;
    case DiagSeverity::Note:
      break;
  }
  return effective;
}

void DiagnosticEngine::report(SourceLocation loc, DiagSeverity severity,
                              std::string_view message) {
  report(makeRange(loc), severity, message);
}

void DiagnosticEngine::report(SourceRange range, DiagSeverity severity,
                              std::string_view message) {
  const DiagSeverity effective = classify(severity);
  emitHeader(effective, message, &range.begin);
  emitSnippet(range);
}

void DiagnosticEngine::reportGlobal(DiagSeverity severity, std::string_view message) {
  const DiagSeverity effective = classify(severity);
  emitHeader(effective, message, nullptr);
}

bool DiagnosticEngine::printSummary() {
  const auto plural = [](unsigned n) { return n == 1 ? "" : "s"; };

  if (warnings_ > 0 && errors_ > 0) {
    out_ << warnings_ << " warning" << plural(warnings_) << " and " << errors_ << " error"
         << plural(errors_) << " generated.\n";
  } else if (errors_ > 0) {
    out_ << errors_ << " error" << plural(errors_) << " generated.\n";
  } else if (warnings_ > 0) {
    out_ << warnings_ << " warning" << plural(warnings_) << " generated.\n";
  }

  return errors_ > 0;
}

}  // namespace optiforge
