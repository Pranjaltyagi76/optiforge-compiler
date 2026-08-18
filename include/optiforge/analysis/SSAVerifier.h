#pragma once

#include <string>
#include <vector>

namespace optiforge::ir {
class Function;
class Module;
}  // namespace optiforge::ir

namespace optiforge::analysis {

class AnalysisManager;

/// Checks the invariants that make SSA form meaningful.
///
/// Separate from ir::Verifier because it needs dominator information, and
/// of_ir may not depend on of_analysis (architectural_design.md section 3).
/// The structural verifier still runs on its own; this adds:
///
///   - every use is dominated by its definition, so a value is never read on
///     a path where it was not computed;
///   - a phi has exactly one incoming edge per predecessor, and they name the
///     right blocks;
///   - a phi operand is available at the end of the predecessor it arrives
///     from, rather than at the phi's own block.
///
/// Valid only *before* SSA destruction: destruction deliberately introduces
/// copies on back edges that do not dominate their uses, which is exactly why
/// it is the last thing to run.
std::vector<std::string> verifySSA(const ir::Function& function,
                                   AnalysisManager& manager);

std::vector<std::string> verifySSA(const ir::Module& module, AnalysisManager& manager);

}  // namespace optiforge::analysis
