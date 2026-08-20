#pragma once

#include <cstdint>

#include <memory>
#include <string>
#include <vector>

#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Instruction.h"
#include "optiforge/ir/Module.h"

namespace optiforge::ir {

/// Creates instructions at a tracked insertion point.
///
/// Centralizing creation here is what keeps naming deterministic and stops
/// every caller from re-deriving result types. It also folds trivially
/// constant operands on the spot, so `2 + 3` never reaches the optimizer.
class IRBuilder {
public:
  explicit IRBuilder(Module& module) : module_(module) {}

  void setInsertPoint(BasicBlock* block) { block_ = block; }
  BasicBlock* insertPoint() const { return block_; }
  Function* function() const { return block_ != nullptr ? block_->parent() : nullptr; }

  /// True when the current block already ends in a terminator, so further
  /// instructions would be unreachable. Callers use this to avoid emitting
  /// dead code after a `return`.
  bool atTerminatedBlock() const { return block_ != nullptr && block_->isTerminated(); }

  // --- Arithmetic and logic ---
  Value* createBinary(Opcode opcode, Value* lhs, Value* rhs);
  Value* createNeg(Value* operand);
  Value* createNot(Value* operand);
  Value* createCmp(Opcode opcode, Predicate predicate, Value* lhs, Value* rhs);
  Value* createSIToFP(Value* operand);

  // --- Memory ---
  /// Creates a stack slot in the current function's entry block, wherever the
  /// insertion point happens to be.
  ///
  /// Every alloca must live in the entry block: one inside a loop would grow
  /// the stack on each iteration, and mem2reg (Phase 6) only promotes
  /// entry-block slots. Keeping that rule here rather than at each call site
  /// is what stops it being forgotten -- it already was once, for variables
  /// declared inside a loop body.
  /// A stack slot for `count` consecutive elements of `allocatedType`.
  ///
  /// `count` defaults to 1, so every existing caller means what it always
  /// meant and arrays cost the scalar path nothing.
  Instruction* createEntryAlloca(const Type* allocatedType, const std::string& name,
                                 std::uint32_t count = 1);

  /// Address of element `index` of the array at `base` (Phase 13).
  ///
  /// `elementType` is what the resulting pointer points at, which the loads and
  /// stores built on top of it need in order to know their own type.
  Value* createGep(Value* base, Value* index, const Type* elementType);
  Value* createLoad(Value* address, const Type* resultType);
  void createStore(Value* value, Value* address);

  // --- Control flow ---
  void createBr(BasicBlock* target);
  void createCondBr(Value* condition, BasicBlock* ifTrue, BasicBlock* ifFalse);
  void createRet(Value* value);
  void createRetVoid();

  // --- Calls ---
  Value* createCall(Function* callee, const std::vector<Value*>& args);

  Module& module() { return module_; }

private:
  Instruction* insert(std::unique_ptr<Instruction> instruction, bool named);

  Module& module_;
  BasicBlock* block_ = nullptr;
};

}  // namespace optiforge::ir
