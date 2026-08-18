#include "optiforge/analysis/AnalysisPrinter.h"

#include <ostream>
#include <string>

#include "optiforge/analysis/AnalysisManager.h"
#include "optiforge/analysis/Dominators.h"
#include "optiforge/analysis/Liveness.h"
#include "optiforge/analysis/LoopInfo.h"
#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Instruction.h"
#include "optiforge/ir/Module.h"

namespace optiforge::analysis {

namespace {

std::string labelOf(const ir::BasicBlock* block) {
  return block == nullptr ? "<exit>" : block->label();
}

std::string nameOf(const ir::Value* value) {
  if (value == nullptr) {
    return "<null>";
  }
  return value->hasName() ? "%" + value->name() : "%<unnamed>";
}

/// Joins with ", ", or "-" when empty so an empty set is visibly empty rather
/// than an ambiguous blank.
template <class Range, class Fn>
std::string join(const Range& range, Fn describe) {
  std::string out;
  for (const auto& item : range) {
    if (!out.empty()) {
      out += ", ";
    }
    out += describe(item);
  }
  return out.empty() ? "-" : out;
}

void printDomTree(const DominatorTree& tree, const ir::BasicBlock* node, int depth,
                  std::ostream& out) {
  out << "    ";
  for (int i = 0; i < depth; ++i) {
    out << "  ";
  }
  out << labelOf(node) << '\n';
  for (const ir::BasicBlock* child : tree.children(node)) {
    printDomTree(tree, child, depth + 1, out);
  }
}

void printLoop(const Loop& loop, std::ostream& out) {
  const std::string indent(4 + 2 * (loop.depth() - 1), ' ');
  out << indent << "loop header=" << labelOf(loop.header()) << " depth=" << loop.depth()
      << '\n';
  out << indent << "  latches:   "
      << join(loop.latches(), [](const ir::BasicBlock* b) { return labelOf(b); }) << '\n';
  out << indent << "  blocks:    "
      << join(loop.blocks(), [](const ir::BasicBlock* b) { return labelOf(b); }) << '\n';
  out << indent << "  exits:     "
      << join(loop.exits(), [](const ir::BasicBlock* b) { return labelOf(b); }) << '\n';
  out << indent << "  preheader: "
      << (loop.preheader() != nullptr ? labelOf(loop.preheader()) : std::string("none"))
      << '\n';
  for (const Loop* nested : loop.subLoops()) {
    printLoop(*nested, out);
  }
}

}  // namespace

void printAnalyses(const ir::Module& module, std::ostream& out) {
  AnalysisManager manager;

  for (const auto& function : module.functions()) {
    if (function->isDeclaration()) {
      continue;
    }

    out << "function @" << function->name() << '\n';

    const DominatorTree& domtree = manager.get<DominatorTreeAnalysis>(*function);
    out << "  dominator tree:\n";
    printDomTree(domtree, function->entry(), 0, out);

    const DominatorTree& postdom = manager.get<PostDominatorTreeAnalysis>(*function);
    out << "  post-dominator tree:\n";
    printDomTree(postdom, nullptr, 0, out);

    const DominanceFrontier& frontier = manager.get<DominanceFrontierAnalysis>(*function);
    out << "  dominance frontiers:\n";
    for (const auto& block : function->blocks()) {
      out << "    " << block->label() << ": "
          << join(frontier.frontierOf(block.get()),
                  [](const ir::BasicBlock* b) { return labelOf(b); })
          << '\n';
    }

    const LoopInfo& loops = manager.get<LoopAnalysis>(*function);
    out << "  loops:\n";
    if (loops.empty()) {
      out << "    none\n";
    } else {
      for (const Loop* loop : loops.topLevelLoops()) {
        printLoop(*loop, out);
      }
    }

    const Liveness& liveness = manager.get<LivenessAnalysis>(*function);
    out << "  liveness:\n";
    for (const auto& block : function->blocks()) {
      out << "    " << block->label() << ":\n";
      out << "      in:  "
          << join(liveness.liveIn(block.get()),
                  [](const ir::Value* v) { return nameOf(v); })
          << '\n';
      out << "      out: "
          << join(liveness.liveOut(block.get()),
                  [](const ir::Value* v) { return nameOf(v); })
          << '\n';
    }

    const ReachingDefinitions& reaching =
        manager.get<ReachingDefinitionsAnalysis>(*function);
    // Two stores to the same slot are different definitions, so the slot name
    // alone is ambiguous; qualify by the block the store sits in.
    const auto describeStore = [](const ir::Instruction* store) {
      return nameOf(store->operand(1)) + "@" + labelOf(store->parent());
    };
    out << "  reaching definitions (stores):\n";
    for (const auto& block : function->blocks()) {
      out << "    " << block->label() << ": in="
          << join(reaching.reachingIn(block.get()), describeStore)
          << "  out=" << join(reaching.reachingOut(block.get()), describeStore)
          << '\n';
    }

    out << '\n';
  }
}

}  // namespace optiforge::analysis
