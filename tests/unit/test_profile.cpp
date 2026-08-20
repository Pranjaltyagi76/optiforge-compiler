#include <cmath>
#include <sstream>
#include <string>

#include "TestHarness.h"
#include "optiforge/profile/Profile.h"

using namespace optiforge;
using namespace optiforge::profile;

namespace {

ProfileData parse(const std::string& text, ProfileLoadOptions options = {}) {
  std::istringstream in(text);
  return parseProfile(in, "test.prof", options);
}

/// A profile of a program with one hot loop, written out the way the runtime
/// writes it. `sum` is called once and runs 4,000 iterations.
std::string wellFormed() {
  return
      "OPTIFORGE_PROFILE 1\n"
      "SOURCE sum.of\n"
      "SRCHASH 0x00000000deadbeef\n"
      "OPTLEVEL 2\n"
      "COMPILER optiforge-0.1.0\n"
      "RUNS 1\n"
      "TOTAL_SAMPLES 8003\n"
      "\n"
      "FUNCTION sum 1\n"
      "FUNCTION main 1\n"
      "\n"
      "BLOCK sum entry 1\n"
      "BLOCK sum while.cond.1 4001\n"
      "BLOCK sum while.body.2 4000\n"
      "BLOCK sum while.end.3 1\n"
      "BLOCK main entry 1\n"
      "\n"
      "BRANCH sum while.cond.1 taken=4000 not_taken=1\n"
      "\n"
      "LOOP sum while.cond.1 entries=1 iterations=4000\n";
}

bool hasWarningContaining(const ProfileData& data, const std::string& needle) {
  for (const std::string& warning : data.warnings()) {
    if (warning.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

TEST("a well-formed profile parses into the counts it states") {
  const ProfileData data = parse(wellFormed());
  CHECK(data.isValid());
  CHECK(data.warnings().empty());

  CHECK_EQ(data.header().version, 1u);
  CHECK_EQ(data.header().optLevel, 2);
  CHECK(data.header().source == "sum.of");
  CHECK(data.matchesSource(0xdeadbeefULL));
  CHECK(!data.matchesSource(0x1234ULL));

  CHECK_EQ(data.functionCount("sum"), std::uint64_t{1});
  CHECK_EQ(data.blockCount("sum", "while.body.2"), std::uint64_t{4000});
  CHECK_EQ(data.blockCount("sum", "no.such.block"), std::uint64_t{0});
  CHECK_EQ(data.totalBlockExecutions(), std::uint64_t{8004});
}

TEST("branch and loop records become probabilities and trip counts") {
  const ProfileData data = parse(wellFormed());

  const BranchProfile* branch = data.branch("sum", "while.cond.1");
  CHECK(branch != nullptr);
  CHECK_EQ(branch->total(), std::uint64_t{4001});
  CHECK(branch->takenProbability() > 0.999);

  const LoopProfile* loop = data.loop("sum", "while.cond.1");
  CHECK(loop != nullptr);
  CHECK_EQ(loop->entries, std::uint64_t{1});
  CHECK(loop->tripCount() == 4000.0);
}

TEST("a branch that never ran reports NaN, not an even split") {
  // "Never observed" and "went both ways equally" must not look alike to a pass
  // deciding which way to lay out the code.
  const BranchProfile never;
  CHECK(std::isnan(never.takenProbability()));
  CHECK(std::isnan(never.bias()));

  const LoopProfile unentered;
  CHECK(std::isnan(unentered.tripCount()));
}

TEST("comments and blank lines are ignored") {
  const ProfileData data = parse(
      "# a leading comment\n"
      "\n"
      "OPTIFORGE_PROFILE 1\n"
      "# another\n"
      "FUNCTION f 3\n"
      "\n"
      "BLOCK f entry 3\n");
  CHECK(data.isValid());
  CHECK(data.warnings().empty());
  CHECK_EQ(data.functionCount("f"), std::uint64_t{3});
}

TEST("a missing profile is a warning, never a failure") {
  const ProfileData data = loadProfile("no/such/file.prof");
  CHECK(!data.isValid());
  CHECK(hasWarningContaining(data, "cannot be opened"));
  // Every query still answers, because the caller must be able to carry on.
  CHECK_EQ(data.functionCount("anything"), std::uint64_t{0});
  CHECK(data.branch("a", "b") == nullptr);
  CHECK(data.functionHeat("anything") == Heat::Unknown);
}

TEST("an empty file is refused rather than treated as an empty profile") {
  const ProfileData data = parse("");
  CHECK(!data.isValid());
  CHECK(hasWarningContaining(data, "is empty"));
}

TEST("a file that is not a profile is refused by its first line") {
  const ProfileData data = parse("hello world\nFUNCTION f 1\n");
  CHECK(!data.isValid());
  CHECK(hasWarningContaining(data, "is not a profile"));
}

TEST("a future format version is refused, not guessed at") {
  const ProfileData data = parse("OPTIFORGE_PROFILE 99\nFUNCTION f 1\n");
  CHECK(!data.isValid());
  CHECK(hasWarningContaining(data, "version 99"));
  CHECK_EQ(data.functionCount("f"), std::uint64_t{0});
}

TEST("an unknown record type is ignored once, and the rest still loads") {
  // Forward compatibility: a profile written by a later compiler must still be
  // usable for the records this one understands.
  const ProfileData data = parse(
      "OPTIFORGE_PROFILE 1\n"
      "SOMETHING_NEW a b c\n"
      "SOMETHING_NEW d e f\n"
      "FUNCTION f 7\n"
      "BLOCK f entry 7\n");
  CHECK(data.isValid());
  CHECK_EQ(data.functionCount("f"), std::uint64_t{7});
  CHECK_EQ(data.warnings().size(), std::size_t{1});  // once, not twice
  CHECK(hasWarningContaining(data, "SOMETHING_NEW"));
}

TEST("a malformed line is dropped and reported once, and the rest still loads") {
  const ProfileData data = parse(
      "OPTIFORGE_PROFILE 1\n"
      "BLOCK f entry notanumber\n"
      "BLOCK f other alsonot\n"
      "BLOCK f good 5\n"
      "BRANCH f good taken=oops not_taken=1\n");
  CHECK(data.isValid());
  CHECK_EQ(data.blockCount("f", "good"), std::uint64_t{5});
  CHECK_EQ(data.blockCount("f", "entry"), std::uint64_t{0});
  CHECK(data.branch("f", "good") == nullptr);
  // Two distinct complaints -- one about BLOCK, one about BRANCH -- and the
  // repeated BLOCK failure does not add a third.
  CHECK_EQ(data.warnings().size(), std::size_t{2});
}

TEST("repeated records for the same entity are summed") {
  // Which is what merging two runs would need (PROF-14), and what a profile
  // with duplicate lines means today.
  const ProfileData data = parse(
      "OPTIFORGE_PROFILE 1\n"
      "FUNCTION f 3\n"
      "FUNCTION f 4\n"
      "BLOCK f entry 3\n"
      "BLOCK f entry 4\n"
      "BRANCH f entry taken=1 not_taken=2\n"
      "BRANCH f entry taken=2 not_taken=2\n");
  CHECK_EQ(data.functionCount("f"), std::uint64_t{7});
  CHECK_EQ(data.blockCount("f", "entry"), std::uint64_t{7});
  CHECK_EQ(data.branch("f", "entry")->total(), std::uint64_t{7});
}

// ---------------------------------------------------------------------------
// Flow conservation
// ---------------------------------------------------------------------------

TEST("a consistent profile reports no flow violations") {
  CHECK(parse(wellFormed()).flowViolations().empty());
}

TEST("a branch whose outcomes do not sum to its block is caught") {
  // The two outcomes of a branch are the counts of its two successors, so they
  // must add up to the block's own count. They cannot disagree in a real run.
  const ProfileData data = parse(
      "OPTIFORGE_PROFILE 1\n"
      "BLOCK f b 100\n"
      "BRANCH f b taken=40 not_taken=40\n");
  CHECK_EQ(data.flowViolations().size(), std::size_t{1});
  CHECK(data.flowViolations()[0].find("taken + not_taken") != std::string::npos);
  // Advisory, not fatal: the counts are still there to be used.
  CHECK(data.isValid());
  CHECK_EQ(data.blockCount("f", "b"), std::uint64_t{100});
}

TEST("a loop whose entries plus iterations miss its header is caught") {
  const ProfileData data = parse(
      "OPTIFORGE_PROFILE 1\n"
      "BLOCK f h 10\n"
      "LOOP f h entries=1 iterations=100\n");
  CHECK_EQ(data.flowViolations().size(), std::size_t{1});
  CHECK(data.flowViolations()[0].find("entries + iterations") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

TEST("the hot prefix is the smallest that reaches the threshold") {
  ProfileLoadOptions options;
  options.hotFloor = 1;  // so the shape of the rule is what is under test
  const ProfileData data = parse(
      "OPTIFORGE_PROFILE 1\n"
      "BLOCK f a 700\n"
      "BLOCK f b 200\n"
      "BLOCK f c 90\n"
      "BLOCK f d 10\n",
      options);

  // Total 1000. 80% is 800, which a + b reaches and a alone does not.
  CHECK(data.blockHeat("f", "a") == Heat::Hot);
  CHECK(data.blockHeat("f", "b") == Heat::Hot);
  CHECK(data.blockHeat("f", "c") == Heat::Warm);
  // 10 is under 0.01% of 700? No -- it is well over, so warm, not cold.
  CHECK(data.blockHeat("f", "d") == Heat::Warm);
}

TEST("the hot floor stops a trivial program having a hot block") {
  // Without it, a program whose busiest block ran four times has a "hot" block,
  // and every threshold discussion becomes noise.
  const ProfileData data = parse(
      "OPTIFORGE_PROFILE 1\n"
      "BLOCK f a 4\n"
      "BLOCK f b 1\n");
  CHECK(data.blockHeat("f", "a") == Heat::Warm);
  CHECK(data.blockHeat("f", "b") == Heat::Warm);
}

TEST("a block that never ran is cold, and one that barely ran is too") {
  const ProfileData data = parse(
      "OPTIFORGE_PROFILE 1\n"
      "BLOCK f busy 10000000\n"
      "BLOCK f rare 5\n"
      "BLOCK f never 0\n");
  CHECK(data.blockHeat("f", "busy") == Heat::Hot);
  CHECK(data.blockHeat("f", "rare") == Heat::Cold);   // under 0.01% of the max
  CHECK(data.blockHeat("f", "never") == Heat::Cold);
}

TEST("--hot-threshold widens or narrows the hot set") {
  const char* source =
      "OPTIFORGE_PROFILE 1\n"
      "BLOCK f a 6000\n"
      "BLOCK f b 3000\n"
      "BLOCK f c 1000\n";

  ProfileLoadOptions narrow;
  narrow.hotThresholdPercent = 50.0;
  CHECK(parse(source, narrow).blockHeat("f", "b") == Heat::Warm);

  ProfileLoadOptions wide;
  wide.hotThresholdPercent = 95.0;
  CHECK(parse(source, wide).blockHeat("f", "b") == Heat::Hot);
}

TEST("a function's heat comes from the work inside it, not its call count") {
  // The case System_design.md 15.1's "inherit heat from the entry block count"
  // rule gets wrong: `compute` is called 20 times and holds all the execution.
  const ProfileData data = parse(
      "OPTIFORGE_PROFILE 1\n"
      "FUNCTION compute 20\n"
      "FUNCTION main 1\n"
      "BLOCK compute entry 20\n"
      "BLOCK compute while.cond.1 100020\n"
      "BLOCK compute while.body.2 100000\n"
      "BLOCK main entry 1\n"
      "BLOCK main while.cond.1 21\n");

  CHECK_EQ(data.functionCount("compute"), std::uint64_t{20});
  CHECK_EQ(data.functionExecutions("compute"), std::uint64_t{200040});
  CHECK(data.functionHeat("compute") == Heat::Hot);
  CHECK(data.functionHeat("main") != Heat::Hot);
}

TEST("an entity the profile says nothing about is Unknown, not Cold") {
  // A pass has to be able to tell "measured, and it never ran" from "not
  // measured at all"; the two justify opposite decisions.
  const ProfileData data = parse(wellFormed());
  CHECK(data.functionHeat("never_compiled") == Heat::Unknown);
  CHECK(data.blockHeat("sum", "no.such.block") == Heat::Unknown);
  CHECK(data.blockHeat("sum", "while.end.3") != Heat::Unknown);
}

TEST("classification is deterministic for equal counts") {
  const char* source =
      "OPTIFORGE_PROFILE 1\n"
      "BLOCK f zzz 100\n"
      "BLOCK f aaa 100\n";
  const ProfileData first = parse(source);
  const ProfileData second = parse(source);
  CHECK_EQ(first.blocks().size(), second.blocks().size());
  for (std::size_t i = 0; i < first.blocks().size(); ++i) {
    CHECK(first.blocks()[i].name == second.blocks()[i].name);
  }
  // The tie-break is the name, so the order is stable rather than incidental.
  CHECK(first.blocks()[0].name == "f aaa");
}
