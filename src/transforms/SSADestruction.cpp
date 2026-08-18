#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>

#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Instruction.h"
#include "optiforge/ir/Module.h"
#include "optiforge/transforms/SSA.h"

namespace optiforge::transforms {

namespace {

/// One assignment of a parallel copy: the location `root` names receives
/// `source`.
struct Assignment {
  ir::Instruction* root;
  ir::Value* source;
};

/// The location a value occupies, following any coalescing.
const ir::Value* locationOf(const ir::Value* value) {
  if (value != nullptr && value->valueKind() == ir::Value::Kind::Instruction) {
    const auto* instruction = static_cast<const ir::Instruction*>(value);
    if (instruction->slotAlias() != nullptr) {
      return instruction->slotAlias();
    }
  }
  return value;
}

/// Emits a parallel copy as a sequence of ordinary ones.
///
/// The phis at the top of a block take effect simultaneously, so
/// `a = phi(.., b), b = phi(.., a)` exchanges the two values. Emitting
/// `a <- b` then `b <- a` would instead leave both holding the old b: the
/// classic swap problem, and reachable from ordinary source such as
/// `tmp = a; a = b; b = tmp;` inside a loop.
///
/// Standard resolution: emit any assignment whose destination no other pending
/// assignment still needs to read. When none qualifies, what is left is a
/// cycle, broken by saving one value into a fresh temporary first.
void emitParallelCopy(ir::BasicBlock& block, std::vector<Assignment> pending) {
  const auto emitOne = [&](ir::Instruction* root, ir::Value* source) {
    auto copy = std::make_unique<ir::Instruction>(ir::Opcode::Copy, root->type());
    copy->setName(block.parent()->nextTempName());
    copy->addOperand(source);
    // Writing where the phi's users will read is the whole point.
    copy->setSlotAlias(root);
    block.insertBeforeTerminator(std::move(copy));
  };

  while (!pending.empty()) {
    const auto ready = std::find_if(
        pending.begin(), pending.end(), [&](const Assignment& candidate) {
          return std::none_of(
              pending.begin(), pending.end(), [&](const Assignment& other) {
                return &other != &candidate &&
                       locationOf(other.source) == candidate.root;
              });
        });

    if (ready != pending.end()) {
      emitOne(ready->root, ready->source);
      pending.erase(ready);
      continue;
    }

    // Every remaining assignment is part of a cycle. Save one destination into
    // a temporary of its own and redirect whoever reads it.
    ir::Instruction* victim = pending.front().root;
    auto save = std::make_unique<ir::Instruction>(ir::Opcode::Copy, victim->type());
    save->setName(block.parent()->nextTempName());
    save->addOperand(victim);
    // Deliberately *not* aliased: the temporary needs a location of its own,
    // which is what breaks the cycle.
    ir::Instruction* saved = block.insertBeforeTerminator(std::move(save));

    for (Assignment& assignment : pending) {
      if (locationOf(assignment.source) == victim) {
        assignment.source = saved;
      }
    }
  }
}

}  // namespace

std::size_t splitCriticalEdges(ir::Function& function) {
  if (function.isDeclaration()) {
    return 0;
  }

  // Snapshot: splitting appends blocks, and iterating a growing container is a
  // bug waiting to happen.
  std::vector<ir::BasicBlock*> blocks;
  for (const auto& block : function.blocks()) {
    blocks.push_back(block.get());
  }

  std::size_t split = 0;
  for (ir::BasicBlock* source : blocks) {
    ir::Instruction* terminator = source->terminator();
    if (terminator == nullptr || terminator->successors().size() < 2) {
      continue;  // one successor cannot make a critical edge
    }

    for (std::size_t i = 0; i < terminator->successors().size(); ++i) {
      ir::BasicBlock* target = terminator->successors()[i];
      if (target->predecessors().size() < 2) {
        continue;
      }

      ir::BasicBlock* bridge = function.createBlock("crit.edge");
      auto branch = std::make_unique<ir::Instruction>(ir::Opcode::Br, ir::Type::getVoid());
      branch->addSuccessor(target);
      bridge->append(std::move(branch));

      terminator->setSuccessor(i, bridge);

      // Phis in the target named the old source; they must now name the
      // bridge, or their incoming edges stop matching the predecessors.
      for (const auto& instruction : target->instructions()) {
        if (instruction->opcode() != ir::Opcode::Phi) {
          break;
        }
        for (std::size_t j = 0; j < instruction->incomingCount(); ++j) {
          if (instruction->incomingBlock(j) == source) {
            instruction->setSuccessor(j, bridge);
          }
        }
      }

      ++split;
    }
  }

  if (split != 0) {
    function.recomputePredecessors();
  }
  return split;
}

std::size_t destroySSA(ir::Function& function) {
  if (function.isDeclaration()) {
    return 0;
  }

  std::vector<ir::Instruction*> phis;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block->instructions()) {
      if (instruction->opcode() != ir::Opcode::Phi) {
        break;  // phis lead the block
      }
      phis.push_back(instruction.get());
    }
  }
  if (phis.empty()) {
    return 0;
  }

  // Splitting first guarantees every phi edge has a block of its own to carry
  // the copy.
  splitCriticalEdges(function);

  // Each phi becomes a location. A copy in its own block reads that location,
  // and every user of the phi is redirected to it.
  std::unordered_map<ir::Instruction*, ir::Instruction*> rootFor;
  for (ir::Instruction* phi : phis) {
    auto root = std::make_unique<ir::Instruction>(ir::Opcode::Copy, phi->type());
    root->setName(phi->name());
    // Reads its own location, which the predecessors have already written. The
    // backend renders this as a self-move, which is harmless; Phase 8's
    // allocator removes it by coalescing.
    ir::Instruction* raw = root.get();
    raw->setSlotAlias(raw);
    raw->addOperand(raw);
    phi->parent()->insertAfterPhis(std::move(root));
    rootFor.emplace(phi, raw);
  }

  // Group by predecessor so each block's phis are handled as one parallel copy
  // rather than one at a time.
  std::unordered_map<ir::BasicBlock*, std::vector<Assignment>> pending;
  for (ir::Instruction* phi : phis) {
    ir::Instruction* root = rootFor.at(phi);
    for (std::size_t i = 0; i < phi->incomingCount(); ++i) {
      ir::Value* source = phi->operand(i);

      // Translate an operand that is itself a phi into that phi's location.
      // Without this the cycle detection below compares a phi against a root
      // and never matches, so `a = phi(.., b), b = phi(.., a)` is emitted
      // sequentially and both end up holding the same value -- the swap
      // problem, silently miscompiled.
      if (source != nullptr && source->valueKind() == ir::Value::Kind::Instruction) {
        const auto it = rootFor.find(static_cast<ir::Instruction*>(source));
        if (it != rootFor.end()) {
          source = it->second;
        }
      }

      pending[phi->incomingBlock(i)].push_back({root, source});
    }
  }

  for (auto& [block, assignments] : pending) {
    emitParallelCopy(*block, std::move(assignments));
  }

  for (ir::Instruction* phi : phis) {
    phi->replaceAllUsesWith(rootFor.at(phi));
  }

  const std::size_t removed = phis.size();
  for (ir::Instruction* phi : phis) {
    phi->eraseFromParent();
  }
  return removed;
}

std::size_t destroySSA(ir::Module& module) {
  std::size_t removed = 0;
  for (const auto& function : module.functions()) {
    removed += destroySSA(*function);
  }
  return removed;
}

}  // namespace optiforge::transforms
