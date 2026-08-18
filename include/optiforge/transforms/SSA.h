#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

namespace optiforge::ir {
class Function;
class Module;
}  // namespace optiforge::ir

namespace optiforge::analysis {
class AnalysisManager;
}

namespace optiforge::transforms {

/// Promotes stack slots to SSA values.
///
/// IRGen deliberately emits every local as an alloca with loads and stores
/// (ADR-02): lowering straight to SSA would need dominance frontiers, which
/// need a CFG, which needs lowering. Promoting afterwards breaks that cycle,
/// and it is how LLVM does it.
///
/// Only allocas in the entry block whose every use is a direct load or store
/// are promoted. An alloca whose address is used any other way has escaped and
/// must stay in memory.
///
/// Returns the number of slots promoted.
std::size_t promoteMemoryToRegisters(ir::Function& function,
                                     analysis::AnalysisManager& manager);

std::size_t promoteMemoryToRegisters(ir::Module& module,
                                     analysis::AnalysisManager& manager);

/// Splits every critical edge -- an edge from a block with several successors
/// to a block with several predecessors.
///
/// SSA destruction places a copy on each incoming edge of a phi. On a critical
/// edge there is nowhere correct to put it: the source block's other successors
/// would also execute the copy, and the target's other predecessors would skip
/// it. Splitting gives every such edge a block of its own.
///
/// Returns the number of edges split.
std::size_t splitCriticalEdges(ir::Function& function);

/// Replaces phi nodes with copies, leaving IR a code generator can consume.
///
/// Runs immediately before code generation. Two hazards are handled explicitly
/// because both silently corrupt values otherwise:
///
///   - **Critical edges**, resolved by splitting them first.
///   - **The swap problem.** The phis at the top of a block take effect
///     simultaneously, so `a = phi(b), b = phi(a)` exchanges the two. Emitting
///     the copies one after another would instead assign both the same value.
///     Each block's phi group is therefore treated as one parallel copy and
///     sequentialized, breaking cycles with a temporary.
///
/// Returns the number of phi nodes removed.
std::size_t destroySSA(ir::Function& function);
std::size_t destroySSA(ir::Module& module);

}  // namespace optiforge::transforms
