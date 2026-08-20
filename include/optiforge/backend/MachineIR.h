#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "optiforge/support/ProfileLayout.h"

namespace optiforge::backend {

/// x86-64 registers. Only the ones this backend actually touches are named.
enum class MReg : std::uint8_t {
  None = 0,
  RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
  R8, R9, R10, R11, R12, R13, R14, R15,
  XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7,
  // xmm8-xmm15 exist so the allocator has something to allocate: on this ABI
  // xmm0-xmm3 carry arguments and xmm4-xmm5 are the code generator's scratch,
  // which leaves xmm6-xmm15 -- all callee-saved -- as the float pool.
  XMM8, XMM9, XMM10, XMM11, XMM12, XMM13, XMM14, XMM15,
};

/// AT&T spelling, including the `%` sigil.
std::string_view regName(MReg reg);
/// Low 8 bits of an integer register, for setcc.
std::string_view regName8(MReg reg);
bool isFloatReg(MReg reg);

/// One operand of a machine instruction.
struct MOperand {
  enum class Kind : std::uint8_t {
    Reg,       // %rax
    Imm,       // $42
    Mem,       // disp(%base)
    Label,     // a branch target or symbol
    RipLabel,  // label(%rip), for constants in .rdata
  };

  Kind kind = Kind::Reg;
  MReg reg = MReg::None;
  std::int64_t imm = 0;
  MReg base = MReg::None;
  std::int32_t disp = 0;
  std::string label;

  /// True when this operand is written rather than read. Phase 8's register
  /// allocator needs def/use information; recording it here means the machine
  /// IR does not have to be re-derived from mnemonics later.
  bool isDef = false;

  static MOperand makeReg(MReg reg, bool isDef = false);
  static MOperand makeImm(std::int64_t value);
  static MOperand makeMem(MReg base, std::int32_t disp);
  static MOperand makeLabel(std::string label);
  static MOperand makeRipLabel(std::string label);
};

/// One machine instruction.
///
/// The mnemonic is a string rather than the opcode enum sketched in
/// System_design.md section 11.1. For a backend whose only job is to be
/// correct, a table of opcodes would add indirection without adding checking;
/// what register allocation actually needs is def/use information, and that
/// lives on the operands. Phase 8 can introduce an opcode enum if it turns out
/// to need one.
struct MInstr {
  const char* mnemonic = "";
  std::vector<MOperand> operands;
  /// Rendered as an assembly comment; carries IR provenance so the emitted
  /// code can be read against `--emit=ir` (requirement BE-09).
  std::string comment;

  bool isLabel = false;   // a label pseudo-instruction rather than a real one
  bool isDirective = false;
};

struct MBasicBlock {
  std::string label;
  std::vector<MInstr> instructions;
  /// Execution count from a profile, used for block layout in Phase 11.
  std::uint64_t executionCount = 0;
};

struct MFunction {
  std::string name;
  std::vector<MBasicBlock> blocks;
  /// Bytes subtracted from rsp in the prologue. Always a multiple of 16.
  std::int32_t frameSize = 0;
};

/// A double that must be materialized from .rdata, since x86-64 has no
/// instruction to load an arbitrary 64-bit float as an immediate.
struct FloatConstant {
  std::string label;
  double value;
};

struct MModule {
  std::string sourceName;
  std::vector<MFunction> functions;
  std::vector<FloatConstant> floatConstants;
  /// True when any function negates a float, which needs a 16-byte sign mask.
  bool needsNegateMask = false;

  /// Counter array, record table and header fields for an instrumented build.
  /// `enabled` is false for every ordinary build, and then nothing extra is
  /// emitted and the profile runtime is not linked.
  ProfileLayout profile;
};

}  // namespace optiforge::backend
