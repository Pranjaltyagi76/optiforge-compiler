#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "optiforge/analysis/AnalysisManager.h"
#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Instruction.h"
#include "optiforge/ir/Module.h"
#include "optiforge/passes/Pass.h"
#include "optiforge/transforms/SSA.h"

namespace optiforge::transforms {

namespace {

using passes::Pass;
using passes::PassRegistration;

/// Sparse conditional constant propagation.
///
/// Preferred over separate constant propagation and unreachable-code removal
/// because it does both at once, and catches cases neither finds alone: a
/// branch proven constant makes one successor unreachable, so values that
/// merge there stay constant instead of being conservatively unknown.
///
/// Lattice per value: Unknown (nothing seen yet) < Constant(c) < Overdefined.
/// Only blocks proven reachable are ever evaluated, which is what "sparse"
/// and "conditional" mean here.
class SCCP final : public Pass {
public:
  std::string_view name() const override { return "sccp"; }
  std::string_view description() const override {
    return "sparse conditional constant propagation";
  }

  bool run(ir::Function& function, analysis::AnalysisManager&) override {
    function_ = &function;
    module_ = function.parent();
    values_.clear();
    reachable_.clear();
    blockWorklist_.clear();
    valueWorklist_.clear();

    markReachable(function.entry());
    solve();
    return rewrite();
  }

private:
  enum class State : std::uint8_t { Unknown, Constant, Overdefined };

  struct Lattice {
    State state = State::Unknown;
    ir::Value* constant = nullptr;  // meaningful only when state == Constant
  };

  // --- Lattice access ---

  Lattice latticeOf(ir::Value* value) {
    if (value == nullptr) {
      return {State::Overdefined, nullptr};
    }
    if (value->isConstant()) {
      return {State::Constant, value};
    }
    if (value->valueKind() == ir::Value::Kind::Argument) {
      return {State::Overdefined, nullptr};  // nothing known about a parameter
    }
    const auto it = values_.find(value);
    return it == values_.end() ? Lattice{} : it->second;
  }

  /// Lowers a value's lattice entry, queueing its users when it moves.
  void update(ir::Value* value, Lattice next) {
    Lattice& current = values_[value];
    if (current.state == next.state && current.constant == next.constant) {
      return;
    }
    // The lattice only ever descends, which is what guarantees termination.
    if (current.state == State::Overdefined) {
      return;
    }
    current = next;
    for (ir::Instruction* user : value->users()) {
      valueWorklist_.push_back(user);
    }
  }

  void markReachable(ir::BasicBlock* block) {
    if (block == nullptr || !reachable_.insert(block).second) {
      return;
    }
    blockWorklist_.push_back(block);
  }

  // --- Solver ---

  void solve() {
    while (!blockWorklist_.empty() || !valueWorklist_.empty()) {
      while (!blockWorklist_.empty()) {
        ir::BasicBlock* block = blockWorklist_.back();
        blockWorklist_.pop_back();
        for (const auto& instruction : block->instructions()) {
          visit(*instruction);
        }
      }
      while (!valueWorklist_.empty()) {
        ir::Instruction* instruction = valueWorklist_.back();
        valueWorklist_.pop_back();
        // Only evaluate instructions in blocks proven reachable.
        if (reachable_.count(instruction->parent()) != 0) {
          visit(*instruction);
        }
      }
    }
  }

  void visit(ir::Instruction& instruction) {
    switch (instruction.opcode()) {
      case ir::Opcode::Phi:
        visitPhi(instruction);
        return;

      case ir::Opcode::Br:
        markReachable(instruction.successors()[0]);
        return;

      case ir::Opcode::CondBr: {
        const Lattice condition = latticeOf(instruction.operand(0));
        if (condition.state == State::Constant &&
            condition.constant->valueKind() == ir::Value::Kind::ConstantBool) {
          // Exactly one edge is live, so the other successor is not visited at
          // all -- the "conditional" in the pass's name.
          const bool taken =
              static_cast<const ir::ConstantBool*>(condition.constant)->value();
          markReachable(instruction.successors()[taken ? 0 : 1]);
          return;
        }
        if (condition.state == State::Overdefined) {
          markReachable(instruction.successors()[0]);
          markReachable(instruction.successors()[1]);
        }
        return;
      }

      default:
        break;
    }

    if (!instruction.hasResult()) {
      return;
    }

    // Anything the evaluator does not model is unknown, and stays that way.
    ir::Value* folded = evaluate(instruction);
    if (folded != nullptr) {
      update(&instruction, {State::Constant, folded});
    } else if (anyOperandOverdefined(instruction) ||
               !isFoldable(instruction.opcode())) {
      update(&instruction, {State::Overdefined, nullptr});
    }
  }

  void visitPhi(ir::Instruction& phi) {
    Lattice result;
    for (std::size_t i = 0; i < phi.incomingCount(); ++i) {
      // An operand arriving along an edge that is not live contributes
      // nothing. This is what lets a phi stay constant when only one path
      // into the block is possible.
      if (reachable_.count(phi.incomingBlock(i)) == 0) {
        continue;
      }
      const Lattice incoming = latticeOf(phi.operand(i));
      if (incoming.state == State::Unknown) {
        continue;
      }
      if (incoming.state == State::Overdefined) {
        result = {State::Overdefined, nullptr};
        break;
      }
      if (result.state == State::Unknown) {
        result = incoming;
      } else if (result.constant != incoming.constant) {
        result = {State::Overdefined, nullptr};
        break;
      }
    }
    update(&phi, result);
  }

  bool anyOperandOverdefined(ir::Instruction& instruction) {
    for (std::size_t i = 0; i < instruction.operandCount(); ++i) {
      if (latticeOf(instruction.operand(i)).state == State::Overdefined) {
        return true;
      }
    }
    return false;
  }

  static bool isFoldable(ir::Opcode opcode) {
    switch (opcode) {
      case ir::Opcode::Add:
      case ir::Opcode::Sub:
      case ir::Opcode::Mul:
      case ir::Opcode::SDiv:
      case ir::Opcode::SRem:
      case ir::Opcode::Shl:
      case ir::Opcode::AShr:
      case ir::Opcode::Neg:
      case ir::Opcode::Not:
      case ir::Opcode::ICmp:
        return true;
      default:
        return false;
    }
  }

  /// Constant result, or null when the instruction is not yet known.
  ir::Value* evaluate(ir::Instruction& instruction) {
    if (!isFoldable(instruction.opcode())) {
      return nullptr;
    }

    std::vector<ir::Value*> constants;
    for (std::size_t i = 0; i < instruction.operandCount(); ++i) {
      const Lattice operand = latticeOf(instruction.operand(i));
      if (operand.state != State::Constant) {
        return nullptr;
      }
      constants.push_back(operand.constant);
    }

    const auto intOf = [](ir::Value* value) {
      return static_cast<const ir::ConstantInt*>(value)->value();
    };

    if (instruction.opcode() == ir::Opcode::Not) {
      return module_->getBool(
          !static_cast<const ir::ConstantBool*>(constants[0])->value());
    }
    if (constants[0]->valueKind() != ir::Value::Kind::ConstantInt) {
      return nullptr;
    }
    if (instruction.opcode() == ir::Opcode::Neg) {
      const std::int64_t value = intOf(constants[0]);
      return value == INT64_MIN ? nullptr : module_->getInt(-value);
    }
    if (constants.size() < 2 ||
        constants[1]->valueKind() != ir::Value::Kind::ConstantInt) {
      return nullptr;
    }

    const std::int64_t a = intOf(constants[0]);
    const std::int64_t b = intOf(constants[1]);

    switch (instruction.opcode()) {
      case ir::Opcode::Add: return module_->getInt(a + b);
      case ir::Opcode::Sub: return module_->getInt(a - b);
      case ir::Opcode::Mul: return module_->getInt(a * b);
      case ir::Opcode::SDiv:
        // Left to trap at run time rather than folded to something the
        // hardware would never produce.
        return (b == 0 || (a == INT64_MIN && b == -1)) ? nullptr
                                                       : module_->getInt(a / b);
      case ir::Opcode::SRem:
        return (b == 0 || (a == INT64_MIN && b == -1)) ? nullptr
                                                       : module_->getInt(a % b);
      case ir::Opcode::Shl:
        return (b < 0 || b >= 64) ? nullptr : module_->getInt(a << b);
      case ir::Opcode::AShr:
        return (b < 0 || b >= 64) ? nullptr : module_->getInt(a >> b);
      case ir::Opcode::ICmp:
        switch (instruction.predicate()) {
          case ir::Predicate::Eq: return module_->getBool(a == b);
          case ir::Predicate::Ne: return module_->getBool(a != b);
          case ir::Predicate::Lt: return module_->getBool(a < b);
          case ir::Predicate::Gt: return module_->getBool(a > b);
          case ir::Predicate::Le: return module_->getBool(a <= b);
          case ir::Predicate::Ge: return module_->getBool(a >= b);
        }
        return nullptr;
      default:
        return nullptr;
    }
  }

  // --- Rewrite ---

  bool rewrite() {
    bool changed = false;
    std::vector<ir::Instruction*> dead;

    for (const auto& block : function_->blocks()) {
      if (reachable_.count(block.get()) == 0) {
        // Unreachable blocks are left for simplify-cfg to remove; touching the
        // block list here would invalidate the iteration in progress.
        continue;
      }

      for (const auto& instruction : block->instructions()) {
        // Replace a conditional branch whose condition is now known.
        if (instruction->opcode() == ir::Opcode::CondBr) {
          const Lattice condition = latticeOf(instruction->operand(0));
          if (condition.state == State::Constant &&
              condition.constant->valueKind() == ir::Value::Kind::ConstantBool) {
            const bool taken =
                static_cast<const ir::ConstantBool*>(condition.constant)->value();
            ir::BasicBlock* target = instruction->successors()[taken ? 0 : 1];
            replaceWithBranch(*block, *instruction, target);
            changed = true;
            break;  // the block's terminator was replaced
          }
          continue;
        }

        if (!instruction->hasResult()) {
          continue;
        }
        const Lattice value = latticeOf(instruction.get());
        if (value.state != State::Constant || value.constant == instruction.get()) {
          continue;
        }
        instruction->replaceAllUsesWith(value.constant);
        dead.push_back(instruction.get());
        changed = true;
      }
    }

    for (ir::Instruction* instruction : dead) {
      instruction->eraseFromParent();
    }
    if (changed) {
      function_->recomputePredecessors();
      // Folding a branch orphans whatever only that edge reached. Leaving the
      // orphans for simplify-cfg would mean this pass returns IR the verifier
      // rejects, which --verify-each would report against the wrong pass.
      removeUnreachableBlocks(*function_);
    }
    return changed;
  }

  void replaceWithBranch(ir::BasicBlock& block, ir::Instruction& condbr,
                         ir::BasicBlock* target) {
    // Drop this block from the phis of the successor no longer reached, or
    // their arity would stop matching the predecessor list.
    for (ir::BasicBlock* successor : condbr.successors()) {
      if (successor != target) {
        removeIncomingFrom(*successor, &block);
      }
    }

    auto branch = std::make_unique<ir::Instruction>(ir::Opcode::Br, ir::Type::getVoid());
    branch->addSuccessor(target);
    condbr.eraseFromParent();
    block.append(std::move(branch));
  }

  static void removeIncomingFrom(ir::BasicBlock& block, ir::BasicBlock* predecessor) {
    for (const auto& instruction : block.instructions()) {
      if (instruction->opcode() != ir::Opcode::Phi) {
        break;
      }
      instruction->removeIncoming(predecessor);
    }
  }

  ir::Function* function_ = nullptr;
  ir::Module* module_ = nullptr;
  std::unordered_map<ir::Value*, Lattice> values_;
  std::unordered_set<const ir::BasicBlock*> reachable_;
  std::vector<ir::BasicBlock*> blockWorklist_;
  std::vector<ir::Instruction*> valueWorklist_;
};

std::unique_ptr<Pass> makeSCCP() { return std::make_unique<SCCP>(); }
const PassRegistration kSccp{"sccp", makeSCCP};

}  // namespace

void anchorSCCP() {}

}  // namespace optiforge::transforms
