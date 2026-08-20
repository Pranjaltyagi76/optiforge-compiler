#pragma once

#include <string_view>
#include <vector>

namespace optiforge {

/// Which profile-guided decisions are allowed to look at the profile.
///
/// Exists for metric G-05. A speedup that cannot be named to a decision is not
/// a finding -- it is as likely to be an accidental code-layout shift as
/// anything the profile bought -- and `methodology.md` section 5 attributes one
/// by switching a single decision off and re-measuring the difference.
///
/// **A disabled decision takes its no-profile path**, which is the same code an
/// ordinary `-O2` build runs, not a third behaviour that exists only during
/// attribution runs. That equivalence is what makes the subtraction meaningful:
/// `contribution(P) = full gain - gain with P disabled` only holds if disabling
/// P leaves everything else exactly as it was.
struct PgoControls {
  bool inlining = true;   ///< G-06: a hot call site gets a larger size budget
  bool unrolling = true;  ///< G-07: a measured trip count chooses a factor
  bool regalloc = true;   ///< G-08: spill cost from measured block counts
  bool layout = true;     ///< G-09: hot chains laid out to fall through
  bool coldSize = true;   ///< G-10: a cold function is compiled at -O1

  /// True when nothing has been switched off, i.e. an ordinary profiled build.
  bool everythingEnabled() const {
    return inlining && unrolling && regalloc && layout && coldSize;
  }
};

/// Turns off the decision named by the argument of `--disable-pgo=`.
///
/// Returns false when the name is not a decision. That has to be an error
/// rather than a shrug: an attribution run whose flag was misspelled disables
/// nothing, measures the full speedup, and reports it as the contribution of a
/// decision that was never switched off -- a wrong number that looks right.
bool disablePgoDecision(std::string_view name, PgoControls& controls);

/// Every decision name, in the order the attribution table lists them.
std::vector<std::string_view> pgoDecisionNames();

/// The names of the decisions that are switched *off*, for `--pgo-remarks`.
///
/// Derived from the same table as everything else rather than hand-listed at
/// the print site, so adding a decision cannot leave the remark quietly
/// under-reporting what an attribution run actually disabled.
std::vector<std::string_view> disabledPgoDecisionNames(const PgoControls& controls);

}  // namespace optiforge
