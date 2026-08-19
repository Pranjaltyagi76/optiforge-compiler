#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <utility>

#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Instruction.h"
#include "optiforge/ir/Module.h"
#include "optiforge/ir/Type.h"
#include "optiforge/ir/Value.h"

namespace optiforge::ir {

// ---------------------------------------------------------------------------
// Type
// ---------------------------------------------------------------------------

std::string_view Type::name() const {
  switch (kind_) {
    case Kind::Void:
      return "void";
    case Kind::I1:
      return "i1";
    case Kind::I64:
      return "i64";
    case Kind::F64:
      return "f64";
    case Kind::Ptr:
      return "ptr";
  }
  return "<unknown>";
}

unsigned Type::sizeInBytes() const {
  switch (kind_) {
    case Kind::Void:
      return 0;
    case Kind::I1:
      return 1;
    case Kind::I64:
    case Kind::F64:
    case Kind::Ptr:
      return 8;
  }
  return 0;
}

const Type* Type::get(Kind kind) {
  static const Type kVoid{Kind::Void};
  static const Type kI1{Kind::I1};
  static const Type kI64{Kind::I64};
  static const Type kF64{Kind::F64};
  static const Type kPtr{Kind::Ptr};

  switch (kind) {
    case Kind::Void:
      return &kVoid;
    case Kind::I1:
      return &kI1;
    case Kind::I64:
      return &kI64;
    case Kind::F64:
      return &kF64;
    case Kind::Ptr:
      return &kPtr;
  }
  return &kVoid;
}

// ---------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------

void Value::removeUser(Instruction* user) {
  // Remove one occurrence only: an instruction using the same value twice
  // appears twice, and dropping both here would lose a live reference.
  const auto it = std::find(users_.begin(), users_.end(), user);
  if (it != users_.end()) {
    users_.erase(it);
  }
}

void Value::replaceAllUsesWith(Value* newValue) {
  if (newValue == this) {
    return;
  }
  // Copy first: rewriting an operand mutates users_ underneath us.
  const std::vector<Instruction*> snapshot = users_;
  for (Instruction* user : snapshot) {
    for (std::size_t i = 0; i < user->operandCount(); ++i) {
      if (user->operand(i) == this) {
        user->setOperand(i, newValue);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Instruction
// ---------------------------------------------------------------------------

namespace {

struct OpcodeInfo {
  const char* mnemonic;
  int numOperands;
  bool hasResult;
  bool isTerminator;
};

const OpcodeInfo& opcodeInfo(Opcode opcode) {
  static const OpcodeInfo kTable[] = {
#define OPCODE(Name, Mnemonic, NumOperands, HasResult, IsTerminator) \
  {Mnemonic, NumOperands, HasResult, IsTerminator},
#include "optiforge/ir/Instruction.def"
#undef OPCODE
  };
  return kTable[static_cast<std::size_t>(opcode)];
}

}  // namespace

std::string_view toString(Opcode opcode) { return opcodeInfo(opcode).mnemonic; }

int declaredOperandCount(Opcode opcode) { return opcodeInfo(opcode).numOperands; }
bool opcodeHasResult(Opcode opcode) { return opcodeInfo(opcode).hasResult; }
bool opcodeIsTerminator(Opcode opcode) { return opcodeInfo(opcode).isTerminator; }

std::string_view toString(Predicate predicate) {
  switch (predicate) {
    case Predicate::Eq:
      return "eq";
    case Predicate::Ne:
      return "ne";
    case Predicate::Lt:
      return "lt";
    case Predicate::Gt:
      return "gt";
    case Predicate::Le:
      return "le";
    case Predicate::Ge:
      return "ge";
  }
  return "??";
}

Instruction::Instruction(Opcode opcode, const Type* type)
    : Value(Value::Kind::Instruction, type), opcode_(opcode) {}

Instruction::~Instruction() { dropAllReferences(); }

void Instruction::addOperand(Value* value) {
  operands_.push_back(value);
  if (value != nullptr) {
    value->addUser(this);
  }
}

void Instruction::setOperand(std::size_t index, Value* value) {
  Value* previous = operands_[index];
  if (previous == value) {
    return;
  }
  if (previous != nullptr) {
    previous->removeUser(this);
  }
  operands_[index] = value;
  if (value != nullptr) {
    value->addUser(this);
  }
}

void Instruction::eraseFromParent() {
  if (parent_ != nullptr) {
    parent_->erase(this);  // destroys *this
  }
}

void Instruction::addIncoming(Value* value, BasicBlock* from) {
  addOperand(value);
  successors_.push_back(from);
}

void Instruction::moveBefore(BasicBlock& block, Instruction* before) {
  // detach() hands back ownership. Discarding the returned unique_ptr would
  // destroy this instruction at the end of the statement and leave the
  // reinsertion below operating on freed memory -- which is exactly what it
  // did, corrupting the heap.
  std::unique_ptr<Instruction> owned;
  if (parent_ != nullptr) {
    owned = parent_->detach(this);
  }
  if (owned == nullptr) {
    owned.reset(this);  // was not in a block to begin with
  }
  block.insertBefore(std::move(owned), before);
}

void Instruction::removeIncoming(const BasicBlock* from) {
  for (std::size_t i = 0; i < successors_.size();) {
    if (successors_[i] != from) {
      ++i;
      continue;
    }
    if (operands_[i] != nullptr) {
      operands_[i]->removeUser(this);
    }
    operands_.erase(operands_.begin() + static_cast<std::ptrdiff_t>(i));
    successors_.erase(successors_.begin() + static_cast<std::ptrdiff_t>(i));
  }
}

void Instruction::removeOneIncoming(const BasicBlock* from) {
  for (std::size_t i = 0; i < successors_.size(); ++i) {
    if (successors_[i] != from) {
      continue;
    }
    if (operands_[i] != nullptr) {
      operands_[i]->removeUser(this);
    }
    operands_.erase(operands_.begin() + static_cast<std::ptrdiff_t>(i));
    successors_.erase(successors_.begin() + static_cast<std::ptrdiff_t>(i));
    return;
  }
}

void Instruction::dropAllReferences() {
  for (Value*& operand : operands_) {
    if (operand != nullptr) {
      operand->removeUser(this);
      operand = nullptr;
    }
  }
}

// ---------------------------------------------------------------------------
// BasicBlock
// ---------------------------------------------------------------------------

Instruction* BasicBlock::append(std::unique_ptr<Instruction> instruction) {
  Instruction* raw = instruction.get();
  raw->setParent(this);
  insts_.push_back(std::move(instruction));

  // The one place CFG edges are created.
  if (raw->isTerminator()) {
    for (BasicBlock* successor : raw->successors()) {
      successor->addPredecessor(this);
    }
  }
  return raw;
}

Instruction* BasicBlock::insertBeforeTerminator(std::unique_ptr<Instruction> instruction) {
  Instruction* raw = instruction.get();
  raw->setParent(this);

  // Only non-terminators may be inserted this way, so no CFG edges change.
  if (terminator() != nullptr) {
    insts_.insert(insts_.end() - 1, std::move(instruction));
  } else {
    insts_.push_back(std::move(instruction));
  }
  return raw;
}

Instruction* BasicBlock::insertAtTop(std::unique_ptr<Instruction> instruction) {
  Instruction* raw = instruction.get();
  raw->setParent(this);
  insts_.insert(insts_.begin(), std::move(instruction));
  return raw;
}

Instruction* BasicBlock::insertAfterPhis(std::unique_ptr<Instruction> instruction) {
  Instruction* raw = instruction.get();
  raw->setParent(this);
  std::size_t position = 0;
  while (position < insts_.size() && insts_[position]->opcode() == Opcode::Phi) {
    ++position;
  }
  insts_.insert(insts_.begin() + static_cast<std::ptrdiff_t>(position),
                std::move(instruction));
  return raw;
}

std::unique_ptr<Instruction> BasicBlock::detach(Instruction* instruction) {
  for (auto it = insts_.begin(); it != insts_.end(); ++it) {
    if (it->get() == instruction) {
      std::unique_ptr<Instruction> owned = std::move(*it);
      insts_.erase(it);
      owned->setParent(nullptr);
      return owned;
    }
  }
  return nullptr;
}

Instruction* BasicBlock::insertBefore(std::unique_ptr<Instruction> instruction,
                                      Instruction* before) {
  Instruction* raw = instruction.get();
  raw->setParent(this);
  if (before == nullptr) {
    insts_.push_back(std::move(instruction));
    return raw;
  }
  for (auto it = insts_.begin(); it != insts_.end(); ++it) {
    if (it->get() == before) {
      insts_.insert(it, std::move(instruction));
      return raw;
    }
  }
  insts_.push_back(std::move(instruction));
  return raw;
}

void BasicBlock::erase(Instruction* instruction) {
  // Drop operand references before destruction so no surviving value keeps
  // this instruction in its user list.
  instruction->dropAllReferences();
  for (auto it = insts_.begin(); it != insts_.end(); ++it) {
    if (it->get() == instruction) {
      insts_.erase(it);
      return;
    }
  }
}

Instruction* BasicBlock::terminator() const {
  if (insts_.empty()) {
    return nullptr;
  }
  Instruction* last = insts_.back().get();
  return last->isTerminator() ? last : nullptr;
}

std::vector<BasicBlock*> BasicBlock::successors() const {
  const Instruction* term = terminator();
  return term != nullptr ? term->successors() : std::vector<BasicBlock*>{};
}

void BasicBlock::removePredecessor(BasicBlock* block) {
  const auto it = std::find(preds_.begin(), preds_.end(), block);
  if (it != preds_.end()) {
    preds_.erase(it);
  }
}

// ---------------------------------------------------------------------------
// Function
// ---------------------------------------------------------------------------

Function::~Function() { dropAllReferences(); }

void Function::dropAllReferences() {
  for (auto& block : blocks_) {
    for (const auto& instruction : block->instructions()) {
      instruction->dropAllReferences();
    }
  }
}

Argument* Function::addArgument(const Type* type, std::string name) {
  auto argument =
      std::make_unique<Argument>(type, std::move(name), static_cast<unsigned>(args_.size()));
  Argument* raw = argument.get();
  args_.push_back(std::move(argument));
  return raw;
}

BasicBlock* Function::createBlock(const std::string& prefix) {
  // The entry block keeps a bare name; everything else is numbered in creation
  // order so the labels are reproducible across compilations (IR-11).
  std::string label =
      blocks_.empty() ? std::string("entry") : prefix + "." + std::to_string(nextBlockId_++);
  auto block = std::make_unique<BasicBlock>(std::move(label), this);
  BasicBlock* raw = block.get();
  blocks_.push_back(std::move(block));
  return raw;
}

std::string Function::nextTempName() { return "t" + std::to_string(nextTempId_++); }

void Function::recomputePredecessors() {
  for (auto& block : blocks_) {
    block->clearPredecessors();
  }
  for (auto& block : blocks_) {
    for (BasicBlock* successor : block->successors()) {
      successor->addPredecessor(block.get());
    }
  }
}

std::size_t Function::pruneUnreachableBlocks() {
  if (blocks_.empty()) {
    return 0;
  }

  std::unordered_set<const BasicBlock*> reachable;
  std::vector<BasicBlock*> worklist{entry()};
  reachable.insert(entry());
  while (!worklist.empty()) {
    BasicBlock* block = worklist.back();
    worklist.pop_back();
    for (BasicBlock* successor : block->successors()) {
      if (reachable.insert(successor).second) {
        worklist.push_back(successor);
      }
    }
  }

  if (reachable.size() == blocks_.size()) {
    return 0;
  }

  // Detach operands before destroying anything: a surviving instruction must
  // never be left in a dead instruction's user list.
  for (auto& block : blocks_) {
    if (reachable.count(block.get()) == 0) {
      for (auto& instruction : block->instructions()) {
        instruction->dropAllReferences();
      }
    }
  }

  const std::size_t before = blocks_.size();
  blocks_.erase(std::remove_if(blocks_.begin(), blocks_.end(),
                               [&](const std::unique_ptr<BasicBlock>& block) {
                                 return reachable.count(block.get()) == 0;
                               }),
                blocks_.end());

  recomputePredecessors();
  return before - blocks_.size();
}

std::vector<Instruction*> Function::allocas() const {
  std::vector<Instruction*> result;
  if (BasicBlock* block = entry()) {
    for (const auto& instruction : block->instructions()) {
      if (instruction->opcode() == Opcode::Alloca) {
        result.push_back(instruction.get());
      }
    }
  }
  return result;
}

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------

Function* Module::createFunction(const std::string& name, const Type* returnType) {
  auto function = std::make_unique<Function>(name, returnType, this);
  Function* raw = function.get();
  functions_.push_back(std::move(function));
  functionsByName_.emplace(name, raw);
  return raw;
}

Function* Module::findFunction(const std::string& name) const {
  const auto it = functionsByName_.find(name);
  return it != functionsByName_.end() ? it->second : nullptr;
}

void Module::eraseFunction(Function* function) {
  functionsByName_.erase(function->name());
  // Drop operand references before the blocks are destroyed, exactly as
  // ~Function does, so no surviving value keeps a dead instruction as a user.
  function->dropAllReferences();
  for (auto it = functions_.begin(); it != functions_.end(); ++it) {
    if (it->get() == function) {
      functions_.erase(it);
      return;
    }
  }
}

ConstantInt* Module::getInt(std::int64_t value) {
  const auto it = intConstants_.find(value);
  if (it != intConstants_.end()) {
    return it->second;
  }
  intStorage_.emplace_back(Type::getI64(), value);
  ConstantInt* constant = &intStorage_.back();
  intConstants_.emplace(value, constant);
  return constant;
}

ConstantFloat* Module::getFloat(double value) {
  // Key on the bit pattern so -0.0 and 0.0 stay distinct and NaN is stable.
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));

  const auto it = floatConstants_.find(bits);
  if (it != floatConstants_.end()) {
    return it->second;
  }
  floatStorage_.emplace_back(Type::getF64(), value);
  ConstantFloat* constant = &floatStorage_.back();
  floatConstants_.emplace(bits, constant);
  return constant;
}

ConstantBool* Module::getBool(bool value) {
  ConstantBool*& slot = value ? trueConstant_ : falseConstant_;
  if (slot == nullptr) {
    boolStorage_.emplace_back(Type::getI1(), value);
    slot = &boolStorage_.back();
  }
  return slot;
}

}  // namespace optiforge::ir
