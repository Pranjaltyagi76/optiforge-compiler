#pragma once

#include <cstdint>
#include <iosfwd>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace optiforge::profile {

/// How often something ran, relative to everything else in the same profile.
///
/// Relative and not absolute: a threshold in executions does not transfer
/// between programs, and one that did would be measuring the workload rather
/// than the code.
enum class Heat : std::uint8_t {
  Unknown,  ///< no profile entry at all -- not the same as "never ran"
  Cold,
  Warm,
  Hot,
};

std::string_view toString(Heat heat);

/// Which way a conditional branch actually went.
struct BranchProfile {
  std::uint64_t taken = 0;
  std::uint64_t notTaken = 0;

  std::uint64_t total() const { return taken + notTaken; }
  /// P(taken), or NaN when the branch never executed. NaN rather than 0.5,
  /// because "never observed" and "an even split" must not look alike to a pass
  /// deciding which way to lay out the code.
  double takenProbability() const;
  /// How one-sided this branch was: 0.5 for an even split, 1.0 for always the
  /// same way. NaN when it never executed.
  double bias() const;
};

/// How often a loop was entered, and how many iterations it ran in total.
struct LoopProfile {
  std::uint64_t entries = 0;
  std::uint64_t iterations = 0;

  /// Average iterations per entry, or NaN when the loop was never entered.
  /// This is what Phase 11 chooses an unroll factor from.
  double tripCount() const;
};

/// The `.prof` header. Everything needed to decide whether this profile
/// describes the program about to be compiled.
struct ProfileHeader {
  unsigned version = 0;
  std::string source;
  std::string compiler;
  std::uint64_t sourceHash = 0;
  int optLevel = -1;
  std::uint64_t runs = 0;
  std::uint64_t totalSamples = 0;
};

/// One named entity with its count and the heat derived from it.
struct HeatEntry {
  std::string name;  ///< "sum" for a function, "sum while.cond.1" otherwise
  /// Calls for a function, executions for a block.
  std::uint64_t count = 0;
  /// What the heat was actually decided from: executions for a function (which
  /// is not the same as its call count), the count itself for a block.
  std::uint64_t weight = 0;
  Heat heat = Heat::Unknown;
};

/// Knobs the classifier reads. Defaults are the ones System_design.md §15.1
/// specifies.
struct ProfileLoadOptions {
  /// Cumulative share of total executions that defines the hot prefix
  /// (PGO-04, `--hot-threshold=`).
  double hotThresholdPercent = 80.0;

  /// A block must run at least this many times to be called hot, whatever the
  /// percentages say. Without it every trivial program has a "hot" block that
  /// ran four times, and every threshold discussion becomes noise.
  std::uint64_t hotFloor = 1000;

  /// Below this share of the busiest entity, something is cold.
  double coldFractionOfMax = 0.0001;  // 0.01%
};

class ProfileData;

ProfileData parseProfile(std::istream& in, const std::string& label,
                         const ProfileLoadOptions& options);
ProfileData loadProfile(const std::string& path, const ProfileLoadOptions& options);

/// A parsed `.prof`, plus the hot/warm/cold classification derived from it.
///
/// Plain data. This deliberately knows nothing about the IR
/// (architectural_design.md §3, rule 6), which is what lets the format be
/// tested standalone and shared with `libofprof`.
///
/// **Nothing here can fail loudly.** A profile that is missing, empty, corrupt
/// or stale yields an object that answers "Unknown" to everything and carries
/// warnings explaining why. That is requirement PGO-11: profile data changes
/// decisions, never semantics.
class ProfileData {
public:
  ProfileData() = default;

  /// True when the file parsed as a profile of a version this compiler knows.
  /// A valid profile can still be stale, empty, or internally inconsistent --
  /// those are separate questions with separate answers below.
  bool isValid() const { return valid_; }
  const ProfileHeader& header() const { return header_; }

  /// Everything noticed while reading, in the order noticed. Empty on a clean
  /// load.
  const std::vector<std::string>& warnings() const { return warnings_; }

  /// True when this profile was collected from source with the given hash.
  bool matchesSource(std::uint64_t hash) const {
    return valid_ && header_.sourceHash == hash;
  }

  // --- Queries. Every one is safe on an invalid profile. ---

  /// How many times the function was called.
  std::uint64_t functionCount(std::string_view function) const;

  /// How many block executions happened *inside* the function. This, not the
  /// call count, is what its heat is derived from -- see `functionHeat`.
  std::uint64_t functionExecutions(std::string_view function) const;
  std::uint64_t blockCount(std::string_view function, std::string_view block) const;

  /// Null when the profile says nothing about this branch or loop, which is the
  /// case every consuming pass has to handle.
  const BranchProfile* branch(std::string_view function, std::string_view block) const;
  const LoopProfile* loop(std::string_view function, std::string_view header) const;

  /// Milliseconds, or NaN when `--profile-time` was not used.
  double functionTime(std::string_view function) const;

  /// Heat from the work done inside the function, not from how often it was
  /// entered.
  ///
  /// **A deliberate departure from System_design.md §15.1**, which specifies
  /// "functions inherit heat from their entry block count". Entry count
  /// measures how often you arrive, not how much happens once you do. A
  /// function called twenty times whose loop runs five thousand iterations each
  /// time is the hot function of its program by any definition a pass could
  /// use, and the entry-count rule labels it cold. Summed block executions is
  /// the measure that answers "where does the time go", which is the question
  /// an inlining or layout decision is really asking.
  Heat functionHeat(std::string_view function) const;
  Heat blockHeat(std::string_view function, std::string_view block) const;

  // --- Enumeration, for the report and for tests ---

  const std::vector<HeatEntry>& functions() const { return functions_; }
  const std::vector<HeatEntry>& blocks() const { return blocks_; }
  const std::map<std::string, BranchProfile>& branches() const { return branches_; }
  const std::map<std::string, LoopProfile>& loops() const { return loops_; }
  const std::map<std::string, double>& times() const { return times_; }

  const ProfileLoadOptions& options() const { return options_; }
  std::uint64_t totalBlockExecutions() const { return totalBlockExecutions_; }

  /// Records that contradicted each other. A branch's two outcomes must sum to
  /// its block's count, and a loop's entries plus iterations must sum to its
  /// header's; both follow from how the counters are placed, so a violation
  /// means the file is not describing a real execution. Advisory: the counts
  /// are still used, with a warning (System_design.md §14.3).
  const std::vector<std::string>& flowViolations() const { return flowViolations_; }

private:
  friend class ProfileParser;
  friend ProfileData parseProfile(std::istream&, const std::string&,
                                  const ProfileLoadOptions&);
  friend ProfileData loadProfile(const std::string&, const ProfileLoadOptions&);
  void classify();

  bool valid_ = false;
  ProfileHeader header_;
  ProfileLoadOptions options_;

  // Keyed by the record's own name field: "sum" for a function, "sum entry"
  // for anything block-scoped. That is exactly the form the runtime writes, so
  // no splitting and rejoining is needed anywhere.
  std::map<std::string, std::uint64_t> functionCounts_;
  std::map<std::string, std::uint64_t> functionExecutions_;  // summed from blocks
  std::map<std::string, std::uint64_t> blockCounts_;
  std::map<std::string, BranchProfile> branches_;
  std::map<std::string, LoopProfile> loops_;
  std::map<std::string, double> times_;

  /// Sorted by executions descending. `count` here is the *call* count, since
  /// that is what a reader wants to see; the heat beside it came from
  /// executions.
  std::vector<HeatEntry> functions_;
  std::vector<HeatEntry> blocks_;     // sorted by count, descending
  std::map<std::string, Heat> functionHeat_;
  std::map<std::string, Heat> blockHeat_;

  std::uint64_t totalBlockExecutions_ = 0;
  std::vector<std::string> warnings_;
  std::vector<std::string> flowViolations_;
};

/// The highest `.prof` format version this compiler understands.
inline constexpr unsigned kSupportedProfileVersion = 1;

/// Reads a profile from a file. Never throws.
///
/// A file that does not exist, cannot be opened, or is not a profile produces
/// an invalid `ProfileData` whose `warnings()` say what went wrong. The caller
/// reports those and compiles without a profile.
inline ProfileData loadProfile(const std::string& path) { return loadProfile(path, {}); }

/// Same, from an already-open stream. `label` names it in diagnostics.
inline ProfileData parseProfile(std::istream& in, const std::string& label) {
  return parseProfile(in, label, {});
}

/// The human-readable hot-path report (PGO-05, `--profile-report=`).
void printProfileReport(const ProfileData& profile, std::ostream& out);

}  // namespace optiforge::profile
