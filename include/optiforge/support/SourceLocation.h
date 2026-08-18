#pragma once

#include <cstdint>

namespace optiforge {

/// Identifies a file registered with a SourceManager.
using FileID = std::uint32_t;

inline constexpr FileID kInvalidFileID = static_cast<FileID>(-1);

/// A point in a source file. Line and column are 1-based, matching how
/// editors and compiler diagnostics number them.
struct SourceLocation {
  FileID file = kInvalidFileID;
  std::uint32_t line = 0;
  std::uint32_t col = 0;

  static constexpr SourceLocation invalid() { return {}; }

  constexpr bool isValid() const { return file != kInvalidFileID && line > 0 && col > 0; }

  friend constexpr bool operator==(const SourceLocation&, const SourceLocation&) = default;
};

/// A half-open span of source text: [begin, end). A range whose endpoints are
/// equal denotes a single character position.
struct SourceRange {
  SourceLocation begin;
  SourceLocation end;

  static constexpr SourceRange invalid() { return {}; }

  constexpr bool isValid() const { return begin.isValid(); }

  /// True when the whole range sits on one line, which is the only case the
  /// diagnostic renderer can underline.
  constexpr bool isSingleLine() const {
    return begin.isValid() && end.isValid() && begin.file == end.file && begin.line == end.line;
  }

  friend constexpr bool operator==(const SourceRange&, const SourceRange&) = default;
};

/// Convenience: a range covering exactly one location.
constexpr SourceRange makeRange(SourceLocation loc) { return {loc, loc}; }

}  // namespace optiforge
