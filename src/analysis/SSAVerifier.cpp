#include "optiforge/analysis/SSAVerifier.h"

#include <algorithm>
#include <unordered_set>

#include "optiforge/analysis/AnalysisManager.h"
#include "optiforge/analysis/Dominators.h"
#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Instruction.h"
#include "optiforge/ir/Module.h"

namespace optiforge::analysis {

namespace {

std::string describe(const ir::Instruction& instruction) {
  std::string text(toString(instruction.opcode()));
  if (instruction.hasResult() && instruction.hasName()) {
    text = "%" + instruction.name() + " = " + text;
  }
  return text;
}

}  // namespace

std::vector<std::string> verifySSA(const ir::Function& function,
                                   AnalysisManager& manager) {
  std::vector<std::string> errors;
  if (function.isDeclaration()) {
    return errors;
  }

  const DominatorTree& domtree = manager.get<DominatorTreeAnalysis>(function);
  const std::string prefix = "function @" + function.name();

  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block->instructions()) {
      const std::string where =
          prefix + ", block '" + block->label() + "', " + describe(*instruction);

      if (instruction->opcode() == ir::Opcode::Phi) {
        // Arity must match the predecessor count, or the phi has no value on
        // some path into the block.
        if (instruction->incomingCount() != block->predecessors().size()) {
          errors.push_back(where + ": phi has " +
                           std::to_string(instruction->incomingCount()) +
                           " incoming edge(s) but the block has " +
                           std::to_string(block->predecessors().size()) +
                           " predecessor(s)");
          continue;
        }

        std::unordered_set<const ir::BasicBlock*> predecessors(
            block->predecessors().begin(), block->predecessors().end());
        for (std::size_t i = 0; i < instruction->incomingCount(); ++i) {
          const ir::BasicBlock* from = instruction->incomingBlock(i);
          if (predecessors.count(from) == 0) {
            errors.push_back(where + ": incoming edge " + std::to_string(i) +
                             " names '" +
                             (from == nullptr ? "<null>" : from->label()) +
                             "', which is not a predecessor");
            continue;
          }
          // The operand must be available at the end of that predecessor.
          const ir::Value* operand = instruction->operand(i);
          if (operand != nullptr &&
              operand->valueKind() == ir::Value::Kind::Instruction) {
            const auto* definition = static_cast<const ir::Instruction*>(operand);
            if (!domtree.dominates(definition->parent(), from)) {
              errors.push_back(where + ": incoming value from '" + from->label() +
                               "' is not available on that edge");
            }
          }
        }
        continue;
      }

      // Ordinary instructions: every operand must be defined on every path
      // reaching this point.
      for (std::size_t i = 0; i < instruction->operandCount(); ++i) {
        const ir::Value* operand = instruction->operand(i);
        if (operand == nullptr ||
            operand->valueKind() != ir::Value::Kind::Instruction) {
          continue;  // constants and arguments are always available
        }
        const auto* definition = static_cast<const ir::Instruction*>(operand);
        const ir::BasicBlock* definedIn = definition->parent();

        if (definedIn == block.get()) {
          // Same block: the definition must come first.
          bool sawDefinition = false;
          for (const auto& candidate : block->instructions()) {
            if (candidate.get() == definition) {
              sawDefinition = true;
              break;
            }
            if (candidate.get() == instruction.get()) {
              break;
            }
          }
          if (!sawDefinition) {
            errors.push_back(where + ": operand " + std::to_string(i) +
                             " is used before it is defined");
          }
          continue;
        }

        if (!domtree.dominates(definedIn, block.get())) {
          errors.push_back(where + ": operand " + std::to_string(i) +
                           " is defined in '" + definedIn->label() +
                           "', which does not dominate this use");
        }
      }
    }
  }

  return errors;
}

std::vector<std::string> verifySSA(const ir::Module& module, AnalysisManager& manager) {
  std::vector<std::string> errors;
  for (const auto& function : module.functions()) {
    std::vector<std::string> found = verifySSA(*function, manager);
    errors.insert(errors.end(), found.begin(), found.end());
  }
  return errors;
}

}  // namespace optiforge::analysis
