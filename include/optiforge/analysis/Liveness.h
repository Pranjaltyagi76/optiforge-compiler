#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "optiforge/analysis/AnalysisManager.h"
#include "optiforge/analysis/BitSet.h"

namespace optiforge::ir {
class BasicBlock;
class Function;
class Instruction;
class Value;
}  // namespace optiforge::ir

namespace optiforge::analysis {

/// Which values are live at each block boundary.
///
/// A value is live at a point if some path from there uses it before it is
/// redefined. This is what the graph-colouring allocator in Phase 8 builds
/// live ranges from, so an error here becomes a miscompile there.
class Liveness {
public:
  Liveness() = default;
  Liveness(const ir::Function& function);

  const std::vector<const ir::Value*>& liveIn(const ir::BasicBlock* block) const;
  const std::vector<const ir::Value*>& liveOut(const ir::BasicBlock* block) const;

  bool isLiveIn(const ir::BasicBlock* block, const ir::Value* value) const;
  bool isLiveOut(const ir::BasicBlock* block, const ir::Value* value) const;

  /// Every value the analysis tracks: instruction results and arguments, in a
  /// fixed order so dumps are deterministic.
  const std::vector<const ir::Value*>& tracked() const { return values_; }

  unsigned iterations() const { return iterations_; }

private:
  std::vector<const ir::Value*> values_;
  std::unordered_map<const ir::Value*, std::size_t> valueIndex_;
  std::unordered_map<const ir::BasicBlock*, std::vector<const ir::Value*>> in_;
  std::unordered_map<const ir::BasicBlock*, std::vector<const ir::Value*>> out_;
  std::vector<const ir::Value*> empty_;
  unsigned iterations_ = 0;
};

struct LivenessAnalysis {
  using Result = Liveness;
  static const char* name() { return "liveness"; }
  static Result run(const ir::Function& function, AnalysisManager&) {
    return Liveness(function);
  }
};

/// Which stores may be the most recent write to each stack slot.
///
/// Formulated over stores to allocas rather than over SSA values: before
/// mem2reg (Phase 6) every variable lives in memory, so that is where the
/// interesting definitions are.
class ReachingDefinitions {
public:
  ReachingDefinitions() = default;
  ReachingDefinitions(const ir::Function& function);

  /// Stores reaching the start of `block`, in program order.
  const std::vector<const ir::Instruction*>& reachingIn(
      const ir::BasicBlock* block) const;
  const std::vector<const ir::Instruction*>& reachingOut(
      const ir::BasicBlock* block) const;

  const std::vector<const ir::Instruction*>& definitions() const { return defs_; }
  unsigned iterations() const { return iterations_; }

private:
  std::vector<const ir::Instruction*> defs_;
  std::unordered_map<const ir::BasicBlock*, std::vector<const ir::Instruction*>> in_;
  std::unordered_map<const ir::BasicBlock*, std::vector<const ir::Instruction*>> out_;
  std::vector<const ir::Instruction*> empty_;
  unsigned iterations_ = 0;
};

struct ReachingDefinitionsAnalysis {
  using Result = ReachingDefinitions;
  static const char* name() { return "reaching-definitions"; }
  static Result run(const ir::Function& function, AnalysisManager&) {
    return ReachingDefinitions(function);
  }
};

/// Where each value is defined, and everywhere it is used.
///
/// The IR already maintains a user list on every value, so this indexes the
/// other direction and gives both a stable, ordered form for dumps and passes.
/// After SSA construction (Phase 6) the definition side becomes trivial --
/// exactly one definition per value -- which is much of the point of SSA.
class UseDefInfo {
public:
  UseDefInfo() = default;
  UseDefInfo(const ir::Function& function);

  /// Instruction defining `value`, or null for an argument or constant.
  const ir::Instruction* definitionOf(const ir::Value* value) const;

  /// Instructions using `value`, in program order.
  const std::vector<const ir::Instruction*>& usersOf(const ir::Value* value) const;

  /// Values `instruction` reads, in operand order.
  std::vector<const ir::Value*> operandsOf(const ir::Instruction* instruction) const;

private:
  std::unordered_map<const ir::Value*, const ir::Instruction*> definition_;
  std::unordered_map<const ir::Value*, std::vector<const ir::Instruction*>> users_;
  std::vector<const ir::Instruction*> empty_;
};

struct UseDefAnalysis {
  using Result = UseDefInfo;
  static const char* name() { return "use-def"; }
  static Result run(const ir::Function& function, AnalysisManager&) {
    return UseDefInfo(function);
  }
};

}  // namespace optiforge::analysis
