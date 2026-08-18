#include "optiforge/support/SourceManager.h"

#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <system_error>
#include <utility>

namespace optiforge {

std::uint64_t fnv1a64(std::string_view data) {
  // Hex form, because the decimal basis is easy to mistype by a digit and the
  // resulting hash still "looks fine" while matching nothing.
  constexpr std::uint64_t kOffsetBasis = 0xcbf29ce484222325ULL;
  constexpr std::uint64_t kPrime = 0x100000001b3ULL;

  std::uint64_t hash = kOffsetBasis;
  for (unsigned char byte : data) {
    hash ^= static_cast<std::uint64_t>(byte);
    hash *= kPrime;
  }
  return hash;
}

namespace {

/// Byte offset of the first character of each line. Index 0 is always present,
/// so a file with no newline at all still has exactly one line.
std::vector<std::uint32_t> computeLineStarts(std::string_view text) {
  std::vector<std::uint32_t> starts;
  starts.push_back(0);
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\n') {
      starts.push_back(static_cast<std::uint32_t>(i + 1));
    }
  }
  return starts;
}

}  // namespace

FileID SourceManager::addBuffer(std::string path, std::string contents) {
  Entry entry;
  entry.path = std::move(path);
  entry.contents = std::move(contents);
  entry.lineStarts = computeLineStarts(entry.contents);
  entry.hash = fnv1a64(entry.contents);

  files_.push_back(std::move(entry));
  return static_cast<FileID>(files_.size() - 1);
}

std::optional<FileID> SourceManager::addFile(const std::string& path) {
  // Check the file type before opening. On glibc, fopen() on a directory
  // succeeds and subsequent reads simply yield nothing, so a directory passed
  // on the command line would silently become an empty source file rather
  // than an error.
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    return std::nullopt;
  }

  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return std::nullopt;
  }

  std::string contents{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  if (in.bad()) {
    return std::nullopt;
  }

  return addBuffer(path, std::move(contents));
}

const SourceManager::Entry* SourceManager::entry(FileID file) const {
  if (!isValid(file)) {
    return nullptr;
  }
  return &files_[file];
}

std::string_view SourceManager::path(FileID file) const {
  const Entry* e = entry(file);
  return e ? std::string_view(e->path) : std::string_view{};
}

std::string_view SourceManager::contents(FileID file) const {
  const Entry* e = entry(file);
  return e ? std::string_view(e->contents) : std::string_view{};
}

std::uint64_t SourceManager::contentHash(FileID file) const {
  const Entry* e = entry(file);
  return e ? e->hash : 0;
}

std::uint32_t SourceManager::lineCount(FileID file) const {
  const Entry* e = entry(file);
  return e ? static_cast<std::uint32_t>(e->lineStarts.size()) : 0;
}

std::string_view SourceManager::line(FileID file, std::uint32_t lineNo) const {
  const Entry* e = entry(file);
  if (e == nullptr || lineNo == 0 || lineNo > e->lineStarts.size()) {
    return {};
  }

  const std::size_t begin = e->lineStarts[lineNo - 1];
  const std::size_t end =
      (lineNo < e->lineStarts.size()) ? e->lineStarts[lineNo] : e->contents.size();

  std::string_view text(e->contents);
  text = text.substr(begin, end - begin);

  // Strip the terminator so callers never have to think about CRLF.
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

}  // namespace optiforge
