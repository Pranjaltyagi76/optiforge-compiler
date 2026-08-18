#include "optiforge/analysis/Liveness.h"

#include <algorithm>

#include "optiforge/analysis/Dataflow.h"
#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Instruction.h"

namespace optiforge::analysis {

// ---------------------------------------------------------------------------
// Liveness
// ---------------------------------------------------------------------------

Liveness::Liveness(const ir::Function& function) {
  // Domain: every value that can be live, in a fixed order.
  for (const auto& argument : function.arguments()) {
    valueIndex_.emplace(argument.get(), values_.size());
    values_.push_back(argument.get());
  }
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block->instructions()) {
      if (instruction->hasResult()) {
        valueIndex_.emplace(instruction.get(), values_.size());
        values_.push_back(instruction.get());
      }
    }
  }

  const std::size_t domainSize = values_.size();
  const BitSet emptySet(domainSize);

  const auto indexOf = [&](const ir::Value* value) -> std::size_t {
    const auto it = valueIndex_.find(value);
    return it == valueIndex_.end() ? domainSize : it->second;
  };

  const DataflowResult solved = runDataflow(
      function, Direction::Backward, domainSize, emptySet, emptySet,
      // Meet: a value is live out of a block if it is live into *any* successor.
      [](BitSet& accumulator, const BitSet& contribution) {
        accumulator.unionWith(contribution);
      },
      // Transfer: walk the block backwards. live_in = use ∪ (live_out − def).
      [&](const ir::BasicBlock& block, const BitSet& liveOut, BitSet& liveIn) {
        liveIn = liveOut;

        const auto& instructions = block.instructions();
        for (auto it = instructions.rbegin(); it != instructions.rend(); ++it) {
          const ir::Instruction& instruction = **it;

          // The definition kills the value: uses below this point are covered,
          // uses above refer to an earlier definition.
          if (instruction.hasResult()) {
            const std::size_t index = indexOf(&instruction);
            if (index < domainSize) {
              liveIn.reset(index);
            }
          }

          // A phi's operands are live at the end of the *corresponding
          // predecessor*, not at the top of this block. Treating them as used
          // here would over-extend their live ranges and is the classic source
          // of register-allocator miscompiles, so phis are skipped and handled
          // below.
          if (instruction.opcode() == ir::Opcode::Phi) {
            continue;
          }

          for (std::size_t i = 0; i < instruction.operandCount(); ++i) {
            const std::size_t index = indexOf(instruction.operand(i));
            if (index < domainSize) {
              liveIn.set(index);
            }
          }
        }

        // Phi operands: live out of the predecessor they arrive from.
        for (const ir::BasicBlock* successor : block.successors()) {
          for (const auto& instruction : successor->instructions()) {
            if (instruction->opcode() != ir::Opcode::Phi) {
              break;  // phis are always first
            }
            // Without incoming-block metadata on phis (Phase 6 adds it), treat
            // every operand as live out of every predecessor. Conservative,
            // never wrong, and this branch is unreachable until phis exist.
            for (std::size_t i = 0; i < instruction->operandCount(); ++i) {
              const std::size_t index = indexOf(instruction->operand(i));
              if (index < domainSize) {
                liveIn.set(index);
              }
            }
          }
        }
      });

  iterations_ = solved.iterations;

  const auto materialize = [&](const BitSet& set) {
    std::vector<const ir::Value*> result;
    for (std::size_t index : set.elements()) {
      result.push_back(values_[index]);
    }
    return result;
  };
  for (const auto& block : function.blocks()) {
    in_[block.get()] = materialize(solved.in.at(block.get()));
    out_[block.get()] = materialize(solved.out.at(block.get()));
  }
}

const std::vector<const ir::Value*>& Liveness::liveIn(const ir::BasicBlock* block) const {
  const auto it = in_.find(block);
  return it == in_.end() ? empty_ : it->second;
}

const std::vector<const ir::Value*>& Liveness::liveOut(const ir::BasicBlock* block) const {
  const auto it = out_.find(block);
  return it == out_.end() ? empty_ : it->second;
}

bool Liveness::isLiveIn(const ir::BasicBlock* block, const ir::Value* value) const {
  const std::vector<const ir::Value*>& set = liveIn(block);
  return std::find(set.begin(), set.end(), value) != set.end();
}

bool Liveness::isLiveOut(const ir::BasicBlock* block, const ir::Value* value) const {
  const std::vector<const ir::Value*>& set = liveOut(block);
  return std::find(set.begin(), set.end(), value) != set.end();
}

// ---------------------------------------------------------------------------
// Reaching definitions
// ---------------------------------------------------------------------------

ReachingDefinitions::ReachingDefinitions(const ir::Function& function) {
  std::unordered_map<const ir::Instruction*, std::size_t> defIndex;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block->instructions()) {
      if (instruction->opcode() == ir::Opcode::Store) {
        defIndex.emplace(instruction.get(), defs_.size());
        defs_.push_back(instruction.get());
      }
    }
  }

  const std::size_t domainSize = defs_.size();
  const BitSet emptySet(domainSize);

  // Kill set per slot, precomputed. The textbook gen/kill formulation: a store
  // kills every other store to the same slot. Recomputing that by scanning all
  // definitions inside the transfer function made it O(stores) per store, which
  // on a function with thousands of each was the dominant cost of the whole
  // analysis.
  std::unordered_map<const ir::Value*, BitSet> killFor;
  for (std::size_t i = 0; i < defs_.size(); ++i) {
    const ir::Value* slot = defs_[i]->operand(1);
    auto it = killFor.find(slot);
    if (it == killFor.end()) {
      it = killFor.emplace(slot, BitSet(domainSize)).first;
    }
    it->second.set(i);
  }

  const DataflowResult solved = runDataflow(
      function, Direction::Forward, domainSize, emptySet, emptySet,
      // Meet: a definition reaches a block if it reaches along *any* path.
      [](BitSet& accumulator, const BitSet& contribution) {
        accumulator.unionWith(contribution);
      },
      // Transfer: out = gen ∪ (in − kill). A store kills every other store to
      // the same slot, because it overwrites what they wrote.
      [&](const ir::BasicBlock& block, const BitSet& reachingIn, BitSet& reachingOut) {
        reachingOut = reachingIn;
        for (const auto& instruction : block.instructions()) {
          if (instruction->opcode() != ir::Opcode::Store) {
            continue;
          }
          const ir::Value* slot = instruction->operand(1);
          const auto kill = killFor.find(slot);
          if (kill != killFor.end()) {
            reachingOut.subtract(kill->second);
          }
          // Subtracting removed this store too; it is the one that survives.
          reachingOut.set(defIndex.at(instruction.get()));
        }
      });

  iterations_ = solved.iterations;

  const auto materialize = [&](const BitSet& set) {
    std::vector<const ir::Instruction*> result;
    for (std::size_t index : set.elements()) {
      result.push_back(defs_[index]);
    }
    return result;
  };
  for (const auto& block : function.blocks()) {
    in_[block.get()] = materialize(solved.in.at(block.get()));
    out_[block.get()] = materialize(solved.out.at(block.get()));
  }
}

const std::vector<const ir::Instruction*>& ReachingDefinitions::reachingIn(
    const ir::BasicBlock* block) const {
  const auto it = in_.find(block);
  return it == in_.end() ? empty_ : it->second;
}

const std::vector<const ir::Instruction*>& ReachingDefinitions::reachingOut(
    const ir::BasicBlock* block) const {
  const auto it = out_.find(block);
  return it == out_.end() ? empty_ : it->second;
}

// ---------------------------------------------------------------------------
// Use-def
// ---------------------------------------------------------------------------

UseDefInfo::UseDefInfo(const ir::Function& function) {
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block->instructions()) {
      if (instruction->hasResult()) {
        definition_.emplace(instruction.get(), instruction.get());
      }
      // Program order, rather than the IR's user list, which is in whatever
      // order operands happened to be wired. Deterministic dumps need this.
      for (std::size_t i = 0; i < instruction->operandCount(); ++i) {
        const ir::Value* operand = instruction->operand(i);
        if (operand == nullptr) {
          continue;
        }
        std::vector<const ir::Instruction*>& list = users_[operand];
        if (list.empty() || list.back() != instruction.get()) {
          list.push_back(instruction.get());
        }
      }
    }
  }
}

const ir::Instruction* UseDefInfo::definitionOf(const ir::Value* value) const {
  const auto it = definition_.find(value);
  return it == definition_.end() ? nullptr : it->second;
}

const std::vector<const ir::Instruction*>& UseDefInfo::usersOf(
    const ir::Value* value) const {
  const auto it = users_.find(value);
  return it == users_.end() ? empty_ : it->second;
}

std::vector<const ir::Value*> UseDefInfo::operandsOf(
    const ir::Instruction* instruction) const {
  std::vector<const ir::Value*> result;
  for (std::size_t i = 0; i < instruction->operandCount(); ++i) {
    result.push_back(instruction->operand(i));
  }
  return result;
}

}  // namespace optiforge::analysis
