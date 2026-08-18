#pragma once

#include <cstdint>
#include <memory>
#include <unordered_set>
#include <vector>

#include "optiforge/analysis/AnalysisManager.h"

namespace optiforge::ir {
class BasicBlock;
class Function;
}  // namespace optiforge::ir

namespace optiforge::analysis {

/// One natural loop.
class Loop {
public:
  /// The single entry point. Every path into the loop passes through it.
  const ir::BasicBlock* header() const { return header_; }

  /// Blocks with a back edge to the header. More than one means the loop has
  /// several tails.
  const std::vector<const ir::BasicBlock*>& latches() const { return latches_; }

  /// Every block in the loop, header included, in CFG order.
  const std::vector<const ir::BasicBlock*>& blocks() const { return blocks_; }

  /// Blocks outside the loop that a loop block branches to.
  const std::vector<const ir::BasicBlock*>& exits() const { return exits_; }

  /// Unique predecessor of the header from outside the loop, or null when
  /// there are several. LICM hoists into the preheader, so a loop without one
  /// has no unambiguous place to put invariant code.
  const ir::BasicBlock* preheader() const { return preheader_; }

  const Loop* parent() const { return parent_; }
  const std::vector<Loop*>& subLoops() const { return subLoops_; }

  /// 1 for an outermost loop.
  unsigned depth() const { return depth_; }

  bool contains(const ir::BasicBlock* block) const;

  // --- Filled from a profile in Phase 10; zero means "unknown" ---
  std::uint64_t entryCount = 0;
  std::uint64_t iterationCount = 0;

  /// Average iterations per entry, which is what drives the PGO unroller
  /// (metric G-07). Zero when no profile has been loaded.
  double averageTripCount() const {
    return entryCount == 0 ? 0.0 : static_cast<double>(iterationCount) /
                                       static_cast<double>(entryCount);
  }

private:
  friend class LoopInfo;

  const ir::BasicBlock* header_ = nullptr;
  const ir::BasicBlock* preheader_ = nullptr;
  std::vector<const ir::BasicBlock*> latches_;
  std::vector<const ir::BasicBlock*> blocks_;
  // Membership is queried once per loop pair while nesting, and once per block
  // by loopFor; a linear scan there made both quadratic.
  std::unordered_set<const ir::BasicBlock*> blockSet_;
  std::vector<const ir::BasicBlock*> exits_;
  Loop* parent_ = nullptr;
  std::vector<Loop*> subLoops_;
  unsigned depth_ = 1;
};

/// Natural loops of a function, nested.
///
/// A back edge is an edge n -> h where h dominates n; the natural loop of that
/// edge is h plus every block that reaches n without passing through h. This
/// is precisely the shape IRGen emits for `while`, which is why getting the
/// lowering right in Phase 3 mattered well beyond Phase 3.
class LoopInfo {
public:
  LoopInfo() = default;
  LoopInfo(const ir::Function& function, const class DominatorTree& domtree);

  /// Outermost loops, in header order.
  const std::vector<Loop*>& topLevelLoops() const { return topLevel_; }

  /// Every loop, outermost first.
  const std::vector<std::unique_ptr<Loop>>& allLoops() const { return loops_; }

  /// Innermost loop containing `block`, or null.
  const Loop* loopFor(const ir::BasicBlock* block) const;

  /// Nesting depth of `block`: 0 outside any loop.
  unsigned depthOf(const ir::BasicBlock* block) const;

  bool empty() const { return loops_.empty(); }

private:
  std::vector<std::unique_ptr<Loop>> loops_;
  std::vector<Loop*> topLevel_;
};

struct LoopAnalysis {
  using Result = LoopInfo;
  static const char* name() { return "loop-info"; }
  static Result run(const ir::Function& function, AnalysisManager& manager);
};

/// Ensures every loop header has a single outside predecessor, creating one
/// where it does not.
///
/// This **mutates the IR** and is therefore not an analysis: rule 4 in
/// architectural_design.md section 3 says analyses are read-only. It lives
/// beside LoopInfo because that is where it is understood, but callers must
/// treat it as a transform and invalidate analyses afterwards.
///
/// Returns the number of preheaders inserted.
std::size_t insertLoopPreheaders(ir::Function& function);

}  // namespace optiforge::analysis
