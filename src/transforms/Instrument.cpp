#include "optiforge/transforms/Instrument.h"

#include <memory>
#include <unordered_map>

#include "optiforge/analysis/AnalysisManager.h"
#include "optiforge/analysis/Dominators.h"
#include "optiforge/analysis/LoopInfo.h"
#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Instruction.h"
#include "optiforge/ir/Module.h"
#include "optiforge/transforms/SSA.h"

namespace optiforge::transforms {

namespace {

/// Places one counter at the very top of a block.
///
/// The top matters. `incq` writes the flags, so anywhere else risks landing it
/// between a comparison and the branch that reads the result. Nothing precedes
/// the first instruction of a block, so nothing there can be broken by it.
void insertCounter(ir::BasicBlock& block, std::uint32_t index) {
  auto counter =
      std::make_unique<ir::Instruction>(ir::Opcode::ProfInc, ir::Type::getVoid());
  counter->setCounterIndex(index);
  block.insertAtTop(std::move(counter));
}

/// Declares one of the runtime's timing hooks, or returns the existing one.
ir::Function* timingHook(ir::Module& module, const std::string& name) {
  if (ir::Function* existing = module.findFunction(name)) {
    return existing;
  }
  ir::Function* hook = module.createFunction(name, ir::Type::getVoid());
  hook->addArgument(ir::Type::getI64(), "slot");
  return hook;
}

std::unique_ptr<ir::Instruction> makeHookCall(ir::Module& module, ir::Function* hook,
                                              std::uint32_t slot) {
  auto call = std::make_unique<ir::Instruction>(ir::Opcode::Call, ir::Type::getVoid());
  call->setCallee(hook);
  call->addOperand(module.getInt(static_cast<std::int64_t>(slot)));
  return call;
}

}  // namespace

ProfileLayout instrumentForProfiling(ir::Module& module,
                                     analysis::AnalysisManager& manager,
                                     int optLevel, const std::string& compilerVersion,
                                     const std::string& defaultOutputPath,
                                     bool withTiming) {
  ProfileLayout layout;
  layout.enabled = true;
  layout.optLevel = optLevel;
  layout.compiler = compilerVersion;
  layout.defaultOutputPath = defaultOutputPath;
  layout.sourceName = module.sourceName();
  layout.sourceHash = module.sourceHash();

  ir::Function* enterHook = nullptr;
  ir::Function* exitHook = nullptr;
  if (withTiming) {
    enterHook = timingHook(module, "__ofprof_enter");
    exitHook = timingHook(module, "__ofprof_exit");
  }

  // Snapshot: the timing hooks above were appended to the module's function
  // list, and instrumenting a declaration would be meaningless anyway.
  std::vector<ir::Function*> bodies;
  for (const auto& function : module.functions()) {
    if (!function->isDeclaration()) {
      bodies.push_back(function.get());
    }
  }

  for (ir::Function* function : bodies) {
    // Hoisted and checked once. isDeclaration() already implies a block exists,
    // but the compiler cannot see that through the container, and -O3 reports
    // every dereference below as a potential null one without it.
    ir::BasicBlock* entryBlock = function->entry();
    if (entryBlock == nullptr) {
      continue;
    }

    // Split first, then read the CFG. Afterwards every successor of a
    // conditional branch has exactly one predecessor, which is what makes the
    // successor's block counter equal to that edge's count.
    splitCriticalEdges(*function);
    manager.invalidate(*function);

    const analysis::LoopInfo& loops = manager.get<analysis::LoopAnalysis>(*function);

    // One counter per block, numbered in the function's own block order so the
    // same source compiled twice produces the same indices (PROF-10).
    std::unordered_map<const ir::BasicBlock*, std::uint32_t> counterFor;
    for (const auto& block : function->blocks()) {
      const std::uint32_t index = layout.counterCount++;
      counterFor.emplace(block.get(), index);
      layout.counterNames.push_back(function->name() + ":" + block->label());
      insertCounter(*block, index);
    }

    // A function is entered exactly as often as its entry block runs.
    layout.records.push_back({ProfileRecord::Kind::Function,
                              counterFor.at(entryBlock), 0, function->name()});

    for (const auto& block : function->blocks()) {
      layout.records.push_back({ProfileRecord::Kind::Block, counterFor.at(block.get()),
                                0, function->name() + " " + block->label()});
    }

    // Branch outcomes, read off the successors rather than counted separately.
    for (const auto& block : function->blocks()) {
      const ir::Instruction* terminator = block->terminator();
      if (terminator == nullptr || terminator->opcode() != ir::Opcode::CondBr ||
          terminator->successors().size() != 2) {
        continue;
      }
      layout.records.push_back({ProfileRecord::Kind::Branch,
                                counterFor.at(terminator->successors()[0]),
                                counterFor.at(terminator->successors()[1]),
                                function->name() + " " + block->label()});
    }

    // Loops. A `while` header runs once per entry plus once per iteration, so
    // iterations = headerCount - entryCount, and the preheader supplies the
    // second number. A loop with several entry edges has no single block to
    // read that from, and gets no LOOP record rather than a wrong one.
    for (const auto& loop : loops.allLoops()) {
      const ir::BasicBlock* preheader = loop->preheader();
      if (preheader == nullptr) {
        continue;
      }
      const auto header = counterFor.find(loop->header());
      const auto entry = counterFor.find(preheader);
      if (header != counterFor.end() && entry != counterFor.end()) {
        layout.records.push_back({ProfileRecord::Kind::Loop, header->second,
                                  entry->second,
                                  function->name() + " " + loop->header()->label()});
      }
    }

    if (withTiming) {
      const auto slot = static_cast<std::uint32_t>(layout.timedFunctions.size());
      layout.timedFunctions.push_back(function->name());
      layout.records.push_back(
          {ProfileRecord::Kind::Time, slot, 0, function->name()});

      // Entry hook after the block counter, so the counter still leads the
      // block and the call cannot land between a compare and its branch.
      ir::Instruction* afterCounter = nullptr;
      std::size_t seen = 0;
      for (const auto& instruction : entryBlock->instructions()) {
        if (seen++ == 1) {
          afterCounter = instruction.get();
          break;
        }
      }
      entryBlock->insertBefore(makeHookCall(module, enterHook, slot), afterCounter);

      // One exit hook per return, because a function with several returns
      // leaves by whichever one it reaches.
      for (const auto& block : function->blocks()) {
        ir::Instruction* terminator = block->terminator();
        if (terminator != nullptr && terminator->opcode() == ir::Opcode::Ret) {
          block->insertBeforeTerminator(makeHookCall(module, exitHook, slot));
        }
      }
    }

    // The block list grew and every block gained an instruction.
    manager.invalidate(*function);
  }

  return layout;
}

}  // namespace optiforge::transforms
