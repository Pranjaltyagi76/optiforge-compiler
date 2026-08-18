#include "optiforge/ir/Verifier.h"

#include <algorithm>
#include <map>
#include <ostream>
#include <unordered_set>
#include <vector>

#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Instruction.h"
#include "optiforge/ir/Module.h"

namespace optiforge::ir {

namespace {

std::string describe(const Instruction& instruction) {
  std::string text(toString(instruction.opcode()));
  if (instruction.hasResult() && instruction.hasName()) {
    text = "%" + instruction.name() + " = " + text;
  }
  return text;
}

}  // namespace

void Verifier::report(const std::string& context, const std::string& message) {
  errors_.push_back(context + ": " + message);
}

bool Verifier::verify(const Module& module) {
  errors_.clear();
  for (const auto& function : module.functions()) {
    checkFunction(*function);
  }
  return errors_.empty();
}

bool Verifier::verify(const Function& function) {
  errors_.clear();
  checkFunction(function);
  return errors_.empty();
}

void Verifier::checkFunction(const Function& function) {
  if (function.isDeclaration()) {
    return;  // no body to check
  }

  const std::string fnContext = "function @" + function.name();

  // --- Entry block ---
  // isDeclaration() already implies a block exists, but the compiler cannot
  // see that through the container, and a missing entry block is worth
  // reporting as the structural error it would be.
  const BasicBlock* entry = function.entry();
  if (entry == nullptr) {
    report(fnContext, "function has a body but no entry block");
    return;
  }
  if (!entry->predecessors().empty()) {
    report(fnContext, "entry block '" + entry->label() +
                          "' has predecessors, which breaks dominance");
  }

  // --- Per-block structure ---
  std::unordered_set<const Instruction*> defined;
  for (const auto& block : function.blocks()) {
    const std::string context = fnContext + ", block '" + block->label() + "'";

    if (block->empty()) {
      report(context, "block is empty; every block must end in a terminator");
      continue;
    }

    // Exactly one terminator, and it must be last.
    std::size_t terminatorCount = 0;
    for (std::size_t i = 0; i < block->instructions().size(); ++i) {
      const Instruction& instruction = *block->instructions()[i];
      if (!instruction.isTerminator()) {
        continue;
      }
      ++terminatorCount;
      if (i + 1 != block->instructions().size()) {
        report(context, "terminator '" + std::string(toString(instruction.opcode())) +
                            "' is not the last instruction");
      }
    }
    if (terminatorCount == 0) {
      report(context, "block does not end in a terminator");
    }

    // Phi nodes may only appear at the top of a block.
    bool seenNonPhi = false;
    for (const auto& instruction : block->instructions()) {
      if (instruction->opcode() == Opcode::Phi) {
        if (seenNonPhi) {
          report(context, "phi node appears after a non-phi instruction");
        }
      } else {
        seenNonPhi = true;
      }
      defined.insert(instruction.get());
    }
  }

  // --- Operand and type checks ---
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block->instructions()) {
      const std::string context =
          fnContext + ", block '" + block->label() + "', " + describe(*instruction);

      const int expected = declaredOperandCount(instruction->opcode());
      if (expected >= 0 &&
          static_cast<std::size_t>(expected) != instruction->operandCount()) {
        report(context, "expects " + std::to_string(expected) + " operand(s) but has " +
                            std::to_string(instruction->operandCount()));
      }

      for (std::size_t i = 0; i < instruction->operandCount(); ++i) {
        const Value* operand = instruction->operand(i);
        if (operand == nullptr) {
          report(context, "operand " + std::to_string(i) + " is null");
          continue;
        }
        // A use of an instruction that is no longer in the function is a
        // dangling reference: memory corruption waiting to happen.
        if (operand->valueKind() == Value::Kind::Instruction &&
            defined.count(static_cast<const Instruction*>(operand)) == 0) {
          report(context, "operand " + std::to_string(i) +
                              " refers to an instruction that is not in this function");
        }
        // Every operand must record this instruction as a user.
        const auto& users = operand->users();
        if (std::find(users.begin(), users.end(), instruction.get()) == users.end()) {
          report(context, "operand " + std::to_string(i) +
                              " does not list this instruction as a user");
        }
      }

      switch (instruction->opcode()) {
        case Opcode::Add:
        case Opcode::Sub:
        case Opcode::Mul:
        case Opcode::SDiv:
        case Opcode::SRem:
          if (instruction->operandCount() == 2) {
            if (!instruction->operand(0)->type()->isI64() ||
                !instruction->operand(1)->type()->isI64()) {
              report(context, "integer arithmetic requires i64 operands");
            }
          }
          break;

        case Opcode::FAdd:
        case Opcode::FSub:
        case Opcode::FMul:
        case Opcode::FDiv:
          if (instruction->operandCount() == 2) {
            if (!instruction->operand(0)->type()->isF64() ||
                !instruction->operand(1)->type()->isF64()) {
              report(context, "floating-point arithmetic requires f64 operands");
            }
          }
          break;

        case Opcode::ICmp:
        case Opcode::FCmp:
          if (!instruction->type()->isI1()) {
            report(context, "comparison must produce i1");
          }
          if (instruction->operandCount() == 2 &&
              instruction->operand(0)->type() != instruction->operand(1)->type()) {
            report(context, "comparison operands have different types");
          }
          break;

        case Opcode::Not:
          if (instruction->operandCount() == 1 && !instruction->operand(0)->type()->isI1()) {
            report(context, "'not' requires an i1 operand");
          }
          break;

        case Opcode::SIToFP:
          if (instruction->operandCount() == 1 && !instruction->operand(0)->type()->isI64()) {
            report(context, "'sitofp' requires an i64 operand");
          }
          if (!instruction->type()->isF64()) {
            report(context, "'sitofp' must produce f64");
          }
          break;

        case Opcode::Alloca:
          if (!instruction->type()->isPtr()) {
            report(context, "'alloca' must produce a pointer");
          }
          if (instruction->allocatedType() == nullptr) {
            report(context, "'alloca' has no allocated type");
          }
          if (block.get() != entry) {
            // An alloca inside a loop would grow the stack every iteration, and
            // mem2reg only promotes entry-block allocas (Phase 6).
            report(context, "'alloca' must be in the entry block");
          }
          break;

        case Opcode::Load:
          if (instruction->operandCount() == 1 && !instruction->operand(0)->type()->isPtr()) {
            report(context, "'load' requires a pointer operand");
          }
          break;

        case Opcode::Store:
          if (instruction->operandCount() == 2 && !instruction->operand(1)->type()->isPtr()) {
            report(context, "'store' requires a pointer as its second operand");
          }
          break;

        case Opcode::CondBr:
          if (instruction->operandCount() == 1 && !instruction->operand(0)->type()->isI1()) {
            report(context, "'condbr' requires an i1 condition");
          }
          if (instruction->successors().size() != 2) {
            report(context, "'condbr' must have exactly two successors");
          }
          break;

        case Opcode::Br:
          if (instruction->successors().size() != 1) {
            report(context, "'br' must have exactly one successor");
          }
          break;

        case Opcode::Ret:
          if (function.returnType()->isVoid()) {
            if (instruction->operandCount() != 0) {
              report(context, "void function must not return a value");
            }
          } else {
            if (instruction->operandCount() != 1) {
              report(context, "non-void function must return exactly one value");
            } else if (instruction->operand(0)->type() != function.returnType()) {
              report(context, "returned value type does not match the function return type");
            }
          }
          break;

        case Opcode::Call:
          if (instruction->callee() == nullptr) {
            report(context, "'call' has no callee");
          } else {
            const Function& callee = *instruction->callee();
            if (instruction->operandCount() != callee.arguments().size()) {
              report(context, "call passes " + std::to_string(instruction->operandCount()) +
                                  " argument(s) but @" + callee.name() + " takes " +
                                  std::to_string(callee.arguments().size()));
            } else {
              for (std::size_t i = 0; i < instruction->operandCount(); ++i) {
                if (instruction->operand(i)->type() != callee.arguments()[i]->type()) {
                  report(context, "argument " + std::to_string(i) + " type does not match @" +
                                      callee.name());
                }
              }
            }
            if (instruction->type() != callee.returnType()) {
              report(context, "call result type does not match @" + callee.name());
            }
          }
          break;

        default:
          break;
      }
    }
  }

  // --- CFG consistency ---
  // The stored predecessor lists are maintained incrementally; recompute them
  // independently and compare, so a bookkeeping slip is caught here rather than
  // becoming a miscompile in a later phase.
  std::map<const BasicBlock*, std::vector<const BasicBlock*>> expected;
  for (const auto& block : function.blocks()) {
    expected[block.get()];  // ensure an entry exists even with no predecessors
  }
  for (const auto& block : function.blocks()) {
    for (const BasicBlock* successor : block->successors()) {
      expected[successor].push_back(block.get());
    }
  }

  for (const auto& block : function.blocks()) {
    std::vector<const BasicBlock*> actual(block->predecessors().begin(),
                                          block->predecessors().end());
    std::vector<const BasicBlock*> want = expected[block.get()];
    std::sort(actual.begin(), actual.end());
    std::sort(want.begin(), want.end());
    if (actual != want) {
      report(fnContext + ", block '" + block->label() + "'",
             "predecessor list is out of sync with the terminators that target it");
    }
  }

  // --- Reachability ---
  std::unordered_set<const BasicBlock*> reachable;
  std::vector<const BasicBlock*> worklist{entry};
  reachable.insert(entry);
  while (!worklist.empty()) {
    const BasicBlock* block = worklist.back();
    worklist.pop_back();
    for (const BasicBlock* successor : block->successors()) {
      if (reachable.insert(successor).second) {
        worklist.push_back(successor);
      }
    }
  }
  for (const auto& block : function.blocks()) {
    if (reachable.count(block.get()) == 0) {
      report(fnContext, "block '" + block->label() +
                            "' is unreachable and should have been pruned");
    }
  }
}

void Verifier::printErrors(std::ostream& out) const {
  for (const std::string& error : errors_) {
    out << "  " << error << '\n';
  }
}

}  // namespace optiforge::ir
