#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "optiforge/backend/MachineIR.h"
#include "optiforge/backend/RegAlloc.h"
#include "optiforge/backend/TargetInfo.h"

namespace optiforge::analysis {
class AnalysisManager;
}

namespace optiforge::ir {
class Module;
class Function;
class BasicBlock;
class Instruction;
class Value;
}  // namespace optiforge::ir

namespace optiforge::backend {

/// What block layout did to one function, for --pgo-remarks and the metrics.
struct LayoutResult {
  std::size_t moved = 0;         ///< blocks that ended up somewhere new
  std::size_t jumpsRemoved = 0;  ///< jumps that became fall-through
};

/// Orders a function's blocks and removes the jumps that ordering made
/// unnecessary (PGO-09).
///
/// With a profile the hot path is laid out to fall through and zero-count blocks
/// are sunk to the end. Without one the order is left alone -- but the
/// fall-through cleanup still runs, because it is a straight win on every build
/// and doing it only for profile-guided ones would flatter the comparison.
LayoutResult layoutBlocks(MFunction& function, bool useProfile);

/// Turns IR into machine instructions.
///
/// One code generator, two allocation strategies (ADR-08):
///
///   - `RegAllocKind::Naive` is Phase 4. Every value lives in its own frame
///     slot; operands are loaded into scratch registers, the operation runs,
///     the result goes straight back to memory. Correct, slow, and completely
///     predictable -- which is exactly what is wanted when the bug is somewhere
///     else.
///   - `RegAllocKind::Graph` is Phase 8. A value that the allocator coloured
///     lives in its register for its whole live range and is never written to
///     memory at all; a value it spilled behaves exactly as it did in Phase 4.
///
/// The two share every lowering rule. `loadInt`, `loadFloat` and `storeResult`
/// are the only places that know where a value lives, so adding the allocator
/// did not fork the instruction selector.
class CodeGen {
public:
  explicit CodeGen(const TargetInfo& target, RegAllocKind allocator = RegAllocKind::Graph)
      : target_(target), allocator_(allocator) {}

  /// `analyses` supplies the liveness and loop information the graph allocator
  /// runs on. It is passed even for the naive allocator so the caller does not
  /// have to know which one is in use; nothing is computed if it is not needed.
  MModule run(const ir::Module& module, analysis::AnalysisManager& analyses);

  /// Data the instrumentation pass wants emitted alongside the code. Set before
  /// `run`; ignored entirely when `enabled` is false.
  void setProfileLayout(ProfileLayout layout) { profile_ = std::move(layout); }

  /// Allocation errors found by `verifyAssignment`, accumulated across the
  /// module. Non-empty means the compiler produced code it cannot vouch for,
  /// which the driver reports as an internal error rather than shipping.
  const std::vector<std::string>& allocationErrors() const { return allocationErrors_; }

  /// Per-function allocation statistics, in module order. Feeds the metrics
  /// table and the tests that would otherwise have to grep assembly.
  const std::vector<RegisterAssignment>& allocations() const { return allocations_; }

  /// Lay blocks out by measured frequency rather than leaving them in IR order.
  /// Off unless a profile was supplied; the fall-through cleanup runs either way.
  void setProfileGuidedLayout(bool value) { profileLayout_ = value; }

  /// Blocks moved and jumps removed across the whole module.
  const LayoutResult& layout() const { return layout_; }

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
  /// True when this comparison feeds nothing but the branch right after it, so
  /// the flags it sets can be branched on directly.
  bool fusesWithNextBranch(const ir::Instruction& instruction) const;
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
  /// Register `value` was allocated, if it has one. A value has either a
  /// register or a slot, never both.
  bool regOf(const ir::Value* value, MReg& reg) const;
  /// The register an instruction's result should be computed into, or `fallback`
  /// when it was spilled.
  MReg destinationFor(const ir::Instruction& instruction, MReg fallback) const;
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
  RegAllocKind allocator_ = RegAllocKind::Graph;
  MModule* module_ = nullptr;
  MFunction* function_ = nullptr;
  MBasicBlock* block_ = nullptr;
  const ir::Function* irFunction_ = nullptr;

  std::unordered_map<const ir::Value*, std::int32_t> slots_;
  std::unordered_map<std::uint64_t, std::string> floatLabels_;
  std::int32_t localBytes_ = 0;
  std::int32_t outgoingBytes_ = 0;

  // --- Register allocation, per function ---
  RegisterAssignment assignment_;
  /// Callee-saved GPRs pushed by the prologue, in push order. Their slots sit
  /// immediately below rbp, so every local offset starts past them.
  std::vector<MReg> savedGprs_;
  /// Callee-saved SSE registers, which have no push instruction and so live in
  /// frame slots of their own.
  std::vector<std::pair<MReg, std::int32_t>> savedXmms_;
  std::int32_t savedGprBytes_ = 0;

  std::vector<std::string> allocationErrors_;
  std::vector<RegisterAssignment> allocations_;
  ProfileLayout profile_;
  /// An integer comparison whose setcc was skipped because the branch that
  /// follows will read its flags instead. Null except across those two
  /// instructions.
  const ir::Instruction* fusedCompare_ = nullptr;
  bool profileLayout_ = false;
  LayoutResult layout_;
};

/// Renders a machine module as GNU assembler input (AT&T syntax).
void printAssembly(const MModule& module, std::ostream& out);

}  // namespace optiforge::backend
