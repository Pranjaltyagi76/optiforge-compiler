#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "optiforge/analysis/AnalysisManager.h"
#include "optiforge/analysis/Dominators.h"
#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Instruction.h"
#include "optiforge/ir/Module.h"
#include "optiforge/transforms/SSA.h"

namespace optiforge::transforms {

namespace {

/// True when every use of this alloca is a plain load, or a store *into* it.
///
/// A store whose *value* operand is the alloca means the address itself was
/// stored somewhere, so the slot has escaped and cannot be promoted. Our
/// language cannot express that today, but the check costs nothing and stops
/// the pass being wrong the moment pointers arrive.
bool isPromotable(const ir::Instruction& alloca) {
  for (const ir::Instruction* user : alloca.users()) {
    switch (user->opcode()) {
      case ir::Opcode::Load:
        break;
      case ir::Opcode::Store:
        // operand(0) is the value, operand(1) the address.
        if (user->operand(1) != &alloca) {
          return false;
        }
        break;
      default:
        return false;
    }
  }
  return true;
}

/// Zero of the given type, used when a slot is read on a path where nothing
/// was stored. IRGen zero-initializes every declaration, so this is reached
/// only for genuinely unreachable reads; producing a defined value keeps the
/// IR well-formed either way.
ir::Value* zeroFor(ir::Module& module, const ir::Type* type) {
  if (type->isF64()) {
    return module.getFloat(0.0);
  }
  if (type->isI1()) {
    return module.getBool(false);
  }
  return module.getInt(0);
}

}  // namespace

std::size_t promoteMemoryToRegisters(ir::Function& function,
                                     analysis::AnalysisManager& manager) {
  if (function.isDeclaration()) {
    return 0;
  }

  // --- Step 1: which slots can be promoted at all ---
  std::vector<ir::Instruction*> promotable;
  for (ir::Instruction* alloca : function.allocas()) {
    if (isPromotable(*alloca)) {
      promotable.push_back(alloca);
    }
  }
  if (promotable.empty()) {
    return 0;
  }

  std::unordered_map<const ir::Instruction*, std::size_t> slotIndex;
  for (std::size_t i = 0; i < promotable.size(); ++i) {
    slotIndex.emplace(promotable[i], i);
  }

  const analysis::DominatorTree& domtree =
      manager.get<analysis::DominatorTreeAnalysis>(function);
  const analysis::DominanceFrontier& frontier =
      manager.get<analysis::DominanceFrontierAnalysis>(function);

  // --- Step 2: phi placement (Cytron et al.) ---
  //
  // A store to a slot in block B makes the slot's value depend on the path
  // taken, at exactly the blocks in B's iterated dominance frontier. Inserting
  // a phi is itself a definition, so its own frontier must be processed too.
  std::unordered_map<const ir::Instruction*, std::size_t> phiSlot;

  for (std::size_t i = 0; i < promotable.size(); ++i) {
    ir::Instruction* alloca = promotable[i];
    const ir::Type* type = alloca->allocatedType();

    std::vector<const ir::BasicBlock*> worklist;
    std::unordered_set<const ir::BasicBlock*> defining;
    for (const ir::Instruction* user : alloca->users()) {
      if (user->opcode() == ir::Opcode::Store && defining.insert(user->parent()).second) {
        worklist.push_back(user->parent());
      }
    }

    std::unordered_set<const ir::BasicBlock*> hasPhi;
    while (!worklist.empty()) {
      const ir::BasicBlock* block = worklist.back();
      worklist.pop_back();

      for (const ir::BasicBlock* join : frontier.frontierOf(block)) {
        if (!hasPhi.insert(join).second) {
          continue;
        }
        auto phi = std::make_unique<ir::Instruction>(ir::Opcode::Phi, type);
        phi->setName(function.nextTempName());
        ir::Instruction* raw = phi.get();
        const_cast<ir::BasicBlock*>(join)->insertAtTop(std::move(phi));
        phiSlot.emplace(raw, i);

        if (defining.insert(join).second) {
          worklist.push_back(join);
        }
      }
    }
  }

  // --- Step 3: rename ---
  //
  // A depth-first walk of the dominator tree, carrying a stack of the current
  // value of each slot. Iterative rather than recursive: a chain of a few
  // thousand ifs gives a dominator tree thousands deep, and recursing once per
  // level would exhaust the native stack.
  std::vector<std::vector<ir::Value*>> stacks(promotable.size());
  std::vector<ir::Instruction*> dead;

  struct Frame {
    const ir::BasicBlock* block;
    std::size_t childIndex;
    std::vector<std::size_t> pushed;  // slots this block pushed, to unwind
    bool entered;
  };

  std::vector<Frame> stack{{function.entry(), 0, {}, false}};

  while (!stack.empty()) {
    Frame& frame = stack.back();

    if (!frame.entered) {
      frame.entered = true;
      auto* block = const_cast<ir::BasicBlock*>(frame.block);

      for (const auto& instruction : block->instructions()) {
        ir::Instruction* raw = instruction.get();

        const auto phiIt = phiSlot.find(raw);
        if (phiIt != phiSlot.end()) {
          stacks[phiIt->second].push_back(raw);
          frame.pushed.push_back(phiIt->second);
          continue;
        }

        if (raw->opcode() == ir::Opcode::Load) {
          const auto it = slotIndex.find(
              static_cast<const ir::Instruction*>(raw->operand(0)));
          if (it == slotIndex.end()) {
            continue;
          }
          std::vector<ir::Value*>& values = stacks[it->second];
          ir::Value* current =
              values.empty()
                  ? zeroFor(*function.parent(), promotable[it->second]->allocatedType())
                  : values.back();
          raw->replaceAllUsesWith(current);
          dead.push_back(raw);
          continue;
        }

        if (raw->opcode() == ir::Opcode::Store) {
          const auto it = slotIndex.find(
              static_cast<const ir::Instruction*>(raw->operand(1)));
          if (it == slotIndex.end()) {
            continue;
          }
          stacks[it->second].push_back(raw->operand(0));
          frame.pushed.push_back(it->second);
          dead.push_back(raw);
          continue;
        }
      }

      // Fill in this block's contribution to each successor's phis.
      for (ir::BasicBlock* successor : block->successors()) {
        for (const auto& instruction : successor->instructions()) {
          const auto phiIt = phiSlot.find(instruction.get());
          if (phiIt == phiSlot.end()) {
            break;  // phis are always first
          }
          std::vector<ir::Value*>& values = stacks[phiIt->second];
          ir::Value* current =
              values.empty()
                  ? zeroFor(*function.parent(), promotable[phiIt->second]->allocatedType())
                  : values.back();
          instruction->addIncoming(current, block);
        }
      }
    }

    const std::vector<const ir::BasicBlock*>& children = domtree.children(frame.block);
    if (frame.childIndex < children.size()) {
      const ir::BasicBlock* child = children[frame.childIndex];
      ++frame.childIndex;
      stack.push_back({child, 0, {}, false});
      continue;
    }

    // Leaving the block: undo everything it pushed.
    for (std::size_t slot : frame.pushed) {
      stacks[slot].pop_back();
    }
    stack.pop_back();
  }

  // --- Step 4: remove the loads, stores and allocas ---
  for (ir::Instruction* instruction : dead) {
    instruction->eraseFromParent();
  }
  for (ir::Instruction* alloca : promotable) {
    alloca->eraseFromParent();
  }

  return promotable.size();
}

std::size_t promoteMemoryToRegisters(ir::Module& module,
                                     analysis::AnalysisManager& manager) {
  std::size_t promoted = 0;
  for (const auto& function : module.functions()) {
    promoted += promoteMemoryToRegisters(*function, manager);
    manager.invalidate(*function);
  }
  return promoted;
}

}  // namespace optiforge::transforms
