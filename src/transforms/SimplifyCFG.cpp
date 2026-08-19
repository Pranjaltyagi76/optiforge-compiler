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
      // corresponding incoming edge or its arity stops matching.
      if (dropped != target) {
        for (const auto& instruction : dropped->instructions()) {
          if (instruction->opcode() != ir::Opcode::Phi) {
            break;
          }
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
