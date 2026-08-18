#include "optiforge/backend/CodeGen.h"

#include <cstring>
#include <utility>

#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Instruction.h"
#include "optiforge/ir/Module.h"

namespace optiforge::backend {

using ir::Opcode;
using ir::Predicate;

namespace {

std::int32_t alignUp(std::int32_t value, std::int32_t alignment) {
  const std::int32_t remainder = value % alignment;
  return remainder == 0 ? value : value + (alignment - remainder);
}

/// Assembly labels may not contain '.', which IR block labels do.
std::string sanitize(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    out += (c == '.' || c == '-') ? '_' : c;
  }
  return out;
}

const char* setccForInt(Predicate predicate) {
  switch (predicate) {
    case Predicate::Eq: return "sete";
    case Predicate::Ne: return "setne";
    case Predicate::Lt: return "setl";
    case Predicate::Gt: return "setg";
    case Predicate::Le: return "setle";
    case Predicate::Ge: return "setge";
  }
  return "sete";
}

/// comisd sets the flags as an *unsigned* comparison would, so the float forms
/// are the above/below family rather than the greater/less one.
const char* setccForFloat(Predicate predicate) {
  switch (predicate) {
    case Predicate::Eq: return "sete";
    case Predicate::Ne: return "setne";
    case Predicate::Lt: return "setb";
    case Predicate::Gt: return "seta";
    case Predicate::Le: return "setbe";
    case Predicate::Ge: return "setae";
  }
  return "sete";
}

std::uint64_t bitsOf(double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

}  // namespace

// ---------------------------------------------------------------------------
// Emission helpers
// ---------------------------------------------------------------------------

void CodeGen::emit(const char* mnemonic, std::vector<MOperand> operands,
                   std::string comment) {
  MInstr instruction;
  instruction.mnemonic = mnemonic;
  instruction.operands = std::move(operands);
  instruction.comment = std::move(comment);
  block_->instructions.push_back(std::move(instruction));
}

void CodeGen::emitLabel(std::string label) {
  MInstr instruction;
  instruction.isLabel = true;
  instruction.comment = std::move(label);
  block_->instructions.push_back(std::move(instruction));
}

std::string CodeGen::blockLabel(const ir::BasicBlock& block) const {
  return ".L_" + sanitize(irFunction_->name()) + "_" + sanitize(block.label());
}

std::string CodeGen::floatConstantLabel(double value) {
  const std::uint64_t bits = bitsOf(value);
  const auto it = floatLabels_.find(bits);
  if (it != floatLabels_.end()) {
    return it->second;
  }
  const std::string label = ".LCF" + std::to_string(floatLabels_.size());
  floatLabels_.emplace(bits, label);
  module_->floatConstants.push_back({label, value});
  return label;
}

// ---------------------------------------------------------------------------
// Frame layout
// ---------------------------------------------------------------------------

bool CodeGen::slotOf(const ir::Value* value, std::int32_t& offset) const {
  const auto it = slots_.find(value);
  if (it == slots_.end()) {
    return false;
  }
  offset = it->second;
  return true;
}

bool CodeGen::directSlot(const ir::Value* value, std::int32_t& offset) const {
  if (value == nullptr || value->valueKind() != ir::Value::Kind::Instruction) {
    return false;
  }
  const auto* instruction = static_cast<const ir::Instruction*>(value);
  if (instruction->opcode() != Opcode::Alloca) {
    return false;
  }
  return slotOf(value, offset);
}

void CodeGen::assignSlots(const ir::Function& function) {
  slots_.clear();
  std::int32_t offset = 0;

  // Incoming arguments are copied into slots by the prologue, so the rest of
  // the generator can treat them exactly like any other value.
  for (const auto& argument : function.arguments()) {
    offset += 8;
    slots_[argument.get()] = -offset;
  }

  std::size_t maxCallArgs = 0;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block->instructions()) {
      if (instruction->opcode() == Opcode::Call) {
        maxCallArgs = std::max(maxCallArgs, instruction->operandCount());
      }
      // An alloca's slot holds the storage; every other result's slot holds
      // the value itself. Values coalesced onto another value's slot are
      // handled in a second pass, once their root has an address.
      // A value aliased onto *another* value's slot is handled below. A value
      // aliased to itself is the coalescing root and needs a real slot.
      if (instruction->slotAlias() != nullptr &&
          instruction->slotAlias() != instruction.get()) {
        continue;
      }
      if (instruction->opcode() == Opcode::Alloca || instruction->hasResult()) {
        offset += 8;
        slots_[instruction.get()] = -offset;
      }
    }
  }

  // Second pass: coalesced values share their root's slot. This is what makes
  // SSA destruction work -- the copies on every incoming edge of a phi all
  // write the one location the phi's users read.
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block->instructions()) {
      const ir::Instruction* root = instruction->slotAlias();
      if (root == nullptr || root == instruction.get()) {
        continue;
      }
      const auto it = slots_.find(root);
      if (it != slots_.end()) {
        slots_[instruction.get()] = it->second;
      }
    }
  }

  localBytes_ = offset;

  // Space for outgoing arguments: shadow space always, plus one slot for each
  // argument past the fourth.
  outgoingBytes_ = target_.shadowSpaceBytes();
  if (maxCallArgs > target_.maxRegisterArgs()) {
    outgoingBytes_ +=
        static_cast<std::int32_t>(maxCallArgs - target_.maxRegisterArgs()) * 8;
  }

  function_->frameSize =
      alignUp(localBytes_ + outgoingBytes_, target_.stackAlignment());
}

void CodeGen::emitPrologue(const ir::Function& function) {
  emit("pushq", {MOperand::makeReg(MReg::RBP)});
  emit("movq", {MOperand::makeReg(MReg::RSP), MOperand::makeReg(MReg::RBP, true)});
  if (function_->frameSize > 0) {
    emit("subq",
         {MOperand::makeImm(function_->frameSize), MOperand::makeReg(MReg::RSP, true)},
         "frame: " + std::to_string(localBytes_) + " local + " +
             std::to_string(outgoingBytes_) + " outgoing");
  }

  // Copy incoming arguments into their slots.
  for (std::size_t i = 0; i < function.arguments().size(); ++i) {
    const ir::Argument& argument = *function.arguments()[i];
    std::int32_t slot = 0;
    if (!slotOf(&argument, slot)) {
      continue;
    }
    const bool isFloat = argument.type()->isF64();

    if (i < target_.maxRegisterArgs()) {
      const MReg source =
          isFloat ? target_.floatArgRegister(static_cast<unsigned>(i))
                  : target_.integerArgRegister(static_cast<unsigned>(i));
      emit(isFloat ? "movsd" : "movq",
           {MOperand::makeReg(source), MOperand::makeMem(MReg::RBP, slot)},
           "arg " + argument.name());
    } else {
      // Stack arguments sit above the return address and saved rbp, past the
      // caller's 32-byte shadow space: rbp + 16 + 32 + 8*(i-4).
      const std::int32_t incoming =
          16 + target_.shadowSpaceBytes() +
          static_cast<std::int32_t>(i - target_.maxRegisterArgs()) * 8;
      const MReg scratch = isFloat ? target_.scratchFloat0() : target_.scratchInt0();
      emit(isFloat ? "movsd" : "movq",
           {MOperand::makeMem(MReg::RBP, incoming), MOperand::makeReg(scratch, true)});
      emit(isFloat ? "movsd" : "movq",
           {MOperand::makeReg(scratch), MOperand::makeMem(MReg::RBP, slot)},
           "arg " + argument.name());
    }
  }
}

void CodeGen::emitEpilogue() {
  emit("movq", {MOperand::makeReg(MReg::RBP), MOperand::makeReg(MReg::RSP, true)});
  emit("popq", {MOperand::makeReg(MReg::RBP, true)});
  emit("ret", {});
}

// ---------------------------------------------------------------------------
// Value movement
// ---------------------------------------------------------------------------

void CodeGen::loadInt(const ir::Value* value, MReg destination) {
  switch (value->valueKind()) {
    case ir::Value::Kind::ConstantInt:
      emit("movq",
           {MOperand::makeImm(static_cast<const ir::ConstantInt*>(value)->value()),
            MOperand::makeReg(destination, true)});
      return;

    case ir::Value::Kind::ConstantBool:
      emit("movq",
           {MOperand::makeImm(static_cast<const ir::ConstantBool*>(value)->value() ? 1 : 0),
            MOperand::makeReg(destination, true)});
      return;

    default:
      break;
  }

  std::int32_t offset = 0;
  if (directSlot(value, offset)) {
    // The value of an alloca is the *address* of its slot.
    emit("leaq", {MOperand::makeMem(MReg::RBP, offset),
                  MOperand::makeReg(destination, true)});
    return;
  }
  if (slotOf(value, offset)) {
    emit("movq", {MOperand::makeMem(MReg::RBP, offset),
                  MOperand::makeReg(destination, true)});
    return;
  }
  // Unreachable for verified IR; emit something harmless rather than crash.
  emit("movq", {MOperand::makeImm(0), MOperand::makeReg(destination, true)},
       "<unmapped value>");
}

void CodeGen::loadFloat(const ir::Value* value, MReg destination) {
  if (value->valueKind() == ir::Value::Kind::ConstantFloat) {
    const double constant = static_cast<const ir::ConstantFloat*>(value)->value();
    emit("movsd", {MOperand::makeRipLabel(floatConstantLabel(constant)),
                   MOperand::makeReg(destination, true)});
    return;
  }

  std::int32_t offset = 0;
  if (slotOf(value, offset)) {
    emit("movsd", {MOperand::makeMem(MReg::RBP, offset),
                   MOperand::makeReg(destination, true)});
    return;
  }
  emit("pxor", {MOperand::makeReg(destination), MOperand::makeReg(destination, true)},
       "<unmapped value>");
}

void CodeGen::storeResult(const ir::Instruction& instruction, MReg source) {
  std::int32_t offset = 0;
  if (!slotOf(&instruction, offset)) {
    return;
  }
  emit(isFloatReg(source) ? "movsd" : "movq",
       {MOperand::makeReg(source), MOperand::makeMem(MReg::RBP, offset)});
}

// ---------------------------------------------------------------------------
// Instruction lowering
// ---------------------------------------------------------------------------

void CodeGen::lowerBinaryInt(const ir::Instruction& instruction, const char* mnemonic) {
  const MReg lhs = target_.scratchInt0();
  const MReg rhs = target_.scratchInt1();
  loadInt(instruction.operand(0), lhs);
  loadInt(instruction.operand(1), rhs);
  emit(mnemonic, {MOperand::makeReg(rhs), MOperand::makeReg(lhs, true)});
  storeResult(instruction, lhs);
}

void CodeGen::lowerBinaryFloat(const ir::Instruction& instruction, const char* mnemonic) {
  const MReg lhs = target_.scratchFloat0();
  const MReg rhs = target_.scratchFloat1();
  loadFloat(instruction.operand(0), lhs);
  loadFloat(instruction.operand(1), rhs);
  emit(mnemonic, {MOperand::makeReg(rhs), MOperand::makeReg(lhs, true)});
  storeResult(instruction, lhs);
}

void CodeGen::lowerDivRem(const ir::Instruction& instruction, bool wantRemainder) {
  // idiv is fixed to rdx:rax for the dividend and leaves the quotient in rax
  // and the remainder in rdx, so the operands cannot be placed freely.
  loadInt(instruction.operand(0), MReg::RAX);
  loadInt(instruction.operand(1), MReg::R10);
  emit("cqto", {}, "sign-extend rax into rdx:rax");
  emit("idivq", {MOperand::makeReg(MReg::R10)});
  storeResult(instruction, wantRemainder ? MReg::RDX : MReg::RAX);
}

void CodeGen::lowerCompare(const ir::Instruction& instruction, bool isFloat) {
  if (isFloat) {
    const MReg lhs = target_.scratchFloat0();
    const MReg rhs = target_.scratchFloat1();
    loadFloat(instruction.operand(0), lhs);
    loadFloat(instruction.operand(1), rhs);
    emit("comisd", {MOperand::makeReg(rhs), MOperand::makeReg(lhs)});
  } else {
    const MReg lhs = target_.scratchInt0();
    const MReg rhs = target_.scratchInt1();
    loadInt(instruction.operand(0), lhs);
    loadInt(instruction.operand(1), rhs);
    emit("cmpq", {MOperand::makeReg(rhs), MOperand::makeReg(lhs)});
  }

  const MReg result = target_.scratchInt0();
  emit(isFloat ? setccForFloat(instruction.predicate())
               : setccForInt(instruction.predicate()),
       {MOperand::makeReg(result, true)}, "8-bit form");
  emit("movzbq", {MOperand::makeReg(result), MOperand::makeReg(result, true)});
  storeResult(instruction, result);
}

void CodeGen::lowerCall(const ir::Instruction& instruction) {
  const ir::Function& callee = *instruction.callee();

  // Arguments are staged through scratch registers, which are never argument
  // registers on this ABI, so setting argument i cannot disturb argument j.
  for (std::size_t i = 0; i < instruction.operandCount(); ++i) {
    const ir::Value* argument = instruction.operand(i);
    const bool isFloat = argument->type()->isF64();

    if (i < target_.maxRegisterArgs()) {
      const MReg destination =
          isFloat ? target_.floatArgRegister(static_cast<unsigned>(i))
                  : target_.integerArgRegister(static_cast<unsigned>(i));
      if (isFloat) {
        loadFloat(argument, destination);
      } else {
        loadInt(argument, destination);
      }
    } else {
      const MReg scratch = isFloat ? target_.scratchFloat0() : target_.scratchInt0();
      if (isFloat) {
        loadFloat(argument, scratch);
      } else {
        loadInt(argument, scratch);
      }
      const std::int32_t slot =
          target_.shadowSpaceBytes() +
          static_cast<std::int32_t>(i - target_.maxRegisterArgs()) * 8;
      emit(isFloat ? "movsd" : "movq",
           {MOperand::makeReg(scratch), MOperand::makeMem(MReg::RSP, slot)},
           "outgoing arg " + std::to_string(i));
    }
  }

  emit("call", {MOperand::makeLabel(target_.symbolName(callee.name()))});

  if (instruction.hasResult()) {
    storeResult(instruction, callee.returnType()->isF64() ? target_.floatReturnRegister()
                                                          : target_.integerReturnRegister());
  }
}

void CodeGen::lowerReturn(const ir::Instruction& instruction) {
  if (instruction.operandCount() == 1) {
    const ir::Value* value = instruction.operand(0);
    if (value->type()->isF64()) {
      loadFloat(value, target_.floatReturnRegister());
    } else {
      loadInt(value, target_.integerReturnRegister());
    }
  }
  emitEpilogue();
}

void CodeGen::lowerInstruction(const ir::Instruction& instruction) {
  switch (instruction.opcode()) {
    case Opcode::Add: lowerBinaryInt(instruction, "addq"); break;
    case Opcode::Sub: lowerBinaryInt(instruction, "subq"); break;
    case Opcode::Mul: lowerBinaryInt(instruction, "imulq"); break;
    case Opcode::SDiv: lowerDivRem(instruction, /*wantRemainder=*/false); break;
    case Opcode::SRem: lowerDivRem(instruction, /*wantRemainder=*/true); break;

    case Opcode::FAdd: lowerBinaryFloat(instruction, "addsd"); break;
    case Opcode::FSub: lowerBinaryFloat(instruction, "subsd"); break;
    case Opcode::FMul: lowerBinaryFloat(instruction, "mulsd"); break;
    case Opcode::FDiv: lowerBinaryFloat(instruction, "divsd"); break;

    case Opcode::Neg: {
      const MReg reg = target_.scratchInt0();
      loadInt(instruction.operand(0), reg);
      emit("negq", {MOperand::makeReg(reg, true)});
      storeResult(instruction, reg);
      break;
    }

    case Opcode::FNeg: {
      // Flip the sign bit rather than computing 0 - x, which would turn -0.0
      // into +0.0.
      const MReg reg = target_.scratchFloat0();
      loadFloat(instruction.operand(0), reg);
      module_->needsNegateMask = true;
      emit("xorpd", {MOperand::makeRipLabel(".LCnegmask"), MOperand::makeReg(reg, true)});
      storeResult(instruction, reg);
      break;
    }

    case Opcode::Not: {
      const MReg reg = target_.scratchInt0();
      loadInt(instruction.operand(0), reg);
      emit("xorq", {MOperand::makeImm(1), MOperand::makeReg(reg, true)},
           "logical not on i1");
      storeResult(instruction, reg);
      break;
    }

    case Opcode::ICmp: lowerCompare(instruction, /*isFloat=*/false); break;
    case Opcode::FCmp: lowerCompare(instruction, /*isFloat=*/true); break;

    case Opcode::SIToFP: {
      const MReg source = target_.scratchInt0();
      const MReg destination = target_.scratchFloat0();
      loadInt(instruction.operand(0), source);
      emit("cvtsi2sdq",
           {MOperand::makeReg(source), MOperand::makeReg(destination, true)});
      storeResult(instruction, destination);
      break;
    }

    case Opcode::Alloca:
      // The slot itself is the storage; nothing to emit.
      break;

    case Opcode::Copy: {
      // Produced by SSA destruction. Coalescing means source and destination
      // frequently share a slot, in which case the value is already where it
      // belongs and the move would be pure waste.
      std::int32_t source = 0;
      std::int32_t destination = 0;
      if (slotOf(instruction.operand(0), source) && slotOf(&instruction, destination) &&
          source == destination) {
        break;
      }
      const bool isFloat = instruction.type()->isF64();
      const MReg reg = isFloat ? target_.scratchFloat0() : target_.scratchInt0();
      if (isFloat) {
        loadFloat(instruction.operand(0), reg);
      } else {
        loadInt(instruction.operand(0), reg);
      }
      storeResult(instruction, reg);
      break;
    }

    case Opcode::Load: {
      const bool isFloat = instruction.type()->isF64();
      const MReg destination = isFloat ? target_.scratchFloat0() : target_.scratchInt0();
      std::int32_t offset = 0;
      if (directSlot(instruction.operand(0), offset)) {
        // Addressing the slot directly avoids computing its address first.
        emit(isFloat ? "movsd" : "movq",
             {MOperand::makeMem(MReg::RBP, offset),
              MOperand::makeReg(destination, true)});
      } else {
        const MReg address = target_.scratchInt1();
        loadInt(instruction.operand(0), address);
        emit(isFloat ? "movsd" : "movq",
             {MOperand::makeMem(address, 0), MOperand::makeReg(destination, true)});
      }
      storeResult(instruction, destination);
      break;
    }

    case Opcode::Store: {
      const ir::Value* value = instruction.operand(0);
      const bool isFloat = value->type()->isF64();
      const MReg source = isFloat ? target_.scratchFloat0() : target_.scratchInt0();
      if (isFloat) {
        loadFloat(value, source);
      } else {
        loadInt(value, source);
      }

      std::int32_t offset = 0;
      if (directSlot(instruction.operand(1), offset)) {
        emit(isFloat ? "movsd" : "movq",
             {MOperand::makeReg(source), MOperand::makeMem(MReg::RBP, offset)});
      } else {
        const MReg address = target_.scratchInt1();
        loadInt(instruction.operand(1), address);
        emit(isFloat ? "movsd" : "movq",
             {MOperand::makeReg(source), MOperand::makeMem(address, 0)});
      }
      break;
    }

    case Opcode::Br:
      emit("jmp", {MOperand::makeLabel(blockLabel(*instruction.successors()[0]))});
      break;

    case Opcode::CondBr: {
      const MReg condition = target_.scratchInt0();
      loadInt(instruction.operand(0), condition);
      emit("testq", {MOperand::makeReg(condition), MOperand::makeReg(condition)});
      emit("jne", {MOperand::makeLabel(blockLabel(*instruction.successors()[0]))});
      emit("jmp", {MOperand::makeLabel(blockLabel(*instruction.successors()[1]))});
      break;
    }

    case Opcode::Ret: lowerReturn(instruction); break;
    case Opcode::Call: lowerCall(instruction); break;

    default:
      emit("nop", {}, "unhandled opcode " + std::string(toString(instruction.opcode())));
      break;
  }
}

// ---------------------------------------------------------------------------
// Function and module
// ---------------------------------------------------------------------------

void CodeGen::lowerBlock(const ir::BasicBlock& block) {
  for (const auto& instruction : block.instructions()) {
    std::string provenance;
    if (instruction->hasResult() && instruction->hasName()) {
      provenance = "%" + instruction->name() + " = ";
    }
    provenance += std::string(toString(instruction->opcode()));

    const std::size_t before = block_->instructions.size();
    lowerInstruction(*instruction);
    // Tag the first machine instruction of each IR instruction so the assembly
    // can be read against --emit=ir (requirement BE-09).
    if (block_->instructions.size() > before) {
      block_->instructions[before].comment =
          provenance + (block_->instructions[before].comment.empty()
                            ? ""
                            : "  " + block_->instructions[before].comment);
    }
  }
}

void CodeGen::lowerFunction(const ir::Function& function, MFunction& out) {
  irFunction_ = &function;
  function_ = &out;
  out.name = target_.symbolName(function.name());

  assignSlots(function);

  out.blocks.reserve(function.blocks().size());
  for (const auto& block : function.blocks()) {
    out.blocks.push_back(MBasicBlock{blockLabel(*block), {}, block->executionCount});
  }

  for (std::size_t i = 0; i < function.blocks().size(); ++i) {
    block_ = &out.blocks[i];
    if (i == 0) {
      emitPrologue(function);
    }
    lowerBlock(*function.blocks()[i]);
  }

  block_ = nullptr;
  function_ = nullptr;
  irFunction_ = nullptr;
}

MModule CodeGen::run(const ir::Module& module) {
  MModule result;
  result.sourceName = module.sourceName();
  module_ = &result;
  floatLabels_.clear();

  for (const auto& function : module.functions()) {
    if (function->isDeclaration()) {
      continue;  // resolved by the linker against libofrt
    }
    result.functions.emplace_back();
    lowerFunction(*function, result.functions.back());
  }

  module_ = nullptr;
  return result;
}

}  // namespace optiforge::backend
