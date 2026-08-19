#include <algorithm>
#include <memory>
#include <unordered_set>
#include <vector>

#include "optiforge/analysis/AnalysisManager.h"
#include "optiforge/analysis/LoopInfo.h"
#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Instruction.h"
#include "optiforge/passes/Pass.h"

namespace optiforge::transforms {

namespace {

using passes::Pass;
using passes::PassRegistration;

/// Instructions that may be moved to a point that executes more often, or on a
/// path where the original would not have run at all.
///
/// Excluded, and why:
///   - loads and stores: nothing here proves memory unchanged across the loop;
///   - calls: no purity information;
///   - division and remainder: they can trap, and hoisting a trap onto a path
///     that would not have taken it changes behaviour;
///   - phi nodes and terminators: their position is their meaning.
bool isSafeToHoist(const ir::Instruction& instruction) {
  switch (instruction.opcode()) {
    case ir::Opcode::Add:
    case ir::Opcode::Sub:
    case ir::Opcode::Mul:
    case ir::Opcode::Shl:
    case ir::Opcode::AShr:
    case ir::Opcode::Neg:
    case ir::Opcode::Not:
    case ir::Opcode::ICmp:
    case ir::Opcode::FAdd:
    case ir::Opcode::FSub:
    case ir::Opcode::FMul:
    case ir::Opcode::FCmp:
    case ir::Opcode::SIToFP:
      return true;
    default:
      return false;
  }
}

/// Moves loop-invariant computations into the preheader.
///
/// A value is invariant when every operand is defined outside the loop, or has
/// already been hoisted out of it. Working innermost-first means an inner
/// hoist can expose an outer one on the same sweep.
class LICM final : public Pass {
public:
  std::string_view name() const override { return "licm"; }
  std::string_view description() const override {
    return "hoist loop-invariant computations into the preheader";
  }

  bool run(ir::Function& function, analysis::AnalysisManager& manager) override {
    const analysis::LoopInfo& loops = manager.get<analysis::LoopAnalysis>(function);
    if (loops.empty()) {
      return false;
    }

    bool changed = false;
    // Deepest first, so an inner loop is done before the loop containing it.
    std::vector<const analysis::Loop*> ordered;
    for (const auto& loop : loops.allLoops()) {
      ordered.push_back(loop.get());
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const analysis::Loop* a, const analysis::Loop* b) {
                return a->depth() > b->depth();
              });

    for (const analysis::Loop* loop : ordered) {
      changed |= hoistFrom(*loop);
    }
    return changed;
  }

private:
  static bool hoistFrom(const analysis::Loop& loop) {
    auto* preheader = const_cast<ir::BasicBlock*>(loop.preheader());
    if (preheader == nullptr) {
      // Without a single dominating entry there is nowhere unambiguous to
      // hoist to: code placed in one predecessor would not run on paths
      // through the others.
      return false;
    }

    std::unordered_set<const ir::Value*> hoisted;
    bool changed = false;

    // Repeat until nothing more moves: hoisting one value can make another
    // invariant, and a single pass would leave those behind.
    bool progress = true;
    while (progress) {
      progress = false;

      for (const ir::BasicBlock* block : loop.blocks()) {
        std::vector<ir::Instruction*> movable;

        for (const auto& instruction : block->instructions()) {
          if (!isSafeToHoist(*instruction) || hoisted.count(instruction.get()) != 0) {
            continue;
          }
          if (!allOperandsInvariant(*instruction, loop, hoisted)) {
            continue;
          }
          movable.push_back(instruction.get());
        }

        for (ir::Instruction* instruction : movable) {
          instruction->moveBefore(*preheader, preheader->terminator());
          hoisted.insert(instruction);
          changed = true;
          progress = true;
        }
      }
    }

    return changed;
  }

  static bool allOperandsInvariant(const ir::Instruction& instruction,
                                   const analysis::Loop& loop,
                                   const std::unordered_set<const ir::Value*>& hoisted) {
    for (std::size_t i = 0; i < instruction.operandCount(); ++i) {
      const ir::Value* operand = instruction.operand(i);
      if (operand == nullptr) {
        return false;
      }
      // Constants and parameters never change inside a loop.
      if (operand->valueKind() != ir::Value::Kind::Instruction) {
        continue;
      }
      const auto* definition = static_cast<const ir::Instruction*>(operand);
      if (hoisted.count(definition) != 0) {
        continue;
      }
      if (loop.contains(definition->parent())) {
        return false;
      }
    }
    return true;
  }
};

std::unique_ptr<Pass> makeLICM() { return std::make_unique<LICM>(); }
const PassRegistration kLicm{"licm", makeLICM};

}  // namespace

void anchorLICM() {}

}  // namespace optiforge::transforms
