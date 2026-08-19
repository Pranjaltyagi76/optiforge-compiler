#include <memory>
#include <unordered_set>
#include <vector>

#include "optiforge/analysis/AnalysisManager.h"
#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Instruction.h"
#include "optiforge/passes/Pass.h"
#include "optiforge/transforms/SSA.h"

namespace optiforge::transforms {

namespace {

using passes::Pass;
using passes::PassRegistration;

/// Tidies the control-flow graph.
///
/// Pairs with SCCP: proving a condition constant is only half the win, and the
/// dead arm stays in the function until the branch is folded and the blocks it
/// alone reached are pruned.
class SimplifyCFG final : public Pass {
public:
  std::string_view name() const override { return "simplify-cfg"; }
  std::string_view description() const override {
    return "fold constant branches and remove unreachable blocks";
  }

  bool run(ir::Function& function, analysis::AnalysisManager&) override {
    bool changed = foldConstantBranches(function);
    changed |= prune(function);
    // After pruning, a block may be left as the only predecessor of its only
    // successor, which is the shape merging removes.
    changed |= mergeIntoPredecessors(function);
    return changed;
  }

private:
  /// `condbr true, a, b` is just `br a`.
  static bool foldConstantBranches(ir::Function& function) {
    bool changed = false;

    for (const auto& block : function.blocks()) {
      ir::Instruction* terminator = block->terminator();
      if (terminator == nullptr || terminator->opcode() != ir::Opcode::CondBr) {
        continue;
      }
      const ir::Value* condition = terminator->operand(0);
      if (condition->valueKind() != ir::Value::Kind::ConstantBool) {
        continue;
      }

      const bool taken = static_cast<const ir::ConstantBool*>(condition)->value();
      ir::BasicBlock* target = terminator->successors()[taken ? 0 : 1];
      ir::BasicBlock* dropped = terminator->successors()[taken ? 1 : 0];

      // The block no longer reaches `dropped`, so any phi there must lose the
      // corresponding incoming edge or its arity stops matching. When both arms
      // lead to the same block the edge count drops from two to one, so exactly
      // one entry goes -- removing every entry for this predecessor would leave
      // the phi with no value on the edge that survives.
      for (const auto& instruction : dropped->instructions()) {
        if (instruction->opcode() != ir::Opcode::Phi) {
          break;
        }
        if (dropped == target) {
          instruction->removeOneIncoming(block.get());
        } else {
          instruction->removeIncoming(block.get());
        }
      }

      auto branch =
          std::make_unique<ir::Instruction>(ir::Opcode::Br, ir::Type::getVoid());
      branch->addSuccessor(target);
      terminator->eraseFromParent();
      block->append(std::move(branch));
      changed = true;
    }

    if (changed) {
      function.recomputePredecessors();
    }
    return changed;
  }

  static bool prune(ir::Function& function) {
    return removeUnreachableBlocks(function) != 0;
  }

  /// Folds a block into its predecessor when control can only ever arrive one
  /// way.
  ///
  /// `A: br B` where B has no other predecessor is a block boundary that means
  /// nothing: the two always run together. Removing it shortens every later
  /// walk over the CFG, removes a jump from the emitted code, and -- the reason
  /// it matters most from Phase 8 on -- gives the register allocator one longer
  /// straight-line region instead of two short ones.
  static bool mergeIntoPredecessors(ir::Function& function) {
    bool changed = false;

    // One merge at a time: it removes a block, so the list being walked
    // changes underneath. Restarting is cheap at the scale this compiler works
    // at, and it keeps the predecessor lists trustworthy at every step.
    bool progress = true;
    while (progress) {
      progress = false;
      function.recomputePredecessors();

      for (const auto& owner : function.blocks()) {
        ir::BasicBlock* block = owner.get();
        ir::Instruction* terminator = block->terminator();
        if (terminator == nullptr || terminator->opcode() != ir::Opcode::Br) {
          continue;
        }

        ir::BasicBlock* successor = terminator->successors()[0];
        if (successor == block || successor == function.entry() ||
            successor->predecessors().size() != 1 || successor->empty()) {
          continue;
        }

        merge(function, *block, *successor);
        progress = true;
        changed = true;
        break;
      }
    }

    return changed;
  }

  /// Moves everything in `successor` into `block` and drops the now-empty
  /// block. `successor` must have `block` as its only predecessor.
  static void merge(ir::Function& function, ir::BasicBlock& block,
                    ir::BasicBlock& successor) {
    // A phi with a single predecessor is not a choice; it is its own operand.
    while (!successor.instructions().empty() &&
           successor.instructions().front()->opcode() == ir::Opcode::Phi) {
      ir::Instruction* phi = successor.instructions().front().get();
      if (phi->operandCount() == 1) {
        phi->replaceAllUsesWith(phi->operand(0));
      }
      phi->eraseFromParent();
    }

    // The jump between the two is what disappears.
    block.terminator()->eraseFromParent();

    std::vector<ir::Instruction*> moving;
    moving.reserve(successor.instructions().size());
    for (const auto& instruction : successor.instructions()) {
      moving.push_back(instruction.get());
    }
    for (ir::Instruction* instruction : moving) {
      // insertBefore(.., nullptr) appends without touching CFG edges; the
      // predecessor lists are rebuilt below, once, from the terminators.
      block.insertBefore(successor.detach(instruction), nullptr);
    }

    // Phis further downstream named the block that just vanished.
    for (ir::BasicBlock* downstream : block.successors()) {
      for (const auto& instruction : downstream->instructions()) {
        if (instruction->opcode() != ir::Opcode::Phi) {
          break;
        }
        for (std::size_t i = 0; i < instruction->incomingCount(); ++i) {
          if (instruction->incomingBlock(i) == &successor) {
            instruction->setSuccessor(i, &block);
          }
        }
      }
    }

    function.recomputePredecessors();
    function.pruneUnreachableBlocks();  // the emptied block is now unreachable
  }
};

std::unique_ptr<Pass> makeSimplifyCFG() { return std::make_unique<SimplifyCFG>(); }
const PassRegistration kSimplify{"simplify-cfg", makeSimplifyCFG};

}  // namespace

std::size_t removeUnreachableBlocks(ir::Function& function) {
  if (function.blocks().empty()) {
    return 0;
  }

  std::unordered_set<const ir::BasicBlock*> reachable;
  std::vector<ir::BasicBlock*> worklist{function.entry()};
  reachable.insert(function.entry());
  while (!worklist.empty()) {
    ir::BasicBlock* block = worklist.back();
    worklist.pop_back();
    for (ir::BasicBlock* successor : block->successors()) {
      if (reachable.insert(successor).second) {
        worklist.push_back(successor);
      }
    }
  }

  if (reachable.size() == function.blocks().size()) {
    return 0;
  }

  // Detach the dying blocks from surviving phis first. pruneUnreachableBlocks
  // drops operand references, but a phi in a live block would still name the
  // dead predecessor and its arity would stop matching.
  for (const auto& block : function.blocks()) {
    if (reachable.count(block.get()) == 0) {
      continue;
    }
    for (const auto& instruction : block->instructions()) {
      if (instruction->opcode() != ir::Opcode::Phi) {
        break;
      }
      std::vector<const ir::BasicBlock*> gone;
      for (std::size_t i = 0; i < instruction->incomingCount(); ++i) {
        if (reachable.count(instruction->incomingBlock(i)) == 0) {
          gone.push_back(instruction->incomingBlock(i));
        }
      }
      for (const ir::BasicBlock* dead : gone) {
        instruction->removeIncoming(dead);
      }
    }
  }

  return function.pruneUnreachableBlocks();
}

void anchorSimplifyCFG() {}

}  // namespace optiforge::transforms
