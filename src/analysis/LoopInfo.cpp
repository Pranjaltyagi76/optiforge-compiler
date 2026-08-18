#include "optiforge/analysis/LoopInfo.h"

#include <algorithm>
#include <unordered_set>

#include "optiforge/analysis/Dominators.h"
#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Instruction.h"

namespace optiforge::analysis {

bool Loop::contains(const ir::BasicBlock* block) const {
  return blockSet_.count(block) != 0;
}

LoopInfo::LoopInfo(const ir::Function& function, const DominatorTree& domtree) {
  if (function.blocks().empty()) {
    return;
  }

  // --- Find back edges: n -> h where h dominates n ---
  // Collected per header so two tails of the same loop merge into one loop
  // rather than producing two overlapping ones.
  std::vector<const ir::BasicBlock*> headers;
  std::vector<std::vector<const ir::BasicBlock*>> latchesFor;

  for (const auto& block : function.blocks()) {
    for (const ir::BasicBlock* successor : block->successors()) {
      if (!domtree.dominates(successor, block.get())) {
        continue;
      }
      const auto it = std::find(headers.begin(), headers.end(), successor);
      if (it == headers.end()) {
        headers.push_back(successor);
        latchesFor.push_back({block.get()});
      } else {
        latchesFor[static_cast<std::size_t>(it - headers.begin())].push_back(block.get());
      }
    }
  }

  if (headers.empty()) {
    return;
  }

  // --- Body of each loop: blocks reaching a latch without passing the header ---
  for (std::size_t i = 0; i < headers.size(); ++i) {
    auto loop = std::make_unique<Loop>();
    loop->header_ = headers[i];
    loop->latches_ = latchesFor[i];

    std::unordered_set<const ir::BasicBlock*> body{headers[i]};
    std::vector<const ir::BasicBlock*> worklist;
    for (const ir::BasicBlock* latch : latchesFor[i]) {
      if (body.insert(latch).second) {
        worklist.push_back(latch);
      }
    }
    while (!worklist.empty()) {
      const ir::BasicBlock* node = worklist.back();
      worklist.pop_back();
      for (const ir::BasicBlock* predecessor : node->predecessors()) {
        if (body.insert(predecessor).second) {
          worklist.push_back(predecessor);
        }
      }
    }

    // Store in CFG order so dumps are deterministic.
    for (const auto& block : function.blocks()) {
      if (body.count(block.get()) != 0) {
        loop->blocks_.push_back(block.get());
      }
    }
    loop->blockSet_ = std::move(body);

    // Exits: successors outside the loop.
    for (const ir::BasicBlock* member : loop->blocks_) {
      for (const ir::BasicBlock* successor : member->successors()) {
        if (loop->blockSet_.count(successor) == 0 &&
            std::find(loop->exits_.begin(), loop->exits_.end(), successor) ==
                loop->exits_.end()) {
          loop->exits_.push_back(successor);
        }
      }
    }

    // Preheader: the single predecessor of the header from outside the loop.
    const ir::BasicBlock* outside = nullptr;
    bool unique = true;
    for (const ir::BasicBlock* predecessor : loop->header_->predecessors()) {
      if (loop->blockSet_.count(predecessor) != 0) {
        continue;  // a latch
      }
      if (outside == nullptr) {
        outside = predecessor;
      } else {
        unique = false;
      }
    }
    // A preheader must also branch nowhere else, or hoisting into it would
    // execute the hoisted code on a path that never enters the loop.
    if (unique && outside != nullptr && outside->successors().size() == 1) {
      loop->preheader_ = outside;
    }

    loops_.push_back(std::move(loop));
  }

  // --- Nest by containment ---
  // A loop's parent is the smallest other loop that contains its header.
  for (std::size_t i = 0; i < loops_.size(); ++i) {
    Loop* inner = loops_[i].get();
    Loop* best = nullptr;
    for (std::size_t j = 0; j < loops_.size(); ++j) {
      if (i == j) {
        continue;
      }
      Loop* outer = loops_[j].get();
      if (!outer->contains(inner->header_)) {
        continue;
      }
      if (best == nullptr || best->blocks_.size() > outer->blocks_.size()) {
        best = outer;
      }
    }
    inner->parent_ = best;
  }

  for (const auto& loop : loops_) {
    if (loop->parent_ != nullptr) {
      loop->parent_->subLoops_.push_back(loop.get());
    } else {
      topLevel_.push_back(loop.get());
    }
  }

  // Depth follows from the parent chain.
  for (const auto& loop : loops_) {
    unsigned depth = 1;
    for (const Loop* parent = loop->parent_; parent != nullptr; parent = parent->parent_) {
      ++depth;
    }
    loop->depth_ = depth;
  }
}

const Loop* LoopInfo::loopFor(const ir::BasicBlock* block) const {
  const Loop* best = nullptr;
  for (const auto& loop : loops_) {
    if (!loop->contains(block)) {
      continue;
    }
    // Innermost wins, and the innermost is the deepest.
    if (best == nullptr || loop->depth() > best->depth()) {
      best = loop.get();
    }
  }
  return best;
}

unsigned LoopInfo::depthOf(const ir::BasicBlock* block) const {
  const Loop* loop = loopFor(block);
  return loop == nullptr ? 0 : loop->depth();
}

LoopInfo LoopAnalysis::run(const ir::Function& function, AnalysisManager& manager) {
  return LoopInfo(function, manager.get<DominatorTreeAnalysis>(function));
}

// ---------------------------------------------------------------------------
// Preheader insertion (a transform, not an analysis)
// ---------------------------------------------------------------------------

std::size_t insertLoopPreheaders(ir::Function& function) {
  if (function.blocks().empty()) {
    return 0;
  }

  std::size_t inserted = 0;

  // Recompute after each insertion: adding a block changes the CFG, and
  // reasoning about a stale loop structure is exactly the kind of bug the
  // analysis/transform split exists to prevent.
  bool changed = true;
  while (changed) {
    changed = false;

    const DominatorTree domtree(function, /*postDominators=*/false);
    const LoopInfo loops(function, domtree);

    for (const auto& loop : loops.allLoops()) {
      if (loop->preheader() != nullptr) {
        continue;
      }

      auto* header = const_cast<ir::BasicBlock*>(loop->header());
      std::vector<ir::BasicBlock*> outside;
      for (const ir::BasicBlock* predecessor : header->predecessors()) {
        if (!loop->contains(predecessor)) {
          outside.push_back(const_cast<ir::BasicBlock*>(predecessor));
        }
      }
      if (outside.empty()) {
        continue;  // unreachable loop; nothing sensible to insert
      }

      ir::BasicBlock* preheader = function.createBlock("preheader");
      auto branch = std::make_unique<ir::Instruction>(ir::Opcode::Br,
                                                      ir::Type::getVoid());
      branch->addSuccessor(header);
      preheader->append(std::move(branch));

      // Redirect every outside edge through the new block.
      for (ir::BasicBlock* predecessor : outside) {
        ir::Instruction* terminator = predecessor->terminator();
        if (terminator == nullptr) {
          continue;
        }
        for (std::size_t i = 0; i < terminator->successors().size(); ++i) {
          if (terminator->successors()[i] == header) {
            terminator->setSuccessor(i, preheader);
          }
        }
      }

      function.recomputePredecessors();
      ++inserted;
      changed = true;
      break;  // restart with fresh analyses
    }
  }

  return inserted;
}

}  // namespace optiforge::analysis
