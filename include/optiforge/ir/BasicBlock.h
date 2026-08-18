#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "optiforge/ir/Instruction.h"

namespace optiforge::ir {

class Function;

/// A straight-line run of instructions with one entry and one exit.
///
/// Successors are *derived* from the terminator, so they can never go stale.
/// Predecessors have no reverse pointer to derive from and so are stored; every
/// mutation goes through append()/eraseTerminator(), which is the single place
/// the edges are maintained. The verifier independently recomputes predecessors
/// and compares, so a mistake here is caught rather than silently miscompiled.
class BasicBlock {
public:
  BasicBlock(std::string label, Function* parent)
      : label_(std::move(label)), parent_(parent) {}

  BasicBlock(const BasicBlock&) = delete;
  BasicBlock& operator=(const BasicBlock&) = delete;

  const std::string& label() const { return label_; }
  Function* parent() const { return parent_; }

  // --- Instructions ---
  const std::vector<std::unique_ptr<Instruction>>& instructions() const { return insts_; }
  bool empty() const { return insts_.empty(); }
  std::size_t size() const { return insts_.size(); }

  /// Appends and, when the instruction is a terminator, registers this block as
  /// a predecessor of each successor.
  Instruction* append(std::unique_ptr<Instruction> instruction);

  /// Inserts just before the terminator, or appends if the block is still
  /// open. Entry-block allocas need this: the entry block is terminated as soon
  /// as the function's first statement branches away, yet declarations found
  /// later still have to place their slots here.
  Instruction* insertBeforeTerminator(std::unique_ptr<Instruction> instruction);

  /// Inserts at the very start. Phi nodes must lead a block, which is what
  /// mem2reg needs when placing them.
  Instruction* insertAtTop(std::unique_ptr<Instruction> instruction);

  /// Inserts immediately after the last phi, which is the first position a
  /// non-phi instruction may legally occupy.
  Instruction* insertAfterPhis(std::unique_ptr<Instruction> instruction);

  /// Removes and destroys `instruction`, dropping its operand references first
  /// so no value is left listing it as a user.
  void erase(Instruction* instruction);

  /// Last instruction if it is a terminator, else null. A null result means the
  /// block is still open (during construction) or malformed (after).
  Instruction* terminator() const;
  bool isTerminated() const { return terminator() != nullptr; }

  /// Successors of this block, derived from its terminator.
  std::vector<BasicBlock*> successors() const;

  // --- Predecessors ---
  const std::vector<BasicBlock*>& predecessors() const { return preds_; }
  void addPredecessor(BasicBlock* block) { preds_.push_back(block); }
  void clearPredecessors() { preds_.clear(); }
  void removePredecessor(BasicBlock* block);

  /// Execution count from a profile, filled in Phase 10. Zero means "unknown",
  /// which is why it is not an optional: every consumer already treats 0 as
  /// "no information".
  std::uint64_t executionCount = 0;

private:
  std::string label_;
  Function* parent_;
  std::vector<std::unique_ptr<Instruction>> insts_;
  std::vector<BasicBlock*> preds_;
};

}  // namespace optiforge::ir
