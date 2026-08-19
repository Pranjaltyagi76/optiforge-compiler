#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "optiforge/ir/Value.h"

namespace optiforge::ir {

class BasicBlock;
class Function;

enum class Opcode : std::uint8_t {
#define OPCODE(Name, Mnemonic, NumOperands, HasResult, IsTerminator) Name,
#include "optiforge/ir/Instruction.def"
#undef OPCODE
};

/// Comparison predicate carried by ICmp and FCmp. Signedness and float-ness are
/// implied by the opcode, so one enum serves both.
enum class Predicate : std::uint8_t { Eq, Ne, Lt, Gt, Le, Ge };

std::string_view toString(Opcode opcode);
std::string_view toString(Predicate predicate);

/// Declared operand count, or -1 for variadic instructions.
int declaredOperandCount(Opcode opcode);
bool opcodeHasResult(Opcode opcode);
bool opcodeIsTerminator(Opcode opcode);

/// One IR instruction.
///
/// Operands are values. Branch *targets* are held separately in `successors_`
/// rather than as operands, which keeps the operand list uniformly typed and
/// lets the CFG derive successors from the terminator without any parallel
/// bookkeeping to fall out of sync.
class Instruction final : public Value {
public:
  Instruction(Opcode opcode, const Type* type);
  ~Instruction() override;

  Opcode opcode() const { return opcode_; }
  /// Rewrites the operation in place, keeping operands and users. Used by
  /// strength reduction, which changes what an instruction computes with
  /// without changing what it reads.
  void setOpcode(Opcode opcode) { opcode_ = opcode; }
  bool isTerminator() const { return opcodeIsTerminator(opcode_); }
  bool hasResult() const { return opcodeHasResult(opcode_) && !type()->isVoid(); }

  BasicBlock* parent() const { return parent_; }
  void setParent(BasicBlock* parent) { parent_ = parent; }

  // --- Operands ---
  std::size_t operandCount() const { return operands_.size(); }
  Value* operand(std::size_t index) const { return operands_[index]; }
  const std::vector<Value*>& operands() const { return operands_; }

  void addOperand(Value* value);
  /// Repoints one operand slot, keeping both values' user lists correct.
  void setOperand(std::size_t index, Value* value);
  /// Drops this instruction from every operand's user list. Called before the
  /// instruction is destroyed so no dangling user pointers survive it.
  void dropAllReferences();

  /// Removes this instruction from its block and destroys it. Nothing may
  /// touch the instruction afterwards.
  void eraseFromParent();

  /// Moves this instruction into `block`, immediately before `before`
  /// (or to the end when `before` is null). Operands and users are untouched:
  /// only its position changes. Used by LICM to hoist into a preheader.
  void moveBefore(BasicBlock& block, Instruction* before);

  // --- Phi incoming edges ---
  //
  // A phi's operand i arrives from predecessor i. Stored in the same vector as
  // terminator successors because an instruction is never both a phi and a
  // terminator; the accessors below are the ones to use for phis, and the
  // verifier checks that the arity matches the block's predecessor count.
  void addIncoming(Value* value, BasicBlock* from);
  BasicBlock* incomingBlock(std::size_t index) const { return successors_[index]; }
  std::size_t incomingCount() const { return successors_.size(); }

  /// Drops the edge arriving from `from`, operand included. Called when a
  /// predecessor stops reaching this block, since a phi's arity must keep
  /// matching the predecessor list.
  void removeIncoming(const BasicBlock* from);

  /// Drops a *single* edge arriving from `from`. A terminator whose two
  /// successors are the same block contributes two predecessor entries and two
  /// phi operands; folding it to one branch removes one of each, not both.
  void removeOneIncoming(const BasicBlock* from);

  // --- Terminator successors ---
  const std::vector<BasicBlock*>& successors() const { return successors_; }
  void addSuccessor(BasicBlock* block) { successors_.push_back(block); }
  void setSuccessor(std::size_t index, BasicBlock* block) { successors_[index] = block; }

  // --- Comparison predicate (ICmp / FCmp only) ---
  Predicate predicate() const { return predicate_; }
  void setPredicate(Predicate predicate) { predicate_ = predicate; }

  // --- Call target (Call only) ---
  Function* callee() const { return callee_; }
  void setCallee(Function* callee) { callee_ = callee; }

  // --- Storage coalescing ---
  //
  // Values that must occupy the *same* location. SSA destruction turns one phi
  // into several copies, one per incoming edge, and every one of them has to
  // write where the phi's users will read. Naming them alike is not enough:
  // the backend allocates per instruction, so without this they would each get
  // their own slot and the phi's meaning would be lost.
  //
  // Null means "give this value its own location", which is the normal case.
  Instruction* slotAlias() const { return slotAlias_; }
  void setSlotAlias(Instruction* root) { slotAlias_ = root; }

  // --- Alloca: the type of the slot, not of the produced address ---
  const Type* allocatedType() const { return allocatedType_; }
  void setAllocatedType(const Type* type) { allocatedType_ = type; }

private:
  Opcode opcode_;
  BasicBlock* parent_ = nullptr;
  std::vector<Value*> operands_;
  std::vector<BasicBlock*> successors_;
  Predicate predicate_ = Predicate::Eq;
  Function* callee_ = nullptr;
  const Type* allocatedType_ = nullptr;
  Instruction* slotAlias_ = nullptr;
};

}  // namespace optiforge::ir
