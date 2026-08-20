#include "optiforge/profile/Profile.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <istream>
#include <limits>
#include <set>
#include <sstream>

namespace optiforge::profile {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

std::vector<std::string> split(const std::string& line) {
  std::vector<std::string> fields;
  std::istringstream in(line);
  std::string field;
  while (in >> field) {
    fields.push_back(field);
  }
  return fields;
}

/// Parses `taken=1234`. Returns false when the prefix or the number is wrong,
/// which the caller turns into "malformed line, skipped".
bool parseKeyed(const std::string& field, std::string_view key, std::uint64_t& out) {
  if (field.size() <= key.size() || field.compare(0, key.size(), key) != 0 ||
      field[key.size()] != '=') {
    return false;
  }
  const std::string number = field.substr(key.size() + 1);
  if (number.empty()) {
    return false;
  }
  try {
    std::size_t consumed = 0;
    const unsigned long long value = std::stoull(number, &consumed);
    if (consumed != number.size()) {
      return false;
    }
    out = static_cast<std::uint64_t>(value);
    return true;
  } catch (...) {
    return false;
  }
}

bool parseUnsigned(const std::string& text, std::uint64_t& out) {
  if (text.empty()) {
    return false;
  }
  try {
    std::size_t consumed = 0;
    const int base = (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
                         ? 16
                         : 10;
    const unsigned long long value = std::stoull(text, &consumed, base);
    if (consumed != text.size()) {
      return false;
    }
    out = static_cast<std::uint64_t>(value);
    return true;
  } catch (...) {
    return false;
  }
}

bool parseDouble(const std::string& text, double& out) {
  try {
    std::size_t consumed = 0;
    const double value = std::stod(text, &consumed);
    if (consumed != text.size()) {
      return false;
    }
    out = value;
    return true;
  } catch (...) {
    return false;
  }
}

/// Joins fields [first, last) with single spaces. Record names are written that
/// way by the runtime and are split back apart here.
std::string join(const std::vector<std::string>& fields, std::size_t first,
                 std::size_t last) {
  std::string out;
  for (std::size_t i = first; i < last; ++i) {
    if (!out.empty()) {
      out += ' ';
    }
    out += fields[i];
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Small value types
// ---------------------------------------------------------------------------

std::string_view toString(Heat heat) {
  switch (heat) {
    case Heat::Unknown: return "UNKNOWN";
    case Heat::Cold: return "COLD";
    case Heat::Warm: return "WARM";
    case Heat::Hot: return "HOT";
  }
  return "UNKNOWN";
}

double BranchProfile::takenProbability() const {
  const std::uint64_t all = total();
  return all == 0 ? kNaN : static_cast<double>(taken) / static_cast<double>(all);
}

double BranchProfile::bias() const {
  const double probability = takenProbability();
  return std::isnan(probability) ? kNaN : std::max(probability, 1.0 - probability);
}

double LoopProfile::tripCount() const {
  return entries == 0 ? kNaN
                      : static_cast<double>(iterations) / static_cast<double>(entries);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

namespace {

std::string keyOf(std::string_view function, std::string_view block) {
  std::string key(function);
  key += ' ';
  key.append(block);
  return key;
}

template <class Map>
const typename Map::mapped_type* lookup(const Map& map, const std::string& key) {
  const auto it = map.find(key);
  return it == map.end() ? nullptr : &it->second;
}

}  // namespace

std::uint64_t ProfileData::functionCount(std::string_view function) const {
  const auto it = functionCounts_.find(std::string(function));
  return it == functionCounts_.end() ? 0 : it->second;
}

std::uint64_t ProfileData::functionExecutions(std::string_view function) const {
  const auto it = functionExecutions_.find(std::string(function));
  return it == functionExecutions_.end() ? 0 : it->second;
}

std::uint64_t ProfileData::blockCount(std::string_view function,
                                      std::string_view block) const {
  const auto it = blockCounts_.find(keyOf(function, block));
  return it == blockCounts_.end() ? 0 : it->second;
}

const BranchProfile* ProfileData::branch(std::string_view function,
                                         std::string_view block) const {
  return lookup(branches_, keyOf(function, block));
}

const LoopProfile* ProfileData::loop(std::string_view function,
                                     std::string_view header) const {
  return lookup(loops_, keyOf(function, header));
}

double ProfileData::functionTime(std::string_view function) const {
  const auto it = times_.find(std::string(function));
  return it == times_.end() ? kNaN : it->second;
}

Heat ProfileData::functionHeat(std::string_view function) const {
  const auto it = functionHeat_.find(std::string(function));
  return it == functionHeat_.end() ? Heat::Unknown : it->second;
}

Heat ProfileData::blockHeat(std::string_view function, std::string_view block) const {
  const auto it = blockHeat_.find(keyOf(function, block));
  return it == blockHeat_.end() ? Heat::Unknown : it->second;
}

// ---------------------------------------------------------------------------
// Classification (System_design.md §15.1)
// ---------------------------------------------------------------------------

namespace {

/// Sorts by count descending, then by name, and labels each entry.
///
/// Hot is the smallest prefix whose cumulative count reaches the threshold
/// share of the total, floored so a program that ran four times has no hot
/// anything. Cold is a count of zero, or one negligible against the busiest
/// entry. Everything else is warm.
///
/// The name is the tie-break so two entities with equal counts always come out
/// in the same order, which is what makes the report diffable across runs.
std::vector<HeatEntry> classifyGroup(const std::map<std::string, std::uint64_t>& weights,
                                     const std::map<std::string, std::uint64_t>& display,
                                     const ProfileLoadOptions& options,
                                     std::map<std::string, Heat>& heatOut) {
  std::vector<HeatEntry> entries;
  entries.reserve(weights.size());
  std::uint64_t total = 0;
  std::uint64_t maximum = 0;
  for (const auto& [name, weight] : weights) {
    const auto shown = display.find(name);
    entries.push_back({name, shown == display.end() ? weight : shown->second, weight,
                       Heat::Unknown});
    total += weight;
    maximum = std::max(maximum, weight);
  }

  std::sort(entries.begin(), entries.end(), [](const HeatEntry& a, const HeatEntry& b) {
    return a.weight != b.weight ? a.weight > b.weight : a.name < b.name;
  });

  const double threshold =
      static_cast<double>(total) * (options.hotThresholdPercent / 100.0);
  const double coldCeiling = static_cast<double>(maximum) * options.coldFractionOfMax;

  std::uint64_t cumulative = 0;
  bool prefixClosed = false;
  for (HeatEntry& entry : entries) {
    const bool inPrefix = !prefixClosed;
    cumulative += entry.weight;
    if (static_cast<double>(cumulative) >= threshold) {
      prefixClosed = true;  // this entry is the last one in the prefix
    }

    if (inPrefix && entry.weight >= options.hotFloor) {
      entry.heat = Heat::Hot;
    } else if (entry.weight == 0 || static_cast<double>(entry.weight) < coldCeiling) {
      entry.heat = Heat::Cold;
    } else {
      entry.heat = Heat::Warm;
    }
    heatOut[entry.name] = entry.heat;
  }

  return entries;
}

}  // namespace

void ProfileData::classify() {
  totalBlockExecutions_ = 0;
  for (const auto& [name, count] : blockCounts_) {
    (void)name;
    totalBlockExecutions_ += count;
  }

  // A function's weight is the work done inside it, not how often it was
  // entered. See ProfileData::functionHeat for why that departs from the design
  // sketch. Blocks are attributed by the "function block" key the records
  // already use, so no extra bookkeeping is needed.
  functionExecutions_.clear();
  for (const auto& [key, count] : blockCounts_) {
    const std::size_t space = key.find(' ');
    if (space != std::string::npos) {
      functionExecutions_[key.substr(0, space)] += count;
    }
  }
  // A function with a FUNCTION record but no BLOCK records still has to appear,
  // or it vanishes from the report entirely.
  for (const auto& [name, calls] : functionCounts_) {
    (void)calls;
    functionExecutions_.emplace(name, 0);
  }

  functions_ = classifyGroup(functionExecutions_, functionCounts_, options_,
                             functionHeat_);
  blocks_ = classifyGroup(blockCounts_, blockCounts_, options_, blockHeat_);

  // Flow conservation. Both of these follow from where the counters are placed,
  // so a violation means the file is not describing a real execution -- a
  // hand-edit, a merge of two runs, or a bug in the writer.
  for (const auto& [name, branch] : branches_) {
    const auto block = blockCounts_.find(name);
    if (block == blockCounts_.end()) {
      continue;  // no block record to check against; already counted as a gap
    }
    if (branch.total() != block->second) {
      flowViolations_.push_back(
          "branch '" + name + "': taken + not_taken = " +
          std::to_string(branch.total()) + " but the block ran " +
          std::to_string(block->second) + " times");
    }
  }
  for (const auto& [name, loop] : loops_) {
    const auto header = blockCounts_.find(name);
    if (header == blockCounts_.end()) {
      continue;
    }
    if (loop.entries + loop.iterations != header->second) {
      flowViolations_.push_back(
          "loop '" + name + "': entries + iterations = " +
          std::to_string(loop.entries + loop.iterations) + " but the header ran " +
          std::to_string(header->second) + " times");
    }
  }
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

/// Reads a profile a line at a time, tolerating anything it does not recognize.
///
/// Every branch here ends in a usable object. A line that makes no sense is
/// dropped with a warning rather than failing the load, because the alternative
/// -- refusing a profile over one bad line -- trades a small loss of data for a
/// total one.
class ProfileParser {
public:
  ProfileParser(ProfileData& data, std::string label) : data_(data), label_(std::move(label)) {}

  void run(std::istream& in) {
    std::string line;
    std::size_t lineNumber = 0;
    bool sawMagic = false;

    while (std::getline(in, line)) {
      ++lineNumber;
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      const std::vector<std::string> fields = split(line);
      if (fields.empty() || fields[0][0] == '#') {
        continue;
      }

      if (!sawMagic) {
        if (fields[0] != "OPTIFORGE_PROFILE") {
          warn("is not a profile: expected 'OPTIFORGE_PROFILE' on the first "
               "non-blank line, found '" + fields[0] + "'");
          return;
        }
        std::uint64_t version = 0;
        if (fields.size() < 2 || !parseUnsigned(fields[1], version)) {
          warn("has no readable format version on line " + std::to_string(lineNumber));
          return;
        }
        if (version > kSupportedProfileVersion) {
          warn("is format version " + std::to_string(version) + ", and this " +
               "compiler understands up to " +
               std::to_string(kSupportedProfileVersion) + "; refusing it");
          return;
        }
        data_.header_.version = static_cast<unsigned>(version);
        sawMagic = true;
        data_.valid_ = true;
        continue;
      }

      parseRecord(fields, lineNumber);
    }

    if (!sawMagic) {
      warn("is empty");
      return;
    }
    data_.classify();
  }

private:
  void warn(const std::string& what) {
    data_.warnings_.push_back("profile '" + label_ + "' " + what);
  }

  void malformed(std::size_t line, const std::string& why) {
    // Once per kind, not once per line: a profile with ten thousand bad lines
    // should say so in one sentence, not ten thousand.
    if (reportedMalformed_.insert(why).second) {
      warn("line " + std::to_string(line) + ": " + why + " (ignored; further " +
           "occurrences of this are not reported)");
    }
  }

  void parseRecord(const std::vector<std::string>& fields, std::size_t line) {
    const std::string& keyword = fields[0];

    // --- Header fields ---
    if (keyword == "SOURCE") {
      data_.header_.source = join(fields, 1, fields.size());
      return;
    }
    if (keyword == "COMPILER") {
      data_.header_.compiler = join(fields, 1, fields.size());
      return;
    }
    if (keyword == "SRCHASH") {
      std::uint64_t hash = 0;
      if (fields.size() < 2 || !parseUnsigned(fields[1], hash)) {
        malformed(line, "SRCHASH is not a number");
        return;
      }
      data_.header_.sourceHash = hash;
      return;
    }
    if (keyword == "OPTLEVEL") {
      std::uint64_t level = 0;
      if (fields.size() < 2 || !parseUnsigned(fields[1], level)) {
        malformed(line, "OPTLEVEL is not a number");
        return;
      }
      data_.header_.optLevel = static_cast<int>(level);
      return;
    }
    if (keyword == "RUNS" || keyword == "TOTAL_SAMPLES") {
      std::uint64_t value = 0;
      if (fields.size() < 2 || !parseUnsigned(fields[1], value)) {
        malformed(line, keyword + " is not a number");
        return;
      }
      (keyword == "RUNS" ? data_.header_.runs : data_.header_.totalSamples) = value;
      return;
    }

    // --- Records ---
    if (keyword == "FUNCTION") {
      std::uint64_t count = 0;
      if (fields.size() != 3 || !parseUnsigned(fields[2], count)) {
        malformed(line, "FUNCTION expects '<name> <count>'");
        return;
      }
      data_.functionCounts_[fields[1]] += count;
      return;
    }
    if (keyword == "BLOCK") {
      std::uint64_t count = 0;
      if (fields.size() != 4 || !parseUnsigned(fields[3], count)) {
        malformed(line, "BLOCK expects '<function> <block> <count>'");
        return;
      }
      data_.blockCounts_[fields[1] + " " + fields[2]] += count;
      return;
    }
    if (keyword == "BRANCH") {
      std::uint64_t taken = 0;
      std::uint64_t notTaken = 0;
      if (fields.size() != 5 || !parseKeyed(fields[3], "taken", taken) ||
          !parseKeyed(fields[4], "not_taken", notTaken)) {
        malformed(line, "BRANCH expects '<function> <block> taken=N not_taken=N'");
        return;
      }
      BranchProfile& entry = data_.branches_[fields[1] + " " + fields[2]];
      entry.taken += taken;
      entry.notTaken += notTaken;
      return;
    }
    if (keyword == "LOOP") {
      std::uint64_t entries = 0;
      std::uint64_t iterations = 0;
      if (fields.size() != 5 || !parseKeyed(fields[3], "entries", entries) ||
          !parseKeyed(fields[4], "iterations", iterations)) {
        malformed(line, "LOOP expects '<function> <header> entries=N iterations=N'");
        return;
      }
      LoopProfile& entry = data_.loops_[fields[1] + " " + fields[2]];
      entry.entries += entries;
      entry.iterations += iterations;
      return;
    }
    if (keyword == "TIME") {
      double milliseconds = 0.0;
      if (fields.size() != 3 || !parseDouble(fields[2], milliseconds)) {
        malformed(line, "TIME expects '<function> <milliseconds>'");
        return;
      }
      data_.times_[fields[1]] += milliseconds;
      return;
    }

    // Forward compatibility: a record this compiler predates is not an error.
    if (unknownKinds_.insert(keyword).second) {
      warn("contains record type '" + keyword +
           "', which this compiler does not know; ignoring it");
    }
  }

  ProfileData& data_;
  std::string label_;
  std::set<std::string> reportedMalformed_;
  std::set<std::string> unknownKinds_;
};

ProfileData parseProfile(std::istream& in, const std::string& label,
                         const ProfileLoadOptions& options) {
  ProfileData data;
  data.options_ = options;
  ProfileParser parser(data, label);
  parser.run(in);
  return data;
}

ProfileData loadProfile(const std::string& path, const ProfileLoadOptions& options) {
  std::ifstream file(path);
  if (!file) {
    ProfileData data;
    data.options_ = options;
    data.warnings_.push_back("profile '" + path + "' cannot be opened");
    return data;
  }
  return parseProfile(file, path, options);
}

}  // namespace optiforge::profile
