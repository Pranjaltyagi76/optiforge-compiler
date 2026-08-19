#include "optiforge/backend/TargetInfo.h"

namespace optiforge::backend {

namespace {

/// Microsoft x64 calling convention (ADR-10).
///
/// The two facts most likely to be got wrong, both verified against a running
/// binary before the code generator was written:
///
///   1. Only four arguments go in registers, and integer and float arguments
///      share those four positions. A float first argument takes xmm0 and an
///      integer second argument takes rdx -- not rcx.
///   2. The caller reserves 32 bytes of shadow space below its outgoing
///      arguments at every call, even when the callee takes none.
class X86_64Windows final : public TargetInfo {
public:
  std::string_view name() const override { return "x86_64-windows-msvc"; }

  MReg integerArgRegister(unsigned index) const override {
    switch (index) {
      case 0: return MReg::RCX;
      case 1: return MReg::RDX;
      case 2: return MReg::R8;
      case 3: return MReg::R9;
      default: return MReg::None;
    }
  }

  MReg floatArgRegister(unsigned index) const override {
    switch (index) {
      case 0: return MReg::XMM0;
      case 1: return MReg::XMM1;
      case 2: return MReg::XMM2;
      case 3: return MReg::XMM3;
      default: return MReg::None;
    }
  }

  unsigned maxRegisterArgs() const override { return 4; }

  std::int32_t shadowSpaceBytes() const override { return 32; }

  MReg integerReturnRegister() const override { return MReg::RAX; }
  MReg floatReturnRegister() const override { return MReg::XMM0; }

  // All four are caller-saved on this ABI, so the code generator may clobber
  // them without saving. r10/r11 in particular are never argument registers,
  // which keeps them usable while a call is being set up.
  MReg scratchInt0() const override { return MReg::RAX; }
  MReg scratchInt1() const override { return MReg::R10; }
  MReg scratchFloat0() const override { return MReg::XMM4; }
  MReg scratchFloat1() const override { return MReg::XMM5; }

  // Everything left after the ABI and the code generator have taken their cut.
  //
  // Reserved and therefore absent: rcx/rdx/r8/r9 carry arguments, rax returns
  // the result and doubles as scratch, r10 is the second scratch, rsp/rbp are
  // the frame. r11 is the one caller-saved register in the pool, so it is the
  // only one that cannot hold a value across a call; it is listed first
  // precisely so it is handed out before a callee-saved register whose use
  // costs a push and a pop.
  const std::vector<MReg>& allocatableIntRegisters() const override {
    static const std::vector<MReg> kRegisters{
        MReg::R11,  // caller-saved: free to use, but not across a call
        MReg::RBX, MReg::RSI, MReg::RDI,
        MReg::R12, MReg::R13, MReg::R14, MReg::R15,
    };
    return kRegisters;
  }

  // xmm0-xmm3 carry arguments and return values, xmm4-xmm5 are scratch. The
  // rest are callee-saved on this ABI, which is why the prologue has to spill
  // them to the frame rather than push them: there is no push for an SSE
  // register.
  const std::vector<MReg>& allocatableFloatRegisters() const override {
    static const std::vector<MReg> kRegisters{
        MReg::XMM6,  MReg::XMM7,  MReg::XMM8,  MReg::XMM9,  MReg::XMM10,
        MReg::XMM11, MReg::XMM12, MReg::XMM13, MReg::XMM14, MReg::XMM15,
    };
    return kRegisters;
  }

  bool isCalleeSaved(MReg reg) const override {
    switch (reg) {
      case MReg::RBX:
      case MReg::RBP:
      case MReg::RSI:
      case MReg::RDI:
      case MReg::RSP:
      case MReg::R12:
      case MReg::R13:
      case MReg::R14:
      case MReg::R15:
        return true;
      default:
        // xmm6-xmm15 are callee-saved; xmm0-xmm5 are not.
        return reg >= MReg::XMM6 && reg <= MReg::XMM15;
    }
  }

  std::int32_t stackAlignment() const override { return 16; }

  std::string symbolName(const std::string& name) const override {
    // 64-bit PE/COFF uses the bare name; the leading underscore is a 32-bit
    // Windows convention only.
    return name;
  }
};

}  // namespace

const TargetInfo& x86_64WindowsTarget() {
  static const X86_64Windows target;
  return target;
}

}  // namespace optiforge::backend
