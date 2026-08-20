#include "optiforge/ir/IRBuilder.h"

#include <utility>

namespace optiforge::ir {

namespace {

/// Result type of a binary opcode given its operand type.
const Type* binaryResultType(Opcode opcode, const Type* operandType) {
  switch (opcode) {
    case Opcode::FAdd:
    case Opcode::FSub:
    case Opcode::FMul:
    case Opcode::FDiv:
      return Type::getF64();
    default:
      return operandType;
  }
}

}  // namespace

Instruction* IRBuilder::insert(std::unique_ptr<Instruction> instruction, bool named) {
  Instruction* raw = instruction.get();
  if (named && raw->hasResult()) {
    raw->setName(block_->parent()->nextTempName());
  }
  return block_->append(std::move(instruction));
}

Value* IRBuilder::createBinary(Opcode opcode, Value* lhs, Value* rhs) {
  // Fold now rather than leaving it for a later pass: constants produced by
  // lowering (array offsets, promoted literals) would otherwise clutter the IR
  // that every subsequent stage has to read.
  if (lhs->valueKind() == Value::Kind::ConstantInt &&
      rhs->valueKind() == Value::Kind::ConstantInt) {
    const std::int64_t a = static_cast<ConstantInt*>(lhs)->value();
    const std::int64_t b = static_cast<ConstantInt*>(rhs)->value();
    switch (opcode) {
      case Opcode::Add:
        return module_.getInt(a + b);
      case Opcode::Sub:
        return module_.getInt(a - b);
      case Opcode::Mul:
        return module_.getInt(a * b);
      case Opcode::SDiv:
        // Division by zero and INT64_MIN / -1 are left for the target to trap
        // on; folding them here would change observable behaviour.
        if (b != 0 && !(a == INT64_MIN && b == -1)) {
          return module_.getInt(a / b);
        }
        break;
      case Opcode::SRem:
        if (b != 0 && !(a == INT64_MIN && b == -1)) {
          return module_.getInt(a % b);
        }
        break;
      default:
        break;
    }
  }

  auto instruction =
      std::make_unique<Instruction>(opcode, binaryResultType(opcode, lhs->type()));
  instruction->addOperand(lhs);
  instruction->addOperand(rhs);
  return insert(std::move(instruction), /*named=*/true);
}

Value* IRBuilder::createNeg(Value* operand) {
  const bool isFloat = operand->type()->isFloat();
  auto instruction =
      std::make_unique<Instruction>(isFloat ? Opcode::FNeg : Opcode::Neg, operand->type());
  instruction->addOperand(operand);
  return insert(std::move(instruction), /*named=*/true);
}

Value* IRBuilder::createNot(Value* operand) {
  auto instruction = std::make_unique<Instruction>(Opcode::Not, Type::getI1());
  instruction->addOperand(operand);
  return insert(std::move(instruction), /*named=*/true);
}

Value* IRBuilder::createCmp(Opcode opcode, Predicate predicate, Value* lhs, Value* rhs) {
  auto instruction = std::make_unique<Instruction>(opcode, Type::getI1());
  instruction->setPredicate(predicate);
  instruction->addOperand(lhs);
  instruction->addOperand(rhs);
  return insert(std::move(instruction), /*named=*/true);
}

Value* IRBuilder::createSIToFP(Value* operand) {
  // Converting a literal at run time is pure waste; fold it to a float
  // constant here so the IR never carries `sitofp i64 2`.
  if (operand->valueKind() == Value::Kind::ConstantInt) {
    return module_.getFloat(static_cast<double>(static_cast<ConstantInt*>(operand)->value()));
  }

  auto instruction = std::make_unique<Instruction>(Opcode::SIToFP, Type::getF64());
  instruction->addOperand(operand);
  return insert(std::move(instruction), /*named=*/true);
}

Instruction* IRBuilder::createEntryAlloca(const Type* allocatedType,
                                          const std::string& name,
                                          std::uint32_t count) {
  auto instruction = std::make_unique<Instruction>(Opcode::Alloca, Type::getPtr());
  instruction->setAllocatedType(allocatedType);
  instruction->setAllocatedCount(count);
  instruction->setName(name + ".addr");

  // Both null checks are real, not defensive noise: -O3 inlines far enough to
  // prove the dereference is reachable, and a builder with no insertion point
  // has no entry block to put a slot in.
  Function* parent = function();
  if (parent == nullptr) {
    return nullptr;
  }
  BasicBlock* entry = parent->entry();
  if (entry == nullptr) {
    return nullptr;
  }

  // The entry block is already terminated once the first statement branches
  // away, so appending would put the alloca after its terminator.
  return entry->insertBeforeTerminator(std::move(instruction));
}

Value* IRBuilder::createGep(Value* base, Value* index, const Type* elementType) {
  auto instruction = std::make_unique<Instruction>(Opcode::Gep, Type::getPtr());
  instruction->setAllocatedType(elementType);
  instruction->addOperand(base);
  instruction->addOperand(index);
  return insert(std::move(instruction), /*named=*/true);
}

Value* IRBuilder::createLoad(Value* address, const Type* resultType) {
  auto instruction = std::make_unique<Instruction>(Opcode::Load, resultType);
  instruction->addOperand(address);
  return insert(std::move(instruction), /*named=*/true);
}

void IRBuilder::createStore(Value* value, Value* address) {
  auto instruction = std::make_unique<Instruction>(Opcode::Store, Type::getVoid());
  instruction->addOperand(value);
  instruction->addOperand(address);
  insert(std::move(instruction), /*named=*/false);
}

void IRBuilder::createBr(BasicBlock* target) {
  auto instruction = std::make_unique<Instruction>(Opcode::Br, Type::getVoid());
  instruction->addSuccessor(target);
  insert(std::move(instruction), /*named=*/false);
}

void IRBuilder::createCondBr(Value* condition, BasicBlock* ifTrue, BasicBlock* ifFalse) {
  auto instruction = std::make_unique<Instruction>(Opcode::CondBr, Type::getVoid());
  instruction->addOperand(condition);
  instruction->addSuccessor(ifTrue);
  instruction->addSuccessor(ifFalse);
  insert(std::move(instruction), /*named=*/false);
}

void IRBuilder::createRet(Value* value) {
  auto instruction = std::make_unique<Instruction>(Opcode::Ret, Type::getVoid());
  instruction->addOperand(value);
  insert(std::move(instruction), /*named=*/false);
}

void IRBuilder::createRetVoid() {
  auto instruction = std::make_unique<Instruction>(Opcode::Ret, Type::getVoid());
  insert(std::move(instruction), /*named=*/false);
}

Value* IRBuilder::createCall(Function* callee, const std::vector<Value*>& args) {
  auto instruction = std::make_unique<Instruction>(Opcode::Call, callee->returnType());
  instruction->setCallee(callee);
  for (Value* arg : args) {
    instruction->addOperand(arg);
  }
  // A void call has no result, so it gets no name.
  return insert(std::move(instruction), /*named=*/!callee->returnType()->isVoid());
}

}  // namespace optiforge::ir
