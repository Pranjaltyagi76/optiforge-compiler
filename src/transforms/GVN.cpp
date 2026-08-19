#include <map>
#include <memory>
#include <vector>

#include "optiforge/analysis/AnalysisManager.h"
#include "optiforge/analysis/Dominators.h"
#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Instruction.h"
#include "optiforge/passes/Pass.h"

namespace optiforge::transforms {

namespace {

using passes::Pass;
using passes::PassRegistration;

/// What makes two computations the same.
struct Expression {
  ir::Opcode opcode;
  ir::Predicate predicate;
  std::vector<const ir::Value*> operands;

  bool operator<(const Expression& other) const {
    if (opcode != other.opcode) return opcode < other.opcode;
    if (predicate != other.predicate) return predicate < other.predicate;
    return operands < other.operands;
  }
};

bool isCommutative(ir::Opcode opcode) {
  switch (opcode) {
    case ir::Opcode::Add:
    case ir::Opcode::Mul:
      return true;
    default:
      return false;
  }
}

/// Instructions worth numbering: pure, result-producing, and cheap to compare.
///
/// Loads and calls are excluded because nothing here proves memory or the
/// callee unchanged between two occurrences. Division is excluded because
/// hoisting a trap would change behaviour.
bool isCandidate(const ir::Instruction& instruction) {
  switch (instruction.opcode()) {
    case ir::Opcode::Add:
    case ir::Opcode::Sub:
    case ir::Opcode::Mul:
    case ir::Opcode::Shl:
    case ir::Opcode::AShr:
    case ir::Opcode::Neg:
    case ir::Opcode::Not:
    case ir::Opcode::ICmp:
    case ir::Opcode::FCmp:
    case ir::Opcode::FAdd:
    case ir::Opcode::FSub:
    case ir::Opcode::FMul:
    case ir::Opcode::SIToFP:
      return true;
    default:
      return false;
  }
}

/// Global value numbering, scoped by the dominator tree.
///
/// Walking the dominator tree in preorder with a scoped table makes the
/// "earlier definition dominates this use" requirement automatic: anything
/// still in the table was defined on the path from the entry to here, so no
/// explicit dominance check is needed. Entries are removed on the way back up.
class GVN final : public Pass {
public:
  std::string_view name() const override { return "gvn"; }
  std::string_view description() const override {
    return "eliminate redundant computations by dominator-scoped value numbering";
  }

  bool run(ir::Function& function, analysis::AnalysisManager& manager) override {
    const analysis::DominatorTree& domtree =
        manager.get<analysis::DominatorTreeAnalysis>(function);

    table_.clear();
    dead_.clear();
    visit(function.entry(), domtree);

    for (ir::Instruction* instruction : dead_) {
      instruction->eraseFromParent();
    }
    return !dead_.empty();
  }

private:
  void visit(const ir::BasicBlock* block, const analysis::DominatorTree& domtree) {
    std::vector<Expression> added;

    for (const auto& instruction : block->instructions()) {
      if (!isCandidate(*instruction)) {
        continue;
      }

      Expression key{instruction->opcode(), instruction->predicate(), {}};
      for (std::size_t i = 0; i < instruction->operandCount(); ++i) {
        key.operands.push_back(instruction->operand(i));
      }
      // a + b and b + a are the same computation, so order the operands before
      // comparing. Sorting by address is fine here: the result is only ever
      // compared against another sorted list, never printed.
      if (isCommutative(key.opcode) && key.operands.size() == 2 &&
          key.operands[1] < key.operands[0]) {
        std::swap(key.operands[0], key.operands[1]);
      }

      const auto existing = table_.find(key);
      if (existing != table_.end()) {
        instruction->replaceAllUsesWith(existing->second);
        dead_.push_back(instruction.get());
        continue;
      }

      table_.emplace(key, instruction.get());
      added.push_back(key);
    }

    for (const ir::BasicBlock* child : domtree.children(block)) {
      visit(child, domtree);
    }

    // Leaving this subtree: these definitions no longer dominate anything.
    for (const Expression& key : added) {
      table_.erase(key);
    }
  }

  std::map<Expression, ir::Value*> table_;
  std::vector<ir::Instruction*> dead_;
};

std::unique_ptr<Pass> makeGVN() { return std::make_unique<GVN>(); }
const PassRegistration kGvn{"gvn", makeGVN};

}  // namespace

void anchorGVN() {}

}  // namespace optiforge::transforms
