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
    if (callee->blocks().empty()) {
      return false;
    }

    // Size is the whole callee now, not one block (Phase 13).
    std::size_t size = 0;
    for (const auto& block : callee->blocks()) {
      size += block->instructions().size();
      if (block->terminator() == nullptr) {
        return false;  // malformed; leave it for the verifier to complain about
      }
    }
    if (size > budget) {
      return false;
    }

    // Two things are still refused, and for different reasons.
    //
    //   `call`  -- so a cycle in the call graph cannot expand forever. The
    //              `callee == &caller` check above only catches direct
    //              recursion; refusing to clone calls at all means an inlined
    //              body can never reintroduce one.
    //   `alloca`-- the verifier requires every alloca in the entry block, so
    //              cloning one means hoisting it into the caller's entry and
    //              reasoning about how many times it is now live. After
    //              mem2reg a small callee has none anyway.
    //
    // Everything else -- loads, stores, geps, phis, branches -- is cloneable,
    // which is what makes a multi-block callee reachable at all.
    for (const auto& block : callee->blocks()) {
      for (const auto& instruction : block->instructions()) {
        if (instruction->opcode() == ir::Opcode::Alloca ||
            instruction->opcode() == ir::Opcode::Call) {
          return false;
        }
      }
    }
    return true;
  }

  /// Splices the callee's body into the caller at the call site.
  ///
  /// A single-block callee could be spliced straight in, which is all this did
  /// before Phase 13. A multi-block one cannot: control has to leave the
  /// caller's block, run the clone, and come back. So the caller's block is
  /// split around the call and the clone is stitched between the halves:
  ///
  ///     before:                     after:
  ///       B: ...                      B:        ...;  br entry'
  ///          %r = call @f(x)          entry':   (clone of @f, x substituted)
  ///          ...rest...                 ...:    br cont
  ///                                   cont:     %r = phi [ ... ]
  ///                                             ...rest...
  ///
  /// Each `ret` in the clone becomes a branch to `cont`, and the values they
  /// returned are merged by a phi there. One return needs no phi, which is the
  /// common case and is worth not paying for.
  static void inlineCall(ir::Instruction& call) {
    const ir::Function& callee = *call.callee();
    ir::BasicBlock& target = *call.parent();
    ir::Function& caller = *target.parent();

    // --- Split the caller's block after the call ---
    ir::BasicBlock* cont = caller.createBlock("inline.cont");
    cont->executionCount = target.executionCount;
    {
      std::vector<ir::Instruction*> trailing;
      bool seen = false;
      for (const auto& instruction : target.instructions()) {
        if (instruction.get() == &call) {
          seen = true;
          continue;
        }
        if (seen) {
          trailing.push_back(instruction.get());
        }
      }
      for (ir::Instruction* instruction : trailing) {
        instruction->moveBefore(*cont, nullptr);
      }

      // The terminator moved with them, so everything the block used to branch
      // to is now reached from `cont`. Any phi there still names the old block
      // as the edge it arrives on, and a phi whose incoming block is not a
      // predecessor is invalid IR -- and worse, silently reads the wrong value
      // if the verifier is not looking.
      const ir::Instruction* terminator = cont->terminator();
      if (terminator != nullptr) {
        for (ir::BasicBlock* successor : terminator->successors()) {
          for (const auto& phi : successor->instructions()) {
            if (phi->opcode() != ir::Opcode::Phi) {
              break;  // phis are only ever at the top of a block
            }
            for (std::size_t i = 0; i < phi->incomingCount(); ++i) {
              if (phi->incomingBlock(i) == &target) {
                phi->setSuccessor(i, cont);
              }
            }
          }
        }
      }
    }

    // --- Clone the blocks, before any instruction, so branches can resolve ---
    std::unordered_map<const ir::BasicBlock*, ir::BasicBlock*> blockMap;
    for (const auto& block : callee.blocks()) {
      ir::BasicBlock* clone = caller.createBlock("inline." + block->label());
      clone->executionCount = target.executionCount;
      blockMap.emplace(block.get(), clone);
    }

    // Parameters map to the actual arguments.
    std::unordered_map<const ir::Value*, ir::Value*> mapping;
    for (std::size_t i = 0; i < callee.arguments().size() && i < call.operandCount();
         ++i) {
      mapping.emplace(callee.arguments()[i].get(), call.operand(i));
    }

    // --- Clone the instructions ---
    //
    // Operands are translated again in a second pass rather than here, because
    // a block may use a value defined by a block cloned after it -- which is
    // exactly what a loop in the callee looks like.
    std::vector<std::pair<ir::Value*, ir::BasicBlock*>> returns;
    std::vector<std::pair<const ir::Instruction*, ir::Instruction*>> cloned;

    for (const auto& block : callee.blocks()) {
      ir::BasicBlock* into = blockMap[block.get()];
      for (const auto& original : block->instructions()) {
        if (original->opcode() == ir::Opcode::Ret) {
          returns.emplace_back(
              original->operandCount() == 1 ? original->operand(0) : nullptr, into);
          auto branch =
              std::make_unique<ir::Instruction>(ir::Opcode::Br, ir::Type::getVoid());
          branch->addSuccessor(cont);
          into->append(std::move(branch));
          continue;
        }

        auto clone =
            std::make_unique<ir::Instruction>(original->opcode(), original->type());
        clone->setPredicate(original->predicate());
        clone->setCallee(original->callee());
        clone->setAllocatedType(original->allocatedType());
        clone->setAllocatedCount(original->allocatedCount());
        if (original->hasResult()) {
          clone->setName(caller.nextTempName());
        }
        for (std::size_t i = 0; i < original->operandCount(); ++i) {
          clone->addOperand(original->operand(i));  // translated below
        }
        for (ir::BasicBlock* successor : original->successors()) {
          const auto it = blockMap.find(successor);
          clone->addSuccessor(it == blockMap.end() ? successor : it->second);
        }
        ir::Instruction* inserted = into->append(std::move(clone));
        mapping.emplace(original.get(), inserted);
        cloned.emplace_back(original.get(), inserted);
      }
    }

    // --- Second pass: point every operand at its clone ---
    for (const auto& [original, clone] : cloned) {
      for (std::size_t i = 0; i < clone->operandCount(); ++i) {
        clone->setOperand(i, translate(mapping, clone->operand(i)));
      }
      (void)original;
    }

    // --- Hand the call's result back ---
    ir::Value* result = nullptr;
    if (call.hasResult() && !returns.empty()) {
      if (returns.size() == 1) {
        result = translate(mapping, returns.front().first);
      } else {
        auto merge = std::make_unique<ir::Instruction>(ir::Opcode::Phi, call.type());
        merge->setName(caller.nextTempName());
        for (const auto& [value, from] : returns) {
          merge->addIncoming(translate(mapping, value), from);
        }
        result = cont->insertAtTop(std::move(merge));
      }
    }
    if (result != nullptr) {
      call.replaceAllUsesWith(result);
    }

    // --- Enter the clone, and drop the call ---
    call.eraseFromParent();
    auto enter = std::make_unique<ir::Instruction>(ir::Opcode::Br, ir::Type::getVoid());
    enter->addSuccessor(blockMap[callee.blocks().front().get()]);
    target.append(std::move(enter));

    caller.recomputePredecessors();
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
