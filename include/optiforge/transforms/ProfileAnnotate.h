#pragma once

#include <cstddef>

namespace optiforge::ir {
class Module;
}

namespace optiforge::profile {
class ProfileData;
}

namespace optiforge::transforms {

/// Copies measured block counts from a profile onto the IR.
///
/// `BasicBlock::executionCount` has existed since Phase 3 waiting for this. It
/// is how the *backend* sees profile data: register allocation and block layout
/// both need per-block frequencies, and neither has an AnalysisManager to ask.
///
/// Matching is by `function` and block label. Labels are assigned when a block
/// is created and never rewritten, so a block that survives the pipeline carries
/// the same name the instrumented build recorded — which is what makes this work
/// despite ADR-05 putting instrumentation after optimization.
///
/// Call it twice: once before the pipeline, so passes see counts, and again
/// before code generation, so blocks created in between (preheaders, split
/// critical edges) pick up whatever the profile knows about them. A block the
/// profile says nothing about keeps the count it already had, which is how an
/// unrolled copy inherits its original's frequency.
///
/// Returns the number of blocks that were given a count.
std::size_t annotateBlockCounts(ir::Module& module, const profile::ProfileData& profile);

}  // namespace optiforge::transforms
