#include "optiforge/backend/CodeGen.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "optiforge/analysis/AnalysisManager.h"
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
///
/// It also sets ZF, PF and CF all to 1 when either operand is NaN, which makes
/// the below/below-or-equal forms report true for an unordered pair. IEEE-754
/// says every ordered predicate is false there, so `<` and `<=` are emitted by
/// comparing the other way round and using the above forms, which read CF=0 and
/// are correctly false when unordered. See setccForFloat's callers.
const char* setccForFloat(Predicate predicate) {
  switch (predicate) {
    case Predicate::Eq: return "sete";
    case Predicate::Ne: return "setne";
    // Emitted against reversed operands, hence "above" for a less-than.
    case Predicate::Lt: return "seta";
    case Predicate::Gt: return "seta";
    case Predicate::Le: return "setae";
    case Predicate::Ge: return "setae";
  }
  return "sete";
}

/// True for the predicates whose operands must be compared in reverse so the
/// unordered case falls out false.
bool floatCompareReversesOperands(Predicate predicate) {
  return predicate == Predicate::Lt || predicate == Predicate::Le;
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

bool CodeGen::regOf(const ir::Value* value, MReg& reg) const {
  return value != nullptr && assignment_.registerFor(value, reg);
}

MReg CodeGen::destinationFor(const ir::Instruction& instruction, MReg fallback) const {
  MReg reg = MReg::None;
  // The class has to match: a float result computed into an integer register
  // would be nonsense. The allocator never produces that, but the check costs
  // nothing and keeps the invariant local to the place that relies on it.
  if (regOf(&instruction, reg) && isFloatReg(reg) == isFloatReg(fallback)) {
    return reg;
  }
  return fallback;
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
  savedGprs_.clear();
  savedXmms_.clear();

  // Callee-saved registers first. The general-purpose ones are pushed as soon
  // as rbp is established, so they occupy the bytes immediately below it and
  // every local has to start past them.
  for (MReg reg : assignment_.usedCalleeSaved) {
    if (!isFloatReg(reg)) {
      savedGprs_.push_back(reg);
    }
  }
  savedGprBytes_ = static_cast<std::int32_t>(savedGprs_.size()) * 8;

  std::int32_t offset = savedGprBytes_;

  // SSE registers have no push instruction, so the callee-saved ones get frame
  // slots of their own. Sixteen bytes each: the ABI wants the whole register
  // back, not just the double we kept in it.
  for (MReg reg : assignment_.usedCalleeSaved) {
    if (isFloatReg(reg)) {
      offset += 16;
      savedXmms_.push_back({reg, -offset});
    }
  }

  // Incoming arguments are copied into their home -- a register when the
  // allocator gave them one, a slot otherwise -- by the prologue, so the rest
  // of the generator treats them exactly like any other value.
  for (const auto& argument : function.arguments()) {
    if (assignment_.assigned.count(argument.get()) != 0) {
      continue;
    }
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
      // A value the allocator coloured needs no slot at all. That, and nothing
      // else, is what makes a spill here free of any rewrite: a spilled value
      // simply keeps the slot it would have had anyway.
      if (assignment_.assigned.count(instruction.get()) != 0) {
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
      if (root == nullptr || root == instruction.get() ||
          assignment_.assigned.count(instruction.get()) != 0) {
        continue;
      }
      const auto it = slots_.find(root);
      if (it != slots_.end()) {
        slots_[instruction.get()] = it->second;
      }
    }
  }

  localBytes_ = offset - savedGprBytes_;

  // Space for outgoing arguments: shadow space always, plus one slot for each
  // argument past the fourth.
  outgoingBytes_ = target_.shadowSpaceBytes();
  if (maxCallArgs > target_.maxRegisterArgs()) {
    outgoingBytes_ +=
        static_cast<std::int32_t>(maxCallArgs - target_.maxRegisterArgs()) * 8;
  }

  function_->frameSize =
      alignUp(localBytes_ + outgoingBytes_, target_.stackAlignment());

  // Alignment, restated because the callee-saved pushes moved it. rsp has to be
  // 16-byte aligned at every call, and what gets it there is `push rbp` plus
  // those pushes plus the frame. An odd number of pushes leaves it eight bytes
  // out, and the frame is the only part still free to correct it.
  if (savedGprs_.size() % 2 == 1) {
    function_->frameSize += 8;
  }
}

void CodeGen::emitPrologue(const ir::Function& function) {
  emit("pushq", {MOperand::makeReg(MReg::RBP)});
  emit("movq", {MOperand::makeReg(MReg::RSP), MOperand::makeReg(MReg::RBP, true)});

  for (MReg reg : savedGprs_) {
    emit("pushq", {MOperand::makeReg(reg)}, "callee-saved");
  }

  if (function_->frameSize > 0) {
    emit("subq",
         {MOperand::makeImm(function_->frameSize), MOperand::makeReg(MReg::RSP, true)},
         "frame: " + std::to_string(localBytes_) + " local + " +
             std::to_string(outgoingBytes_) + " outgoing");
  }

  // After the frame is reserved, never before: Windows x64 has no red zone, so
  // anything written below rsp can be overwritten before it is read back.
  for (const auto& saved : savedXmms_) {
    emit("movups",
         {MOperand::makeReg(saved.first), MOperand::makeMem(MReg::RBP, saved.second)},
         "callee-saved");
  }

  // Copy incoming arguments into their home.
  for (std::size_t i = 0; i < function.arguments().size(); ++i) {
    const ir::Argument& argument = *function.arguments()[i];
    const bool isFloat = argument.type()->isF64();
    const char* move = isFloat ? "movsd" : "movq";

    MReg home = MReg::None;
    const bool inRegister = regOf(&argument, home);
    std::int32_t slot = 0;
    if (!inRegister && !slotOf(&argument, slot)) {
      continue;  // an argument nothing reads
    }

    if (i < target_.maxRegisterArgs()) {
      const MReg source =
          isFloat ? target_.floatArgRegister(static_cast<unsigned>(i))
                  : target_.integerArgRegister(static_cast<unsigned>(i));
      if (!inRegister) {
        emit(move, {MOperand::makeReg(source), MOperand::makeMem(MReg::RBP, slot)},
             "arg " + argument.name());
      } else if (home != source) {
        // Argument registers are never allocatable, so moving one into an
        // allocated register cannot clobber an argument not yet copied.
        emit(move, {MOperand::makeReg(source), MOperand::makeReg(home, true)},
             "arg " + argument.name());
      }
      continue;
    }

    // Stack arguments sit above the return address and saved rbp, past the
    // caller's 32-byte shadow space: rbp + 16 + 32 + 8*(i-4).
    const std::int32_t incoming =
        16 + target_.shadowSpaceBytes() +
        static_cast<std::int32_t>(i - target_.maxRegisterArgs()) * 8;
    if (inRegister) {
      emit(move, {MOperand::makeMem(MReg::RBP, incoming), MOperand::makeReg(home, true)},
           "arg " + argument.name());
    } else {
      const MReg scratch = isFloat ? target_.scratchFloat0() : target_.scratchInt0();
      emit(move,
           {MOperand::makeMem(MReg::RBP, incoming), MOperand::makeReg(scratch, true)});
      emit(move, {MOperand::makeReg(scratch), MOperand::makeMem(MReg::RBP, slot)},
           "arg " + argument.name());
    }
  }
}

void CodeGen::emitEpilogue() {
  for (const auto& saved : savedXmms_) {
    emit("movups",
         {MOperand::makeMem(MReg::RBP, saved.second), MOperand::makeReg(saved.first, true)},
         "restore callee-saved");
  }

  if (savedGprs_.empty()) {
    emit("movq", {MOperand::makeReg(MReg::RBP), MOperand::makeReg(MReg::RSP, true)});
  } else {
    // Point rsp at the last register pushed rather than trusting it to still be
    // where the prologue left it, then unwind in reverse.
    emit("leaq", {MOperand::makeMem(MReg::RBP, -savedGprBytes_),
                  MOperand::makeReg(MReg::RSP, true)});
    for (auto it = savedGprs_.rbegin(); it != savedGprs_.rend(); ++it) {
      emit("popq", {MOperand::makeReg(*it, true)}, "restore callee-saved");
    }
  }
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

  MReg home = MReg::None;
  if (regOf(value, home)) {
    if (home != destination) {
      emit("movq", {MOperand::makeReg(home), MOperand::makeReg(destination, true)});
    }
    return;
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

  MReg home = MReg::None;
  if (regOf(value, home)) {
    if (home != destination) {
      emit("movsd", {MOperand::makeReg(home), MOperand::makeReg(destination, true)});
    }
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
  MReg home = MReg::None;
  if (regOf(&instruction, home)) {
    // Usually a no-op: the lowering asked for the result to be computed in the
    // register it lives in, which is the entire point of allocating one.
    if (home != source) {
      emit(isFloatReg(source) ? "movsd" : "movq",
           {MOperand::makeReg(source), MOperand::makeReg(home, true)});
    }
    return;
  }

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
  // Compute straight into the result's own register when it has one, which
  // turns the store that followed in Phase 4 into nothing at all.
  const MReg lhs = destinationFor(instruction, target_.scratchInt0());
  // Read the right operand where it already lives, unless that is the register
  // the result is being computed in -- loading the left operand there would
  // destroy it.
  MReg rhs = MReg::None;
  if (!regOf(instruction.operand(1), rhs) || rhs == lhs) {
    rhs = target_.scratchInt1();
    loadInt(instruction.operand(1), rhs);
  }
  loadInt(instruction.operand(0), lhs);
  emit(mnemonic, {MOperand::makeReg(rhs), MOperand::makeReg(lhs, true)});
  storeResult(instruction, lhs);
}

void CodeGen::lowerBinaryFloat(const ir::Instruction& instruction, const char* mnemonic) {
  const MReg lhs = destinationFor(instruction, target_.scratchFloat0());
  MReg rhs = MReg::None;  // see lowerBinaryInt for why the order matters
  if (!regOf(instruction.operand(1), rhs) || rhs == lhs) {
    rhs = target_.scratchFloat1();
    loadFloat(instruction.operand(1), rhs);
  }
  loadFloat(instruction.operand(0), lhs);
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
  const Predicate predicate = instruction.predicate();
  // setcc writes a byte, so the result register must have an 8-bit form. Every
  // allocatable integer register does; the scratch fallback does too.
  const MReg result = destinationFor(instruction, target_.scratchInt0());

  if (!isFloat) {
    // Both operands are only read, so wherever they already are will do; the
    // comparison finishes before setcc writes anything.
    MReg lhs = MReg::None;
    if (!regOf(instruction.operand(0), lhs)) {
      lhs = target_.scratchInt0();
      loadInt(instruction.operand(0), lhs);
    }
    MReg rhs = MReg::None;
    if (!regOf(instruction.operand(1), rhs)) {
      rhs = target_.scratchInt1();
      loadInt(instruction.operand(1), rhs);
    }
    emit("cmpq", {MOperand::makeReg(rhs), MOperand::makeReg(lhs)});
    emit(setccForInt(predicate), {MOperand::makeReg(result, true)}, "8-bit form");
    emit("movzbq", {MOperand::makeReg(result), MOperand::makeReg(result, true)});
    storeResult(instruction, result);
    return;
  }

  MReg first = MReg::None;
  if (!regOf(instruction.operand(0), first)) {
    first = target_.scratchFloat0();
    loadFloat(instruction.operand(0), first);
  }
  MReg second = MReg::None;
  if (!regOf(instruction.operand(1), second)) {
    second = target_.scratchFloat1();
    loadFloat(instruction.operand(1), second);
  }

  // `comisd b, a` sets the flags for `a ? b`. For < and <= the operands are
  // swapped so the above-family condition can be used, which is false when
  // either operand is NaN -- as IEEE-754 requires and as the below family is
  // not.
  if (floatCompareReversesOperands(predicate)) {
    emit("comisd", {MOperand::makeReg(first), MOperand::makeReg(second)});
  } else {
    emit("comisd", {MOperand::makeReg(second), MOperand::makeReg(first)});
  }

  emit(setccForFloat(predicate), {MOperand::makeReg(result, true)}, "8-bit form");

  // Equality is the one pair the flags alone cannot express: comisd sets ZF for
  // "equal" *and* for "unordered", so PF has to be consulted as well.
  if (predicate == Predicate::Eq || predicate == Predicate::Ne) {
    const MReg parity = target_.scratchInt1();
    emit(predicate == Predicate::Eq ? "setnp" : "setp",
         {MOperand::makeReg(parity, true)}, "NaN is unordered, not equal");
    emit(predicate == Predicate::Eq ? "andb" : "orb",
         {MOperand::makeReg(parity), MOperand::makeReg(result, true)});
  }

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

    case Opcode::Shl:
    case Opcode::AShr: {
      // x86-64 takes a variable shift count only in cl, so the amount cannot
      // be placed freely the way an ordinary binary operand can.
      const MReg value = destinationFor(instruction, target_.scratchInt0());
      // The count first: `value` may be the register the count lives in.
      loadInt(instruction.operand(1), MReg::RCX);
      loadInt(instruction.operand(0), value);
      emit(instruction.opcode() == Opcode::Shl ? "shlq" : "sarq",
           {MOperand::makeReg(MReg::RCX), MOperand::makeReg(value, true)},
           "shift count must be in cl");
      storeResult(instruction, value);
      break;
    }
    case Opcode::SRem: lowerDivRem(instruction, /*wantRemainder=*/true); break;

    case Opcode::FAdd: lowerBinaryFloat(instruction, "addsd"); break;
    case Opcode::FSub: lowerBinaryFloat(instruction, "subsd"); break;
    case Opcode::FMul: lowerBinaryFloat(instruction, "mulsd"); break;
    case Opcode::FDiv: lowerBinaryFloat(instruction, "divsd"); break;

    case Opcode::Neg: {
      const MReg reg = destinationFor(instruction, target_.scratchInt0());
      loadInt(instruction.operand(0), reg);
      emit("negq", {MOperand::makeReg(reg, true)});
      storeResult(instruction, reg);
      break;
    }

    case Opcode::FNeg: {
      // Flip the sign bit rather than computing 0 - x, which would turn -0.0
      // into +0.0.
      const MReg reg = destinationFor(instruction, target_.scratchFloat0());
      loadFloat(instruction.operand(0), reg);
      module_->needsNegateMask = true;
      emit("xorpd", {MOperand::makeRipLabel(".LCnegmask"), MOperand::makeReg(reg, true)});
      storeResult(instruction, reg);
      break;
    }

    case Opcode::Not: {
      const MReg reg = destinationFor(instruction, target_.scratchInt0());
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
      const MReg destination = destinationFor(instruction, target_.scratchFloat0());
      loadInt(instruction.operand(0), source);
      emit("cvtsi2sdq",
           {MOperand::makeReg(source), MOperand::makeReg(destination, true)});
      storeResult(instruction, destination);
      break;
    }

    case Opcode::Alloca:
      // The slot itself is the storage; nothing to emit.
      break;

    case Opcode::ProfInc:
      // One instruction, no call, no register touched -- which is the whole
      // reason the counters are a static array rather than a runtime call.
      emit("incq",
           {MOperand::makeRipLabel("__ofprof_counters+" +
                                   std::to_string(instruction.counterIndex() * 8))},
           "profile counter " + std::to_string(instruction.counterIndex()));
      break;

    case Opcode::Copy: {
      // Produced by SSA destruction. Source and destination frequently share a
      // location -- a slot by coalescing, a register by the allocator having
      // merged the two ends of this very copy -- and then the move is waste.
      MReg sourceReg = MReg::None;
      MReg destinationReg = MReg::None;
      if (regOf(instruction.operand(0), sourceReg) &&
          regOf(&instruction, destinationReg) && sourceReg == destinationReg) {
        break;
      }
      std::int32_t source = 0;
      std::int32_t destination = 0;
      if (slotOf(instruction.operand(0), source) && slotOf(&instruction, destination) &&
          source == destination) {
        break;
      }
      const bool isFloat = instruction.type()->isF64();
      const MReg reg = destinationFor(
          instruction, isFloat ? target_.scratchFloat0() : target_.scratchInt0());
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
      const MReg destination = destinationFor(
          instruction, isFloat ? target_.scratchFloat0() : target_.scratchInt0());
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
      // A value already in a register is stored straight from it; only a
      // constant or a spilled value has to pass through scratch.
      MReg source = MReg::None;
      if (!regOf(value, source)) {
        source = isFloat ? target_.scratchFloat0() : target_.scratchInt0();
        if (isFloat) {
          loadFloat(value, source);
        } else {
          loadInt(value, source);
        }
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
      // Test the condition where it already is, when it is anywhere at all.
      MReg condition = MReg::None;
      if (!regOf(instruction.operand(0), condition)) {
        condition = target_.scratchInt0();
        loadInt(instruction.operand(0), condition);
      }
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

MModule CodeGen::run(const ir::Module& module, analysis::AnalysisManager& analyses) {
  MModule result;
  result.sourceName = module.sourceName();
  result.profile = profile_;
  module_ = &result;
  floatLabels_.clear();
  allocationErrors_.clear();
  allocations_.clear();

  for (const auto& function : module.functions()) {
    if (function->isDeclaration()) {
      continue;  // resolved by the linker against libofrt
    }

    assignment_ = RegisterAssignment{};
    if (allocator_ == RegAllocKind::Graph) {
      assignment_ = allocateRegisters(*function, analyses, target_);
      // Checked here rather than trusted: an allocator that puts two live
      // values in one register produces code that is wrong in a way no test
      // reliably catches, so it is verified on every compilation.
      const std::vector<std::string> errors =
          verifyAssignment(*function, analyses, target_, assignment_);
      allocationErrors_.insert(allocationErrors_.end(), errors.begin(), errors.end());
      if (!errors.empty()) {
        assignment_ = RegisterAssignment{};  // fall back rather than emit it
      }
    }
    allocations_.push_back(assignment_);

    result.functions.emplace_back();
    lowerFunction(*function, result.functions.back());
  }

  module_ = nullptr;
  return result;
}

}  // namespace optiforge::backend
