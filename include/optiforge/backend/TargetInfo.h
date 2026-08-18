#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "optiforge/backend/MachineIR.h"

namespace optiforge::backend {

/// Every ABI fact the code generator needs, in one place.
///
/// ADR-10 fixes the target as x86-64 Windows (Microsoft x64). Keeping these
/// facts behind an interface is what allows a System V target to be added later
/// as a data change rather than a rewrite -- the code generator itself never
/// names a register directly.
class TargetInfo {
public:
  virtual ~TargetInfo() = default;

  virtual std::string_view name() const = 0;

  /// Integer register for argument `index`, or None when it must go on the
  /// stack. Argument positions are shared between integer and float registers,
  /// so index 1 is the *second* argument regardless of the first one's type.
  virtual MReg integerArgRegister(unsigned index) const = 0;
  virtual MReg floatArgRegister(unsigned index) const = 0;

  /// How many arguments are passed in registers before the stack is used.
  virtual unsigned maxRegisterArgs() const = 0;

  /// Bytes the caller must reserve below its outgoing stack arguments. On
  /// Microsoft x64 this is 32 bytes of shadow space, required even when the
  /// callee takes no arguments at all.
  virtual std::int32_t shadowSpaceBytes() const = 0;

  virtual MReg integerReturnRegister() const = 0;
  virtual MReg floatReturnRegister() const = 0;

  /// Scratch registers the naive code generator may clobber freely. All must
  /// be caller-saved so no save/restore is needed around them.
  virtual MReg scratchInt0() const = 0;
  virtual MReg scratchInt1() const = 0;
  virtual MReg scratchFloat0() const = 0;
  virtual MReg scratchFloat1() const = 0;

  virtual std::int32_t stackAlignment() const = 0;

  /// Assembly symbol for a source-level name. 64-bit PE/COFF applies no
  /// underscore prefix, unlike 32-bit Windows.
  virtual std::string symbolName(const std::string& name) const = 0;
};

/// Microsoft x64 (ADR-10).
const TargetInfo& x86_64WindowsTarget();

}  // namespace optiforge::backend
