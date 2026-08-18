#include "optiforge/backend/MachineIR.h"

#include <utility>

namespace optiforge::backend {

std::string_view regName(MReg reg) {
  switch (reg) {
    case MReg::None: return "%<none>";
    case MReg::RAX: return "%rax";
    case MReg::RCX: return "%rcx";
    case MReg::RDX: return "%rdx";
    case MReg::RBX: return "%rbx";
    case MReg::RSP: return "%rsp";
    case MReg::RBP: return "%rbp";
    case MReg::RSI: return "%rsi";
    case MReg::RDI: return "%rdi";
    case MReg::R8:  return "%r8";
    case MReg::R9:  return "%r9";
    case MReg::R10: return "%r10";
    case MReg::R11: return "%r11";
    case MReg::R12: return "%r12";
    case MReg::R13: return "%r13";
    case MReg::R14: return "%r14";
    case MReg::R15: return "%r15";
    case MReg::XMM0: return "%xmm0";
    case MReg::XMM1: return "%xmm1";
    case MReg::XMM2: return "%xmm2";
    case MReg::XMM3: return "%xmm3";
    case MReg::XMM4: return "%xmm4";
    case MReg::XMM5: return "%xmm5";
    case MReg::XMM6: return "%xmm6";
    case MReg::XMM7: return "%xmm7";
  }
  return "%<?>";
}

std::string_view regName8(MReg reg) {
  switch (reg) {
    case MReg::RAX: return "%al";
    case MReg::RCX: return "%cl";
    case MReg::RDX: return "%dl";
    case MReg::RBX: return "%bl";
    case MReg::RSI: return "%sil";
    case MReg::RDI: return "%dil";
    case MReg::R8:  return "%r8b";
    case MReg::R9:  return "%r9b";
    case MReg::R10: return "%r10b";
    case MReg::R11: return "%r11b";
    case MReg::R12: return "%r12b";
    case MReg::R13: return "%r13b";
    case MReg::R14: return "%r14b";
    case MReg::R15: return "%r15b";
    default: return regName(reg);
  }
}

bool isFloatReg(MReg reg) {
  return reg >= MReg::XMM0 && reg <= MReg::XMM7;
}

MOperand MOperand::makeReg(MReg reg, bool isDef) {
  MOperand operand;
  operand.kind = Kind::Reg;
  operand.reg = reg;
  operand.isDef = isDef;
  return operand;
}

MOperand MOperand::makeImm(std::int64_t value) {
  MOperand operand;
  operand.kind = Kind::Imm;
  operand.imm = value;
  return operand;
}

MOperand MOperand::makeMem(MReg base, std::int32_t disp) {
  MOperand operand;
  operand.kind = Kind::Mem;
  operand.base = base;
  operand.disp = disp;
  return operand;
}

MOperand MOperand::makeLabel(std::string label) {
  MOperand operand;
  operand.kind = Kind::Label;
  operand.label = std::move(label);
  return operand;
}

MOperand MOperand::makeRipLabel(std::string label) {
  MOperand operand;
  operand.kind = Kind::RipLabel;
  operand.label = std::move(label);
  return operand;
}

}  // namespace optiforge::backend
