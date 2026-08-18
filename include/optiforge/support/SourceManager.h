#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "optiforge/support/SourceLocation.h"

namespace optiforge {

/// FNV-1a, 64-bit. Used for the source hash stamped into profile headers so
/// stale profiles can be detected (architectural_design.md ADR-06).
std::uint64_t fnv1a64(std::string_view data);

/// Owns the text of every source file in a compilation and answers the
/// questions diagnostics need: what is the path, what does line N say.
///
/// Line starts are indexed once at registration, so rendering a caret is O(1)
/// rather than a scan of the whole buffer per diagnostic.
class SourceManager {
public:
  /// Registers in-memory text. Always succeeds. Returns the new file id.
  FileID addBuffer(std::string path, std::string contents);

  /// Reads a file from disk. Returns nullopt if it cannot be opened or read.
  std::optional<FileID> addFile(const std::string& path);

  bool isValid(FileID file) const { return file < files_.size(); }
  FileID fileCount() const { return static_cast<FileID>(files_.size()); }

  std::string_view path(FileID file) const;
  std::string_view contents(FileID file) const;
  std::uint64_t contentHash(FileID file) const;

  /// Number of lines. A trailing newline produces a final empty line, matching
  /// how editors display files.
  std::uint32_t lineCount(FileID file) const;

  /// Text of a 1-based line, with its line terminator stripped (both LF and
  /// CRLF). Returns an empty view if the file or line number is out of range.
  std::string_view line(FileID file, std::uint32_t line) const;

private:
  struct Entry {
    std::string path;
    std::string contents;
    std::vector<std::uint32_t> lineStarts;  // byte offset of each line's first char
    std::uint64_t hash = 0;
  };

  const Entry* entry(FileID file) const;

  // std::deque, not std::vector: Token::lexeme and every diagnostic snippet are
  // string_views into Entry::contents. A vector reallocates on growth, which
  // moves each Entry -- and a short string stores its bytes inside the object,
  // so its data address changes and all outstanding views dangle. deque keeps
  // references to existing elements valid across push_back.
  std::deque<Entry> files_;
};

}  // namespace optiforge
