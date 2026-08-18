#pragma once

#include <iosfwd>

namespace optiforge::ir {
class Module;
}

namespace optiforge::analysis {

/// Dumps every analysis for every function, for `--emit=analysis`.
///
/// The exit criterion for Phase 5 is golden dumps of dominator trees, loop
/// nests and live sets on hand-checked CFGs, and this is what produces them.
/// Deterministic by construction: every set is emitted in a fixed order.
void printAnalyses(const ir::Module& module, std::ostream& out);

}  // namespace optiforge::analysis
