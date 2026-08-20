#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "optiforge/analysis/AnalysisManager.h"
#include "optiforge/analysis/ProfileAnalysis.h"
#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Instruction.h"
#include "optiforge/ir/Module.h"
#include "optiforge/passes/Pass.h"
#include "optiforge/profile/Profile.h"

namespace optiforge::transforms {

namespace {

using passes::Pass;
using passes::PassRegistration;

/// Largest callee body worth inlining, in instructions, with no profile to go on.
///
/// A static guess, and a conservative one, because without measurement every
/// inline is a bet that the call site is hot.
constexpr std::size_t kSizeBudget = 12;

/// The same budget once the guess is replaced by a measurement (PGO-06).
///
/// A hot call site can afford a much larger callee: the code growth is paid
/// once and the call overhead is saved every time round. A cold one gets zero,
/// which is not a smaller budget but a different decision -- code that never
/// runs should not be duplicated at all.
constexpr std::size_t kHotSizeBudget = 250;

/// Inlines small callees.
///
/// **Restricted to single-block callees** whose body ends in `ret`. That covers
/// the shape inlining is worth most for -- a small arithmetic helper -- without
/// the block cloning, block renaming and return-merging phi that a general
/// inliner needs. Attempting the general case here would risk correctness for
/// a gain the benchmark suite cannot yet measure; a multi-block inliner belongs
/// with the profile data that says which call sites deserve it.
///
/// The real prize is not the call overhead removed but what inlining exposes:
/// the callee's body meets the caller's constants, which is why the -O2
/// pipeline runs sccp and gvn again immediately afterwards.
class Inliner final : public Pass {
public:
  std::string_view name() const override { return "inline"; }
  std::string_view description() const override {
    return "inline small single-block callees";
  }

  bool run(ir::Function& function, analysis::AnalysisManager& manager) override {
    // `--disable-pgo=inline` withholds the profile from this pass alone, which
    // makes it take exactly the path an unprofiled build takes -- the static
    // budget for every call site. That equivalence is what lets the attribution
    // subtraction in methodology.md section 5 mean anything (G-05, G-06).
    const profile::ProfileData* profile =
        pgo().inlining ? manager.getCached<analysis::ProfileAnalysis>(function)
                       : nullptr;
    const bool measured = profile != nullptr && profile->isValid();

    bool changed = false;

    // Collect first: inlining mutates the block being walked.
    std::vector<ir::Instruction*> calls;
    for (const auto& block : function.blocks()) {
      const profile::Heat heat =
          measured ? profile->blockHeat(function.name(), block->label())
                   : profile::Heat::Unknown;

      // A cold call site is not inlined at any size. Without a profile every
      // site is Unknown, which takes the static budget -- the pass's normal
      // behaviour, not a degradation path.
      if (heat == profile::Heat::Cold) {
        for (const auto& instruction : block->instructions()) {
          if (instruction->opcode() == ir::Opcode::Call &&
              instruction->callee() != nullptr &&
              !instruction->callee()->isDeclaration()) {
            remark(function, block->label(), "cold call site to @" +
                                                 instruction->callee()->name() +
                                                 "; not inlined");
          }
        }
        continue;
      }

      const std::size_t budget =
          heat == profile::Heat::Hot ? kHotSizeBudget : kSizeBudget;

      for (const auto& instruction : block->instructions()) {
        if (instruction->opcode() != ir::Opcode::Call) {
          continue;
        }
        if (!shouldInline(function, *instruction, budget)) {
          continue;
        }
        if (heat == profile::Heat::Hot && instruction->callee() != nullptr) {
          remark(function, block->label(),
                 "hot call site to @" + instruction->callee()->name() +
                     "; budget raised to " + std::to_string(budget));
        }
        calls.push_back(instruction.get());
      }
    }

    for (ir::Instruction* call : calls) {
      inlineCall(*call);
      changed = true;
    }
    return changed;
  }

private:
  void remark(const ir::Function& function, const std::string& block,
              const std::string& what) {
    if (remarks() != nullptr) {
      *remarks() << "inline: @" << function.name() << ":" << block << ": " << what
                 << "\n";
    }
  }

  static bool shouldInline(const ir::Function& caller, const ir::Instruction& call,
                           std::size_t budget) {
    const ir::Function* callee = call.callee();
    if (callee == nullptr || callee->isDeclaration()) {
      return false;  // a runtime builtin has no body to inline
    }
    if (callee == &caller) {
      return false;  // recursion would not terminate
    }
    if (callee->blocks().size() != 1) {
      return false;  // see the class comment
    }

    const ir::BasicBlock& body = *callee->blocks().front();
    if (body.instructions().empty()) {
      return false;
    }
    const ir::Instruction* terminator = body.terminator();
    if (terminator == nullptr || terminator->opcode() != ir::Opcode::Ret) {
      return false;
    }
    if (body.instructions().size() > budget) {
      return false;
    }

    // Anything with its own storage or further calls is left alone: cloning
    // those correctly needs the general machinery this pass avoids.
    for (const auto& instruction : body.instructions()) {
      switch (instruction->opcode()) {
        case ir::Opcode::Alloca:
        case ir::Opcode::Call:
        case ir::Opcode::Load:
        case ir::Opcode::Store:
        case ir::Opcode::Phi:
          return false;
        default:
          break;
      }
    }
    return true;
  }

  /// Clones the callee's instructions in front of the call, substituting
  /// arguments for parameters, then replaces the call with the returned value.
  static void inlineCall(ir::Instruction& call) {
    const ir::Function& callee = *call.callee();
    ir::BasicBlock& target = *call.parent();
    ir::Function& caller = *target.parent();

    // Parameters map to the actual arguments; every cloned instruction maps to
    // its clone.
    std::unordered_map<const ir::Value*, ir::Value*> mapping;
    for (std::size_t i = 0; i < callee.arguments().size() && i < call.operandCount();
         ++i) {
      mapping.emplace(callee.arguments()[i].get(), call.operand(i));
    }

    ir::Value* returned = nullptr;

    for (const auto& original : callee.blocks().front()->instructions()) {
      if (original->opcode() == ir::Opcode::Ret) {
        if (original->operandCount() == 1) {
          returned = translate(mapping, original->operand(0));
        }
        break;
      }

      auto clone =
          std::make_unique<ir::Instruction>(original->opcode(), original->type());
      clone->setPredicate(original->predicate());
      if (original->hasResult()) {
        clone->setName(caller.nextTempName());
      }
      for (std::size_t i = 0; i < original->operandCount(); ++i) {
        clone->addOperand(translate(mapping, original->operand(i)));
      }

      ir::Instruction* inserted = target.insertBefore(std::move(clone), &call);
      mapping.emplace(original.get(), inserted);
    }

    if (call.hasResult() && returned != nullptr) {
      call.replaceAllUsesWith(returned);
    }
    call.eraseFromParent();
  }

  static ir::Value* translate(
      const std::unordered_map<const ir::Value*, ir::Value*>& mapping,
      ir::Value* value) {
    const auto it = mapping.find(value);
    // Anything not in the map is a constant, which needs no translation: the
    // module interns them, so both functions already share one.
    return it == mapping.end() ? value : it->second;
  }
};

std::unique_ptr<Pass> makeInliner() { return std::make_unique<Inliner>(); }
const PassRegistration kInline{"inline", makeInliner};

}  // namespace

void anchorInline() {}

}  // namespace optiforge::transforms

namespace optiforge::transforms {

std::size_t removeUnusedFunctions(ir::Module& module) {
  std::size_t removed = 0;
  bool progress = true;

  // Repeat: removing one function can leave the only caller of another gone.
  while (progress) {
    progress = false;

    std::unordered_map<const ir::Function*, unsigned> callers;
    for (const auto& function : module.functions()) {
      callers.emplace(function.get(), 0);
    }
    for (const auto& function : module.functions()) {
      for (const auto& block : function->blocks()) {
        for (const auto& instruction : block->instructions()) {
          if (instruction->opcode() == ir::Opcode::Call &&
              instruction->callee() != nullptr) {
            ++callers[instruction->callee()];
          }
        }
      }
    }

    for (const auto& function : module.functions()) {
      if (function->isDeclaration() || function->name() == "main") {
        continue;  // declarations are the runtime; main is the entry point
      }
      if (callers[function.get()] != 0) {
        continue;
      }
      module.eraseFunction(function.get());
      ++removed;
      progress = true;
      break;  // the container just changed underneath us
    }
  }

  return removed;
}

}  // namespace optiforge::transforms
