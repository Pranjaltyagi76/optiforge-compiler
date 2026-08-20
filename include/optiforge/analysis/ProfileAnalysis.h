#pragma once

#include "optiforge/analysis/AnalysisManager.h"
#include "optiforge/profile/Profile.h"

namespace optiforge::ir {
class Function;
}

namespace optiforge::analysis {

/// Makes a loaded profile queryable by any pass (PGO-02).
///
/// Unlike every other analysis, this one is never *computed* — it is loaded
/// from a file by the driver and handed to the manager with `provide`. `run`
/// exists only so the analysis concept is satisfied, and returns an empty
/// profile: asking `get` for one that was never supplied should produce
/// something harmless, not a fabricated answer.
///
/// **Passes must use `getCached`, not `get`.** A null result means no profile
/// was supplied, and that is the fallback path every profile-guided pass is
/// required to have (System_design.md §14.4). Using `get` would silently
/// manufacture an empty profile and hide the distinction between "nothing ran"
/// and "nothing was measured".
struct ProfileAnalysis {
  using Result = profile::ProfileData;
  static const char* name() { return "profile"; }
  static Result run(const ir::Function&, AnalysisManager&) { return Result{}; }
};

}  // namespace optiforge::analysis
