#pragma once

#include <string>

#include "optiforge/support/ProfileLayout.h"

namespace optiforge::ir {
class Module;
}

namespace optiforge::analysis {
class AnalysisManager;
}

namespace optiforge::transforms {

/// Inserts profile counters into every function of `module`.
///
/// Runs **after** the optimization pipeline and after SSA destruction (ADR-05).
/// Instrumenting earlier would let optimizations delete and move counters, and
/// the CFG measured would not be the CFG the profile-guided build later sees.
/// The cost is that the instrumented and PGO builds must use the same `-O`
/// level, which the driver enforces and the profile header records.
///
/// Critical edges are split first. That is what makes a branch's two outcomes
/// countable without counters of their own: afterwards each successor of a
/// conditional branch has exactly one predecessor, so its block counter *is*
/// that edge's count.
///
/// Only one kind of counter is actually emitted -- one per basic block. A
/// function's entry count, a branch's two outcomes, and a loop's entry and
/// iteration counts are all derived from those. That is cheaper at run time
/// than four counter kinds, and it makes the four numbers impossible to get
/// inconsistent with one another.
///
/// `withTiming` additionally wraps each function in calls to the runtime's
/// clock. Off by default and deliberately so: a call in the hot path is the one
/// thing the counter array exists to avoid.
ProfileLayout instrumentForProfiling(ir::Module& module,
                                     analysis::AnalysisManager& manager,
                                     int optLevel, const std::string& compilerVersion,
                                     const std::string& defaultOutputPath,
                                     bool withTiming);

}  // namespace optiforge::transforms
