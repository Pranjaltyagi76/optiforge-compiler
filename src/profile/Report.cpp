#include <algorithm>
#include <cmath>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "optiforge/profile/Profile.h"

namespace optiforge::profile {

namespace {

/// 50000000 -> "50,000,000". Grouping is not decoration here: the whole point
/// of the report is that a reader can see at a glance which number is six
/// orders of magnitude larger than the others.
std::string grouped(std::uint64_t value) {
  std::string digits = std::to_string(value);
  std::string out;
  int since = 0;
  for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
    if (since == 3) {
      out += ',';
      since = 0;
    }
    out += *it;
    ++since;
  }
  std::reverse(out.begin(), out.end());
  return out;
}

std::string percent(double fraction, int places = 2) {
  if (std::isnan(fraction)) {
    return "n/a";
  }
  std::ostringstream out;
  out << std::fixed << std::setprecision(places) << (fraction * 100.0) << "%";
  return out.str();
}

std::string fixed(double value, int places = 1) {
  if (std::isnan(value)) {
    return "n/a";
  }
  std::ostringstream out;
  out << std::fixed << std::setprecision(places) << value;
  return out.str();
}

/// The unroll factor Phase 11 would pick for a loop with this trip count.
///
/// Reported here rather than decided here: this file makes no changes, and a
/// report that says what *would* happen is how a PGO decision becomes reviewable
/// before any pass exists to make it.
int suggestedUnrollFactor(double tripCount) {
  if (std::isnan(tripCount) || tripCount < 4.0) {
    return 1;  // too short to be worth the code growth
  }
  if (tripCount < 8.0) {
    return 2;
  }
  if (tripCount < 32.0) {
    return 4;
  }
  return 8;
}

/// `total` is the sum of the weights heat was decided from, so the percentage
/// beside each entry is its share of *that*, not of the count printed next to
/// it. For functions those differ, which is the whole point: the share is where
/// the time went, the count is how often it was called.
void printGroup(std::ostream& out, const char* title,
                const std::vector<HeatEntry>& entries, Heat wanted,
                std::uint64_t total, const char* countLabel) {
  std::vector<const HeatEntry*> selected;
  for (const HeatEntry& entry : entries) {
    if (entry.heat == wanted) {
      selected.push_back(&entry);
    }
  }
  if (selected.empty()) {
    return;
  }

  out << "\n" << title << "\n";
  for (const HeatEntry* entry : selected) {
    const double share = total == 0 ? 0.0
                                    : static_cast<double>(entry->weight) /
                                          static_cast<double>(total);
    out << "  " << std::left << std::setw(24) << entry->name << std::right
        << countLabel << " = " << std::setw(12) << grouped(entry->count) << "   "
        << std::setw(8) << percent(share) << " of executions   ["
        << toString(entry->heat) << "]\n";
  }
}

}  // namespace

void printProfileReport(const ProfileData& profile, std::ostream& out) {
  out << "OptiForge Profile Report";
  if (!profile.header().source.empty()) {
    // ASCII, not an em dash: this goes to a console whose code page is not
    // guaranteed to be UTF-8, and a report that renders as mojibake is a report
    // nobody trusts.
    out << " - " << profile.header().source;
  }
  out << "\n";

  if (!profile.isValid()) {
    out << "\nThis file could not be read as a profile.\n";
    for (const std::string& warning : profile.warnings()) {
      out << "  warning: " << warning << "\n";
    }
    return;
  }

  out << "Format version " << profile.header().version << ", built at -O"
      << profile.header().optLevel << " by " << profile.header().compiler << "\n";
  out << "Source hash 0x" << std::hex << std::setw(16) << std::setfill('0')
      << profile.header().sourceHash << std::dec << std::setfill(' ') << ", "
      << profile.header().runs << " run(s), "
      << grouped(profile.header().totalSamples) << " samples\n";

  for (const std::string& warning : profile.warnings()) {
    out << "  warning: " << warning << "\n";
  }

  if (profile.totalBlockExecutions() == 0) {
    out << "\nNothing in this profile ran. Either the program exited before doing\n"
           "any work, or it was never run at all.\n";
    return;
  }

  // --- Functions ---
  std::uint64_t totalWork = 0;
  for (const HeatEntry& entry : profile.functions()) {
    totalWork += entry.weight;
  }
  printGroup(out, "HOT FUNCTIONS", profile.functions(), Heat::Hot, totalWork, "calls");
  printGroup(out, "WARM FUNCTIONS", profile.functions(), Heat::Warm, totalWork, "calls");
  printGroup(out, "COLD FUNCTIONS", profile.functions(), Heat::Cold, totalWork, "calls");

  // --- Loops ---
  //
  // Sorted by total iterations rather than by name: the loop that ran most is
  // the one the reader is looking for, and it should not be buried.
  std::vector<std::pair<std::string, LoopProfile>> loops(profile.loops().begin(),
                                                         profile.loops().end());
  std::sort(loops.begin(), loops.end(), [](const auto& a, const auto& b) {
    return a.second.iterations != b.second.iterations
               ? a.second.iterations > b.second.iterations
               : a.first < b.first;
  });

  bool printedLoopHeading = false;
  for (const auto& [name, loop] : loops) {
    const std::size_t space = name.find(' ');
    const std::string function = name.substr(0, space);
    const std::string header = space == std::string::npos ? "" : name.substr(space + 1);
    const Heat heat = profile.blockHeat(function, header);

    if (!printedLoopHeading) {
      out << "\nLOOPS\n";
      printedLoopHeading = true;
    }
    out << "  " << std::left << std::setw(28) << (function + ":" + header) << std::right
        << "entries = " << std::setw(12) << grouped(loop.entries)
        << "   iterations = " << std::setw(14) << grouped(loop.iterations) << "\n";
    out << "  " << std::string(28, ' ') << "avg trip count = " << fixed(loop.tripCount())
        << "   [" << toString(heat) << "]\n";

    const int factor = suggestedUnrollFactor(loop.tripCount());
    if (heat == Heat::Hot && factor > 1) {
      out << "  " << std::string(28, ' ')
          << "-> unroll candidate (factor " << factor << ")\n";
    } else if (heat == Heat::Cold) {
      out << "  " << std::string(28, ' ') << "-> cold: leave it alone\n";
    }
  }

  // --- Branches ---
  //
  // Only the one-sided ones. A branch that went both ways half the time tells a
  // layout pass nothing, and listing it would bury the ones that do.
  constexpr double kBiasThreshold = 0.90;
  std::vector<std::pair<std::string, BranchProfile>> branches(profile.branches().begin(),
                                                              profile.branches().end());
  std::sort(branches.begin(), branches.end(), [](const auto& a, const auto& b) {
    return a.second.total() != b.second.total() ? a.second.total() > b.second.total()
                                                : a.first < b.first;
  });

  bool printedBranchHeading = false;
  for (const auto& [name, branch] : branches) {
    if (branch.total() == 0 || std::isnan(branch.bias()) ||
        branch.bias() < kBiasThreshold) {
      continue;
    }
    if (!printedBranchHeading) {
      out << "\nBIASED BRANCHES  (over " << percent(kBiasThreshold, 0) << " one-sided)\n";
      printedBranchHeading = true;
    }
    const std::size_t space = name.find(' ');
    const std::string label =
        space == std::string::npos ? name : name.substr(0, space) + ":" + name.substr(space + 1);
    out << "  " << std::left << std::setw(28) << label << std::right << "taken "
        << std::setw(7) << percent(branch.takenProbability()) << "  /  not-taken "
        << std::setw(7) << percent(1.0 - branch.takenProbability()) << "\n";
  }

  // --- Timing, when it was collected ---
  if (!profile.times().empty()) {
    std::vector<std::pair<std::string, double>> times(profile.times().begin(),
                                                      profile.times().end());
    std::sort(times.begin(), times.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    out << "\nWALL TIME (milliseconds, --profile-time)\n";
    for (const auto& [name, milliseconds] : times) {
      out << "  " << std::left << std::setw(28) << name << std::right << std::setw(12)
          << fixed(milliseconds, 3) << "\n";
    }
  }

  // --- Summary ---
  std::size_t hot = 0;
  std::size_t warm = 0;
  std::size_t cold = 0;
  std::size_t never = 0;
  for (const HeatEntry& entry : profile.blocks()) {
    switch (entry.heat) {
      case Heat::Hot: ++hot; break;
      case Heat::Warm: ++warm; break;
      case Heat::Cold: ++cold; break;
      case Heat::Unknown: break;
    }
    if (entry.count == 0) {
      ++never;
    }
  }

  out << "\nSUMMARY\n";
  out << "  " << hot << " hot, " << warm << " warm, " << cold << " cold block(s)"
      << " (" << never << " never executed)\n";
  out << "  hot threshold: " << fixed(profile.options().hotThresholdPercent, 1)
      << "% cumulative, floor " << grouped(profile.options().hotFloor)
      << " executions\n";

  if (hot == 0) {
    out << "  nothing qualified as hot: no block ran at least "
        << grouped(profile.options().hotFloor)
        << " times, so this workload is too small to guide optimization\n";
  }

  if (!profile.flowViolations().empty()) {
    out << "\nFLOW CONSERVATION\n";
    out << "  These records contradict each other, so the counts are advisory:\n";
    for (const std::string& violation : profile.flowViolations()) {
      out << "    " << violation << "\n";
    }
  }
}

}  // namespace optiforge::profile
