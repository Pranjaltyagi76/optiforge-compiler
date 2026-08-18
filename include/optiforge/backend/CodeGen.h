#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <vector>

#include "optiforge/backend/MachineIR.h"
#include "optiforge/backend/TargetInfo.h"

namespace optiforge::ir {
class Module;
class Function;
class BasicBlock;
class Instruction;
class Value;
}  // namespace optiforge::ir

namespace optiforge::backend {

/// Naive code generator: correct, and deliberately not fast.
///
/// Every value lives in its own stack slot. Operands are loaded into scratch
/// registers, the operation is performed, and the result is stored straight
/// back to memory. Nothing is kept in a register across instructions.
///
/// That is the whole point of Phase 4 (ADR-08): it establishes a working
/// executable that the graph-colouring allocator in Phase 8 can be measured
/// against, and it stays in the tree afterwards as `--regalloc=naive` so a
/// miscompile can be bisected to the allocator in one flag.
class CodeGen {
public:
  explicit CodeGen(const TargetInfo& target) : target_(target) {}

  MModule run(const ir::Module& module);

private:
  void lowerFunction(const ir::Function& function, MFunction& out);

  /// Assigns a frame slot to every argument, alloca and instruction result,
  /// and computes the frame size.
  void assignSlots(const ir::Function& function);

  void emitPrologue(const ir::Function& function);
  void emitEpilogue();

  void lowerBlock(const ir::BasicBlock& block);
  void lowerInstruction(const ir::Instruction& instruction);
  void lowerBinaryInt(const ir::Instruction& instruction, const char* mnemonic);
  void lowerBinaryFloat(const ir::Instruction& instruction, const char* mnemonic);
  void lowerDivRem(const ir::Instruction& instruction, bool wantRemainder);
  void lowerCompare(const ir::Instruction& instruction, bool isFloat);
  void lowerCall(const ir::Instruction& instruction);
  void lowerReturn(const ir::Instruction& instruction);

  // --- Value movement ---
  /// Materializes `value` in an integer register.
  void loadInt(const ir::Value* value, MReg destination);
  /// Materializes `value` in an SSE register.
  void loadFloat(const ir::Value* value, MReg destination);
  void storeResult(const ir::Instruction& instruction, MReg source);

  /// Frame offset of the slot `value` occupies, if it has one.
  bool slotOf(const ir::Value* value, std::int32_t& offset) const;
  /// Frame offset an alloca's storage occupies, when `value` is that alloca.
  /// Lets `load`/`store` address the slot directly instead of computing its
  /// address into a register first.
  bool directSlot(const ir::Value* value, std::int32_t& offset) const;

  std::string blockLabel(const ir::BasicBlock& block) const;
  std::string floatConstantLabel(double value);

  void emit(const char* mnemonic, std::vector<MOperand> operands,
            std::string comment = {});
  void emitLabel(std::string label);

  const TargetInfo& target_;
  MModule* module_ = nullptr;
  MFunction* function_ = nullptr;
  MBasicBlock* block_ = nullptr;
  const ir::Function* irFunction_ = nullptr;

  std::unordered_map<const ir::Value*, std::int32_t> slots_;
  std::unordered_map<std::uint64_t, std::string> floatLabels_;
  std::int32_t localBytes_ = 0;
  std::int32_t outgoingBytes_ = 0;
};

/// Renders a machine module as GNU assembler input (AT&T syntax).
void printAssembly(const MModule& module, std::ostream& out);

}  // namespace optiforge::backend
