#pragma once

#include <memory>
#include <string>
#include <vector>

#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Value.h"

namespace optiforge::ir {

class Module;

/// One function: a signature plus a control-flow graph of basic blocks.
class Function {
public:
  Function(std::string name, const Type* returnType, Module* parent)
      : name_(std::move(name)), returnType_(returnType), parent_(parent) {}

  ~Function();

  Function(const Function&) = delete;
  Function& operator=(const Function&) = delete;

  /// Clears every instruction's operand list.
  ///
  /// Instructions reference each other through raw pointers held in user lists,
  /// and blocks are destroyed one at a time. Without this, destroying block 1
  /// leaves an instruction in block 2 pointing at freed memory, which its own
  /// destructor then writes to. Dropping every reference up front makes the
  /// order irrelevant.
  void dropAllReferences();

  const std::string& name() const { return name_; }
  const Type* returnType() const { return returnType_; }
  Module* parent() const { return parent_; }

  /// True for a declaration with no body, such as a runtime builtin.
  bool isDeclaration() const { return blocks_.empty(); }

  // --- Parameters ---
  Argument* addArgument(const Type* type, std::string name);
  const std::vector<std::unique_ptr<Argument>>& arguments() const { return args_; }

  // --- Blocks ---
  /// The entry block is always blocks_[0].
  BasicBlock* entry() const { return blocks_.empty() ? nullptr : blocks_.front().get(); }
  const std::vector<std::unique_ptr<BasicBlock>>& blocks() const { return blocks_; }

  /// Creates a block with a deterministic label.
  ///
  /// The first block is "entry"; every later one is "<prefix>.<n>" with a
  /// per-function counter. Requirement IR-11 makes this naming part of the
  /// contract rather than a printing detail: profile records are keyed by
  /// function:block, so a label that shifts between compilations makes every
  /// PGO decision silently miss (ADR-06).
  BasicBlock* createBlock(const std::string& prefix);

  /// Removes blocks not reachable from the entry block and rebuilds every
  /// predecessor list. Returns the number of blocks removed.
  std::size_t pruneUnreachableBlocks();

  /// Rebuilds all predecessor lists from the terminators. Cheap, and the
  /// authority the verifier compares the incremental lists against.
  void recomputePredecessors();

  /// Next "%tN" temporary name. Sequential per function, so identical input
  /// yields identical output (NFR-06).
  std::string nextTempName();

  // --- Stack slots ---
  /// Entry-block allocas, in creation order. mem2reg (Phase 6) promotes these.
  std::vector<Instruction*> allocas() const;

private:
  std::string name_;
  const Type* returnType_;
  Module* parent_;
  std::vector<std::unique_ptr<Argument>> args_;
  std::vector<std::unique_ptr<BasicBlock>> blocks_;
  unsigned nextBlockId_ = 1;
  unsigned nextTempId_ = 0;
};

}  // namespace optiforge::ir
