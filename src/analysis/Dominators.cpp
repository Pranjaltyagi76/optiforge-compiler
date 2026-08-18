#include "optiforge/analysis/Dominators.h"

#include <algorithm>
#include <limits>

#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"

namespace optiforge::analysis {

namespace {
constexpr std::size_t kUndefined = std::numeric_limits<std::size_t>::max();
}

DominatorTree::DominatorTree(const ir::Function& function, bool postDominators)
    : post_(postDominators) {
  if (function.blocks().empty()) {
    return;
  }
  if (postDominators) {
    buildPost(function);
  } else {
    build(function);
  }
  computeIdoms();

  // Materialize the tree in reverse-postorder so children lists, and therefore
  // every dump built from them, are deterministic.
  rpo_.reserve(order_.size());
  for (std::size_t position : order_) {
    rpo_.push_back(blocks_[position]);
  }
  // Walk the block list rather than reverse-postorder so children come out in
  // CFG order. Both are deterministic; this one is readable.
  for (std::size_t position = 0; position < blocks_.size(); ++position) {
    if (position == root_ || idom_[position] == kUndefined) {
      continue;
    }
    children_[blocks_[idom_[position]]].push_back(blocks_[position]);
  }

  numberSubtrees();
}

void DominatorTree::numberSubtrees() {
  enter_.assign(blocks_.size(), 0);
  exit_.assign(blocks_.size(), 0);

  // Iterative to avoid recursing once per tree level: a chain of 3000 ifs
  // gives a dominator tree thousands deep.
  std::size_t clock = 0;
  std::vector<std::pair<std::size_t, bool>> stack{{root_, false}};
  while (!stack.empty()) {
    auto [node, done] = stack.back();
    stack.pop_back();
    if (done) {
      exit_[node] = clock++;
      continue;
    }
    enter_[node] = clock++;
    stack.push_back({node, true});

    const auto it = children_.find(blocks_[node]);
    if (it != children_.end()) {
      for (const ir::BasicBlock* child : it->second) {
        const auto childIt = index_.find(child);
        if (childIt != index_.end()) {
          stack.push_back({childIt->second, false});
        }
      }
    }
  }
}

void DominatorTree::build(const ir::Function& function) {
  // Index every block, then record predecessors as index lists.
  for (const auto& block : function.blocks()) {
    index_.emplace(block.get(), blocks_.size());
    blocks_.push_back(block.get());
  }
  preds_.resize(blocks_.size());
  for (const auto& block : function.blocks()) {
    const std::size_t from = index_.at(block.get());
    for (const ir::BasicBlock* successor : block->successors()) {
      const auto it = index_.find(successor);
      if (it != index_.end()) {
        preds_[it->second].push_back(from);
      }
    }
  }
  root_ = index_.at(function.entry());

  // Reverse postorder of a depth-first search from the root.
  std::vector<bool> visited(blocks_.size(), false);
  std::vector<std::size_t> postorder;
  std::vector<std::pair<std::size_t, std::size_t>> stack{{root_, 0}};
  visited[root_] = true;

  while (!stack.empty()) {
    auto& [node, next] = stack.back();
    const auto successors = blocks_[node]->successors();
    if (next < successors.size()) {
      const std::size_t child = index_.at(successors[next]);
      ++next;
      if (!visited[child]) {
        visited[child] = true;
        stack.push_back({child, 0});
      }
    } else {
      postorder.push_back(node);
      stack.pop_back();
    }
  }

  order_.assign(postorder.rbegin(), postorder.rend());
  rpoNumber_.assign(blocks_.size(), kUndefined);
  for (std::size_t i = 0; i < order_.size(); ++i) {
    rpoNumber_[order_[i]] = i;
  }
}

void DominatorTree::buildPost(const ir::Function& function) {
  // The post-dominator tree is the dominator tree of the reversed CFG. A
  // function may have several returns, so a virtual exit node is added as the
  // single root; without it, blocks ending in different returns would have no
  // common post-dominator.
  for (const auto& block : function.blocks()) {
    index_.emplace(block.get(), blocks_.size());
    blocks_.push_back(block.get());
  }
  const std::size_t virtualExit = blocks_.size();
  blocks_.push_back(nullptr);  // the virtual exit
  root_ = virtualExit;

  preds_.resize(blocks_.size());
  // In the reversed graph, a block's predecessors are its CFG successors.
  std::vector<std::vector<std::size_t>> reverseSuccessors(blocks_.size());
  for (const auto& block : function.blocks()) {
    const std::size_t node = index_.at(block.get());
    const auto successors = block->successors();
    if (successors.empty()) {
      // A return block: its only reversed successor is the virtual exit.
      reverseSuccessors[virtualExit].push_back(node);
      preds_[node].push_back(virtualExit);
      continue;
    }
    for (const ir::BasicBlock* successor : successors) {
      const std::size_t target = index_.at(successor);
      reverseSuccessors[target].push_back(node);
      preds_[node].push_back(target);
    }
  }

  std::vector<bool> visited(blocks_.size(), false);
  std::vector<std::size_t> postorder;
  std::vector<std::pair<std::size_t, std::size_t>> stack{{root_, 0}};
  visited[root_] = true;

  while (!stack.empty()) {
    auto& [node, next] = stack.back();
    if (next < reverseSuccessors[node].size()) {
      const std::size_t child = reverseSuccessors[node][next];
      ++next;
      if (!visited[child]) {
        visited[child] = true;
        stack.push_back({child, 0});
      }
    } else {
      postorder.push_back(node);
      stack.pop_back();
    }
  }

  order_.assign(postorder.rbegin(), postorder.rend());
  rpoNumber_.assign(blocks_.size(), kUndefined);
  for (std::size_t i = 0; i < order_.size(); ++i) {
    rpoNumber_[order_[i]] = i;
  }
}

std::size_t DominatorTree::intersect(std::size_t a, std::size_t b) const {
  // Walk both up the partially built tree until they meet. Comparing
  // reverse-postorder numbers is what makes this terminate quickly.
  while (a != b) {
    while (rpoNumber_[a] > rpoNumber_[b]) {
      a = idom_[a];
    }
    while (rpoNumber_[b] > rpoNumber_[a]) {
      b = idom_[b];
    }
  }
  return a;
}

void DominatorTree::computeIdoms() {
  idom_.assign(blocks_.size(), kUndefined);
  idom_[root_] = root_;

  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t node : order_) {
      if (node == root_) {
        continue;
      }

      std::size_t newIdom = kUndefined;
      for (std::size_t predecessor : preds_[node]) {
        if (idom_[predecessor] == kUndefined) {
          continue;  // not processed yet on this iteration
        }
        newIdom = (newIdom == kUndefined) ? predecessor : intersect(predecessor, newIdom);
      }

      if (newIdom != kUndefined && idom_[node] != newIdom) {
        idom_[node] = newIdom;
        changed = true;
      }
    }
  }
}

const ir::BasicBlock* DominatorTree::immediateDominator(const ir::BasicBlock* block) const {
  const auto it = index_.find(block);
  if (it == index_.end() || it->second == root_) {
    return nullptr;
  }
  const std::size_t parent = idom_[it->second];
  if (parent == kUndefined) {
    return nullptr;
  }
  return blocks_[parent];  // may be null for the virtual exit
}

bool DominatorTree::dominates(const ir::BasicBlock* a, const ir::BasicBlock* b) const {
  const auto ita = index_.find(a);
  const auto itb = index_.find(b);
  if (ita == index_.end() || itb == index_.end()) {
    return false;
  }
  if (enter_.empty()) {
    return false;
  }
  // b is in a's subtree exactly when its preorder number falls inside the
  // interval a spans.
  return enter_[ita->second] <= enter_[itb->second] &&
         exit_[itb->second] <= exit_[ita->second];
}

const std::vector<const ir::BasicBlock*>& DominatorTree::children(
    const ir::BasicBlock* block) const {
  const auto it = children_.find(block);
  return it == children_.end() ? empty_ : it->second;
}

std::vector<const ir::BasicBlock*> DominatorTree::preorder() const {
  std::vector<const ir::BasicBlock*> result;
  if (blocks_.empty()) {
    return result;
  }

  const ir::BasicBlock* rootBlock = blocks_[root_];
  std::vector<const ir::BasicBlock*> stack{rootBlock};
  while (!stack.empty()) {
    const ir::BasicBlock* node = stack.back();
    stack.pop_back();
    if (node != nullptr) {
      result.push_back(node);
    }
    const std::vector<const ir::BasicBlock*>& kids = children(node);
    // Pushed in reverse so they pop in order.
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
      stack.push_back(*it);
    }
  }
  return result;
}

// ---------------------------------------------------------------------------
// Dominance frontier
// ---------------------------------------------------------------------------

DominanceFrontier::DominanceFrontier(const ir::Function& function,
                                     const DominatorTree& domtree) {
  // Cytron et al.: for every join point, walk up from each predecessor until
  // the join's immediate dominator is reached, adding the join to each block
  // passed along the way.
  for (const auto& block : function.blocks()) {
    if (block->predecessors().size() < 2) {
      continue;
    }
    const ir::BasicBlock* stop = domtree.immediateDominator(block.get());
    for (const ir::BasicBlock* predecessor : block->predecessors()) {
      const ir::BasicBlock* runner = predecessor;
      while (runner != nullptr && runner != stop) {
        std::vector<const ir::BasicBlock*>& set = frontier_[runner];
        if (std::find(set.begin(), set.end(), block.get()) == set.end()) {
          set.push_back(block.get());
        }
        runner = domtree.immediateDominator(runner);
      }
    }
  }
}

const std::vector<const ir::BasicBlock*>& DominanceFrontier::frontierOf(
    const ir::BasicBlock* block) const {
  const auto it = frontier_.find(block);
  return it == frontier_.end() ? empty_ : it->second;
}

DominanceFrontier DominanceFrontierAnalysis::run(const ir::Function& function,
                                                 AnalysisManager& manager) {
  return DominanceFrontier(function, manager.get<DominatorTreeAnalysis>(function));
}

}  // namespace optiforge::analysis
