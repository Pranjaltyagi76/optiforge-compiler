#include <algorithm>
#include <memory>
#include <unordered_set>
#include <vector>

#include "optiforge/analysis/AnalysisManager.h"
#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Instruction.h"
#include "optiforge/ir/Module.h"
#include "optiforge/passes/Pass.h"

namespace optiforge::transforms {

namespace {

using passes::Pass;
using passes::PassRegistration;

const ir::ConstantInt* asInt(const ir::Value* value) {
  return value != nullptr && value->valueKind() == ir::Value::Kind::ConstantInt
             ? static_cast<const ir::ConstantInt*>(value)
             : nullptr;
}

const ir::ConstantBool* asBool(const ir::Value* value) {
  return value != nullptr && value->valueKind() == ir::Value::Kind::ConstantBool
             ? static_cast<const ir::ConstantBool*>(value)
             : nullptr;
}

bool isInt(const ir::Value* value, std::int64_t wanted) {
  const ir::ConstantInt* constant = asInt(value);
  return constant != nullptr && constant->value() == wanted;
}

/// Instructions whose removal would change what the program does.
bool hasSideEffects(const ir::Instruction& instruction) {
  switch (instruction.opcode()) {
    case ir::Opcode::Store:
    case ir::Opcode::Call:  // conservatively: nothing here proves a call is pure
    case ir::Opcode::Ret:
    case ir::Opcode::Br:
    case ir::Opcode::CondBr:
      return true;
    case ir::Opcode::SDiv:
    case ir::Opcode::SRem:
      // Division may trap, so removing it is only safe when the divisor is
      // provably non-zero.
      return asInt(instruction.operand(1)) == nullptr ||
             asInt(instruction.operand(1))->value() == 0;
    default:
      return false;
  }
}

/// Power of two, or -1.
int log2Exact(std::int64_t value) {
  if (value <= 0 || (value & (value - 1)) != 0) {
    return -1;
  }
  int shift = 0;
  while ((std::int64_t{1} << shift) != value) {
    ++shift;
  }
  return shift;
}

// ---------------------------------------------------------------------------
// Constant folding
// ---------------------------------------------------------------------------

/// Evaluates instructions whose operands are all known, and applies the
/// algebraic identities that need only one operand to be known.
class ConstantFolding final : public Pass {
public:
  std::string_view name() const override { return "constant-folding"; }
  std::string_view description() const override {
    return "evaluate constant expressions and apply algebraic identities";
  }

  bool run(ir::Function& function, analysis::AnalysisManager&) override {
    ir::Module& module = *function.parent();
    std::vector<ir::Instruction*> dead;
    bool changed = false;

    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block->instructions()) {
        ir::Value* replacement = fold(module, *instruction);
        if (replacement == nullptr || replacement == instruction.get()) {
          continue;
        }
        instruction->replaceAllUsesWith(replacement);
        dead.push_back(instruction.get());
        changed = true;
      }
    }

    for (ir::Instruction* instruction : dead) {
      instruction->eraseFromParent();
    }
    return changed;
  }

private:
  static ir::Value* fold(ir::Module& module, ir::Instruction& instruction) {
    const std::size_t operands = instruction.operandCount();
    ir::Value* lhs = operands > 0 ? instruction.operand(0) : nullptr;
    ir::Value* rhs = operands > 1 ? instruction.operand(1) : nullptr;

    const ir::ConstantInt* a = asInt(lhs);
    const ir::ConstantInt* b = asInt(rhs);

    switch (instruction.opcode()) {
      case ir::Opcode::Add:
        if (a != nullptr && b != nullptr) return module.getInt(a->value() + b->value());
        if (isInt(rhs, 0)) return lhs;
        if (isInt(lhs, 0)) return rhs;
        return nullptr;

      case ir::Opcode::Sub:
        if (a != nullptr && b != nullptr) return module.getInt(a->value() - b->value());
        if (isInt(rhs, 0)) return lhs;
        // x - x is zero even when x is unknown.
        if (lhs == rhs) return module.getInt(0);
        return nullptr;

      case ir::Opcode::Mul:
        if (a != nullptr && b != nullptr) return module.getInt(a->value() * b->value());
        if (isInt(rhs, 1)) return lhs;
        if (isInt(lhs, 1)) return rhs;
        if (isInt(rhs, 0) || isInt(lhs, 0)) return module.getInt(0);
        return nullptr;

      case ir::Opcode::SDiv:
        // INT64_MIN / -1 overflows, so it is left for the target to trap on
        // rather than folded to a value the hardware would never produce.
        if (a != nullptr && b != nullptr && b->value() != 0 &&
            !(a->value() == INT64_MIN && b->value() == -1)) {
          return module.getInt(a->value() / b->value());
        }
        if (isInt(rhs, 1)) return lhs;
        return nullptr;

      case ir::Opcode::SRem:
        if (a != nullptr && b != nullptr && b->value() != 0 &&
            !(a->value() == INT64_MIN && b->value() == -1)) {
          return module.getInt(a->value() % b->value());
        }
        if (isInt(rhs, 1)) return module.getInt(0);
        return nullptr;

      case ir::Opcode::Shl:
        if (a != nullptr && b != nullptr && b->value() >= 0 && b->value() < 64) {
          return module.getInt(a->value() << b->value());
        }
        if (isInt(rhs, 0)) return lhs;
        return nullptr;

      case ir::Opcode::AShr:
        if (a != nullptr && b != nullptr && b->value() >= 0 && b->value() < 64) {
          return module.getInt(a->value() >> b->value());
        }
        if (isInt(rhs, 0)) return lhs;
        return nullptr;

      case ir::Opcode::Neg:
        if (a != nullptr && a->value() != INT64_MIN) {
          return module.getInt(-a->value());
        }
        return nullptr;

      case ir::Opcode::Not: {
        const ir::ConstantBool* value = asBool(lhs);
        return value != nullptr ? module.getBool(!value->value()) : nullptr;
      }

      case ir::Opcode::ICmp: {
        if (a != nullptr && b != nullptr) {
          return module.getBool(compare(instruction.predicate(), a->value(), b->value()));
        }
        // `bool == bool` and `bool != bool` are legal source, and the operands
        // are ConstantBool rather than ConstantInt, so the integer path above
        // never sees them.
        const ir::ConstantBool* lhsBool = asBool(lhs);
        const ir::ConstantBool* rhsBool = asBool(rhs);
        if (lhsBool != nullptr && rhsBool != nullptr) {
          if (instruction.predicate() == ir::Predicate::Eq) {
            return module.getBool(lhsBool->value() == rhsBool->value());
          }
          if (instruction.predicate() == ir::Predicate::Ne) {
            return module.getBool(lhsBool->value() != rhsBool->value());
          }
        }
        // A value always equals itself, whatever it is.
        if (lhs == rhs && lhs != nullptr) {
          switch (instruction.predicate()) {
            case ir::Predicate::Eq:
            case ir::Predicate::Le:
            case ir::Predicate::Ge:
              return module.getBool(true);
            case ir::Predicate::Ne:
            case ir::Predicate::Lt:
            case ir::Predicate::Gt:
              return module.getBool(false);
          }
        }
        return nullptr;
      }

      default:
        return nullptr;
    }
  }

  static bool compare(ir::Predicate predicate, std::int64_t a, std::int64_t b) {
    switch (predicate) {
      case ir::Predicate::Eq: return a == b;
      case ir::Predicate::Ne: return a != b;
      case ir::Predicate::Lt: return a < b;
      case ir::Predicate::Gt: return a > b;
      case ir::Predicate::Le: return a <= b;
      case ir::Predicate::Ge: return a >= b;
    }
    return false;
  }
};

// ---------------------------------------------------------------------------
// Copy propagation
// ---------------------------------------------------------------------------

/// Removes copies, and phis that have nothing to choose between.
///
/// A phi whose incoming values are all the same value is not a choice at all,
/// and folding it away is what lets the blocks around it merge.
class CopyPropagation final : public Pass {
public:
  std::string_view name() const override { return "copy-propagation"; }
  std::string_view description() const override {
    return "forward copies and fold phis whose operands all agree";
  }

  bool run(ir::Function& function, analysis::AnalysisManager&) override {
    std::vector<ir::Instruction*> dead;
    bool changed = false;

    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block->instructions()) {
        ir::Value* source = nullptr;

        if (instruction->opcode() == ir::Opcode::Copy &&
            instruction->slotAlias() == nullptr) {
          source = instruction->operand(0);
        } else if (instruction->opcode() == ir::Opcode::Phi &&
                   instruction->operandCount() > 0) {
          ir::Value* first = instruction->operand(0);
          bool uniform = true;
          for (std::size_t i = 1; i < instruction->operandCount(); ++i) {
            // An operand that is the phi itself is a self-reference around a
            // loop and does not make the phi non-uniform.
            if (instruction->operand(i) != first &&
                instruction->operand(i) != instruction.get()) {
              uniform = false;
              break;
            }
          }
          if (uniform && first != instruction.get()) {
            source = first;
          }
        }

        if (source == nullptr || source == instruction.get()) {
          continue;
        }
        instruction->replaceAllUsesWith(source);
        dead.push_back(instruction.get());
        changed = true;
      }
    }

    for (ir::Instruction* instruction : dead) {
      instruction->eraseFromParent();
    }
    return changed;
  }
};

// ---------------------------------------------------------------------------
// Dead code elimination
// ---------------------------------------------------------------------------

/// Mark-and-sweep over the use lists.
///
/// Seeded with the instructions that have observable effects, then propagated
/// backwards through operands. On SSA with maintained use lists this is one
/// linear pass and needs no dataflow at all.
class DeadCodeElimination final : public Pass {
public:
  std::string_view name() const override { return "dce"; }
  std::string_view description() const override {
    return "remove instructions whose results nothing observes";
  }

  bool run(ir::Function& function, analysis::AnalysisManager&) override {
    std::unordered_set<const ir::Instruction*> live;
    std::vector<ir::Instruction*> worklist;

    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block->instructions()) {
        if (hasSideEffects(*instruction)) {
          if (live.insert(instruction.get()).second) {
            worklist.push_back(instruction.get());
          }
        }
      }
    }

    while (!worklist.empty()) {
      ir::Instruction* instruction = worklist.back();
      worklist.pop_back();
      for (std::size_t i = 0; i < instruction->operandCount(); ++i) {
        ir::Value* operand = instruction->operand(i);
        if (operand == nullptr ||
            operand->valueKind() != ir::Value::Kind::Instruction) {
          continue;
        }
        auto* definition = static_cast<ir::Instruction*>(operand);
        if (live.insert(definition).second) {
          worklist.push_back(definition);
        }
      }
    }

    std::vector<ir::Instruction*> dead;
    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block->instructions()) {
        if (live.count(instruction.get()) == 0) {
          dead.push_back(instruction.get());
        }
      }
    }

    // Detach every operand first: a dead instruction may be used by another
    // dead one, and erasing in order would otherwise leave a stale user entry.
    for (ir::Instruction* instruction : dead) {
      instruction->dropAllReferences();
    }
    for (ir::Instruction* instruction : dead) {
      instruction->eraseFromParent();
    }
    return !dead.empty();
  }
};

// ---------------------------------------------------------------------------
// Strength reduction
// ---------------------------------------------------------------------------

/// Replaces expensive operations with cheaper ones of identical meaning.
///
/// Only multiplication by a power of two is reduced. Signed division and
/// remainder by a power of two are *not*: both round toward zero while an
/// arithmetic shift rounds toward negative infinity, so a correct rewrite
/// needs a sign-correction sequence and a logical-shift opcode the IR does not
/// have. Approximating it would miscompile every negative dividend, so it is
/// left alone rather than done nearly right.
class StrengthReduction final : public Pass {
public:
  std::string_view name() const override { return "strength-reduction"; }
  std::string_view description() const override {
    return "replace multiplication by a power of two with a shift";
  }

  bool run(ir::Function& function, analysis::AnalysisManager&) override {
    ir::Module& module = *function.parent();
    bool changed = false;

    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block->instructions()) {
        if (instruction->opcode() != ir::Opcode::Mul) {
          continue;
        }

        // Constant folding has already handled two constants, so at most one
        // side is known here. Hold the pointer rather than looking it up twice:
        // the second lookup is provably non-null but nothing in the code says
        // so, and -O3 rightly flags the dereference.
        std::size_t constantSide = 1;
        const ir::ConstantInt* factor = asInt(instruction->operand(1));
        if (factor == nullptr) {
          constantSide = 0;
          factor = asInt(instruction->operand(0));
        }
        if (factor == nullptr) {
          continue;
        }

        const int shift = log2Exact(factor->value());
        if (shift <= 0) {
          continue;  // 1 is an identity, handled by folding
        }

        ir::Value* value = instruction->operand(1 - constantSide);
        instruction->setOperand(0, value);
        instruction->setOperand(1, module.getInt(shift));
        instruction->setOpcode(ir::Opcode::Shl);
        changed = true;
      }
    }
    return changed;
  }
};

std::unique_ptr<Pass> makeConstantFolding() { return std::make_unique<ConstantFolding>(); }
std::unique_ptr<Pass> makeCopyPropagation() { return std::make_unique<CopyPropagation>(); }
std::unique_ptr<Pass> makeDCE() { return std::make_unique<DeadCodeElimination>(); }
std::unique_ptr<Pass> makeStrengthReduction() {
  return std::make_unique<StrengthReduction>();
}

const PassRegistration kFolding{"constant-folding", makeConstantFolding};
const PassRegistration kCopy{"copy-propagation", makeCopyPropagation};
const PassRegistration kDce{"dce", makeDCE};
const PassRegistration kStrength{"strength-reduction", makeStrengthReduction};

}  // namespace

/// Referenced by the driver so the linker keeps this translation unit, and
/// with it the registrations above.
void anchorScalarPasses() {}

}  // namespace optiforge::transforms
