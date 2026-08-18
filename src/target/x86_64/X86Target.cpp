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
