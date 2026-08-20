#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "optiforge/backend/MachineIR.h"
#include "optiforge/backend/TargetInfo.h"

namespace optiforge::ir {
class Function;
class Value;
}  // namespace optiforge::ir

namespace optiforge::analysis {
class AnalysisManager;
}

namespace optiforge::backend {

/// Which allocator the backend should use (ADR-08).
enum class RegAllocKind {
  /// Phase 4: every value lives in a frame slot. Kept forever, because when
  /// the graph allocator miscompiles, one flag says whether it is to blame.
  Naive,
  /// Phase 8: Chaitin-Briggs graph colouring.
  Graph,
};

/// The result of allocating one function.
///
/// A value present in `assigned` lives in that register for its whole live
/// range. A value *absent* lives in a frame slot and is loaded and stored
/// around each use -- which is precisely what the naive allocator does for
/// everything, and is why spilling here needs no rewrite pass and no second
/// colouring round. See `allocateRegisters` for why that is sound.
struct RegisterAssignment {
  /// The function this describes, so `--print-regalloc` can name it.
  std::string function;

  std::unordered_map<const ir::Value*, MReg> assigned;

  /// Callee-saved registers the assignment actually uses, in the order the
  /// prologue should save them. Empty when nothing callee-saved was needed,
  /// which keeps leaf functions as cheap as they were in Phase 4.
  std::vector<MReg> usedCalleeSaved;

  // --- Statistics, for the metrics table and for tests that would otherwise
  // --- have to assert on generated assembly.
  std::size_t candidates = 0;   ///< allocation units considered
  std::size_t colored = 0;      ///< units that got a register
  std::size_t spilled = 0;      ///< units left in memory
  std::size_t coalesced = 0;    ///< copies removed by merging their two ends
  std::size_t frozen = 0;       ///< copies given up on so their nodes could simplify
  std::size_t maxPressure = 0;  ///< most units live at any one point

  /// One line: what the allocator did to this function. The raw material for
  /// metric BE-04's quality table, and the first thing to look at when the
  /// generated code is slower than expected.
  std::string summary() const;

  bool registerFor(const ir::Value* value, MReg& out) const {
    const auto it = assigned.find(value);
    if (it == assigned.end()) {
      return false;
    }
    out = it->second;
    return true;
  }
};

/// Allocates registers for one function by graph colouring.
///
/// Chaitin-Briggs, in the order System_design.md section 12.2 lays out: build
/// the interference graph from liveness, coalesce move-related nodes under
/// Briggs' conservative test, simplify, freeze, spill optimistically, select.
///
/// Two things make this smaller than the textbook version, both deliberate:
///
///   - **Spilling needs no rewrite round.** The code generator already knows
///     how to compute with a value that lives in memory -- that is all the
///     naive allocator ever did. So "spill" here means only "assign no
///     register", and the reload code it implies uses the reserved scratch
///     registers, which are not allocatable and so add no live range the graph
///     would have to know about. The usual build/spill/rebuild loop exists to
///     account for those new ranges; with none to account for, one pass is
///     exact.
///
///   - **No pre-colouring.** `idiv`, variable shifts, calls and returns all
///     demand specific registers. Rather than model that with pre-coloured
///     nodes, the target simply keeps those registers out of the allocatable
///     pool (see `TargetInfo::allocatableIntRegisters`). Fewer registers, far
///     fewer ways to be wrong.
///
/// Values coalesced onto a common frame slot by SSA destruction -- every copy
/// belonging to one phi -- are merged into a single allocation unit before the
/// graph is built. They must share a location, so they must share a register.
///
/// `useProfileWeights` is how `--disable-pgo=regalloc` is expressed (G-05,
/// G-08). False makes spill cost fall back to 10^loopDepth for every block even
/// when the IR carries measured counts -- which is what an unprofiled build
/// does, so the two builds differ in this decision and nothing else.
RegisterAssignment allocateRegisters(const ir::Function& function,
                                     analysis::AnalysisManager& manager,
                                     const TargetInfo& target,
                                     bool useProfileWeights = true);

/// Recomputes liveness and checks the assignment against it.
///
/// Returns one message per violation, empty when the assignment is sound. Two
/// values live at the same point must not share a register, and a value live
/// across a call must not sit in a caller-saved one. This is the
/// register-pressure verification the roadmap asks Phase 8 for: cheap enough
/// to run on every compilation, and the difference between a miscompile that
/// is reported and one that is shipped.
std::vector<std::string> verifyAssignment(const ir::Function& function,
                                          analysis::AnalysisManager& manager,
                                          const TargetInfo& target,
                                          const RegisterAssignment& assignment);

}  // namespace optiforge::backend
