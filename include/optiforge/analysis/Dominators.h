#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "optiforge/analysis/AnalysisManager.h"

namespace optiforge::ir {
class BasicBlock;
class Function;
}  // namespace optiforge::ir

namespace optiforge::analysis {

/// Immediate-dominator tree over a function's CFG.
///
/// Computed with the iterative Cooper-Harvey-Kennedy algorithm rather than
/// Lengauer-Tarjan. It is a fraction of the code, easy to check by hand, and
/// near-linear in practice on CFGs of the size this compiler sees. Should a
/// profile (metric P-04) ever show dominators dominating compile time, the
/// asymptotically better algorithm can replace it behind this same interface.
class DominatorTree {
public:
  DominatorTree() = default;
  DominatorTree(const ir::Function& function, bool postDominators);

  /// Immediate dominator of `block`, or null for the root.
  const ir::BasicBlock* immediateDominator(const ir::BasicBlock* block) const;

  /// True when `a` dominates `b`. Every block dominates itself.
  ///
  /// O(1): each node carries the interval of preorder numbers spanned by its
  /// subtree, and `a` dominates `b` exactly when b's number lies inside a's
  /// interval. Walking the idom chain instead was O(tree depth), which turned
  /// loop detection into O(edges x depth) -- 59 seconds on a 9000-block
  /// function, against an NFR-01 budget of 2 seconds for a whole compilation.
  bool dominates(const ir::BasicBlock* a, const ir::BasicBlock* b) const;

  /// True when `a` strictly dominates `b`.
  bool strictlyDominates(const ir::BasicBlock* a, const ir::BasicBlock* b) const {
    return a != b && dominates(a, b);
  }

  /// Children in the dominator tree, in reverse-postorder for determinism.
  const std::vector<const ir::BasicBlock*>& children(const ir::BasicBlock* block) const;

  /// Blocks in reverse postorder. Also the order most dataflow analyses want.
  const std::vector<const ir::BasicBlock*>& reversePostorder() const { return rpo_; }

  /// Preorder walk of the dominator tree, which is the order SSA renaming and
  /// dominator-scoped value numbering need.
  std::vector<const ir::BasicBlock*> preorder() const;

  bool isPostDominatorTree() const { return post_; }

private:
  void build(const ir::Function& function);
  void buildPost(const ir::Function& function);
  void computeIdoms();
  void numberSubtrees();
  std::size_t intersect(std::size_t a, std::size_t b) const;

  bool post_ = false;
  std::vector<const ir::BasicBlock*> blocks_;   // index -> block
  std::unordered_map<const ir::BasicBlock*, std::size_t> index_;
  std::vector<std::vector<std::size_t>> preds_; // edges in traversal direction
  std::vector<std::size_t> rpoNumber_;          // index -> position in rpo_
  std::vector<std::size_t> order_;              // rpo position -> index
  std::vector<std::size_t> idom_;               // index -> index, self for root
  std::size_t root_ = 0;

  // Subtree interval per node, indexed like blocks_.
  std::vector<std::size_t> enter_;
  std::vector<std::size_t> exit_;

  std::vector<const ir::BasicBlock*> rpo_;
  std::unordered_map<const ir::BasicBlock*, std::vector<const ir::BasicBlock*>> children_;
  std::vector<const ir::BasicBlock*> empty_;
};

/// Analysis wrapper: the dominator tree of a function.
struct DominatorTreeAnalysis {
  using Result = DominatorTree;
  static const char* name() { return "dominator-tree"; }
  static Result run(const ir::Function& function, AnalysisManager&) {
    return DominatorTree(function, /*postDominators=*/false);
  }
};

/// Analysis wrapper: the post-dominator tree.
struct PostDominatorTreeAnalysis {
  using Result = DominatorTree;
  static const char* name() { return "post-dominator-tree"; }
  static Result run(const ir::Function& function, AnalysisManager&) {
    return DominatorTree(function, /*postDominators=*/true);
  }
};

/// For each block, the set of blocks where its dominance stops.
///
/// This is the input phi placement needs: a definition in block B needs a phi
/// exactly at the blocks in B's dominance frontier (Cytron et al.), which is
/// what Phase 6 will use.
class DominanceFrontier {
public:
  DominanceFrontier() = default;
  DominanceFrontier(const ir::Function& function, const DominatorTree& domtree);

  const std::vector<const ir::BasicBlock*>& frontierOf(const ir::BasicBlock* block) const;

private:
  std::unordered_map<const ir::BasicBlock*, std::vector<const ir::BasicBlock*>> frontier_;
  std::vector<const ir::BasicBlock*> empty_;
};

struct DominanceFrontierAnalysis {
  using Result = DominanceFrontier;
  static const char* name() { return "dominance-frontier"; }
  static Result run(const ir::Function& function, AnalysisManager& manager);
};

}  // namespace optiforge::analysis
