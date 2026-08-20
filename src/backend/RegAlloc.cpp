#include "optiforge/backend/RegAlloc.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <set>
#include <unordered_set>
#include <utility>

#include "optiforge/analysis/AnalysisManager.h"
#include "optiforge/analysis/BitSet.h"
#include "optiforge/analysis/Dataflow.h"
#include "optiforge/analysis/LoopInfo.h"
#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Instruction.h"

namespace optiforge::backend {

namespace {

/// Integer or floating-point. A value's class follows from its type, and the
/// two never compete for the same register, so they are coloured separately.
enum class RegClass : std::uint8_t { Integer, Float };

RegClass classOf(const ir::Value& value) {
  return value.type()->isF64() ? RegClass::Float : RegClass::Integer;
}

/// One thing to allocate.
///
/// Usually one IR value. Several when SSA destruction has coalesced them onto a
/// common location: the copies belonging to one phi all write where the phi's
/// users read, so they share a unit and therefore a register.
struct Unit {
  std::vector<const ir::Value*> members;
  RegClass regClass = RegClass::Integer;
  std::set<std::size_t> neighbours;  // ordered, so colouring is deterministic

  /// True when this unit is live across a call, which rules out every
  /// caller-saved register.
  bool crossesCall = false;

  /// Sum of 10^loopDepth over the blocks that define or use it -- the static
  /// stand-in for "how often does this run". Phase 11 replaces the base with a
  /// measured execution count (PGO-08), which is the whole reason the cost is
  /// factored out rather than inlined into the spill choice.
  double useWeight = 0.0;

  bool removed = false;    // taken out of the graph during simplify
  bool spilled = false;    // no register available at select
  MReg color = MReg::None;
  std::size_t mergedInto = 0;  // coalescing: index of the surviving unit
  bool merged = false;
};

/// Union-find over allocation units, used for both the mandatory merging of
/// slot-aliased values and for coalescing.
std::size_t findRoot(std::vector<Unit>& units, std::size_t index) {
  while (units[index].merged) {
    index = units[index].mergedInto;
  }
  return index;
}

/// The location a value occupies once SSA destruction's coalescing is applied.
const ir::Value* locationOf(const ir::Value* value) {
  if (value != nullptr && value->valueKind() == ir::Value::Kind::Instruction) {
    const auto* instruction = static_cast<const ir::Instruction*>(value);
    if (instruction->slotAlias() != nullptr) {
      return instruction->slotAlias();
    }
  }
  return value;
}

/// True when this value can live in a register at all.
///
/// An alloca is excluded because its *value* is the address of a frame slot:
/// the slot has to exist, and code addresses it directly. Everything else that
/// produces a result is a candidate, arguments included.
bool isCandidate(const ir::Value* value) {
  if (value == nullptr || value->type() == nullptr || value->type()->isVoid()) {
    return false;
  }
  if (value->valueKind() == ir::Value::Kind::Argument) {
    return true;
  }
  if (value->valueKind() != ir::Value::Kind::Instruction) {
    return false;  // constants are materialized inline
  }
  const auto& instruction = *static_cast<const ir::Instruction*>(value);
  return instruction.hasResult() && instruction.opcode() != ir::Opcode::Alloca;
}

/// True for the self-referencing copy SSA destruction puts at the top of a
/// block holding a phi.
///
/// `%x = copy %x` reads and writes the same location, which is to say it does
/// nothing at all. It exists so the phi's users have one dominating value to
/// name, and it reads itself so value-liveness carries the location back to the
/// predecessors that wrote it. Treating it as a real definition here would kill
/// the location at the top of the block it is live into; treating it as a real
/// use would keep the location live all the way to the function entry. It is
/// neither, and skipping it outright is what makes the live ranges below exact.
bool isTransparentSelfCopy(const ir::Instruction& instruction) {
  return instruction.opcode() == ir::Opcode::Copy && instruction.operandCount() == 1 &&
         instruction.operand(0) == &instruction;
}

double weightForDepth(unsigned depth) {
  double weight = 1.0;
  for (unsigned i = 0; i < depth && i < 6; ++i) {
    weight *= 10.0;  // capped, or a deep nest overflows the comparison entirely
  }
  return weight;
}

/// Everything the builder needs to go from a function to a coloured graph.
class Allocator {
public:
  Allocator(const ir::Function& function, analysis::AnalysisManager& manager,
            const TargetInfo& target, bool useProfileWeights)
      : function_(function),
        target_(target),
        loops_(manager.get<analysis::LoopAnalysis>(function)),
        useProfileWeights_(useProfileWeights) {}

  RegisterAssignment run() {
    buildUnits();
    if (units_.empty()) {
      return std::move(result_);
    }
    computeUnitLiveness();
    buildInterference();
    addArgumentInterference();
    colorClass(RegClass::Integer, target_.allocatableIntRegisters());
    colorClass(RegClass::Float, target_.allocatableFloatRegisters());
    publish();
    return std::move(result_);
  }

private:
  // -------------------------------------------------------------------------
  // Units
  // -------------------------------------------------------------------------

  std::size_t unitOf(const ir::Value* value) {
    const auto it = unitIndex_.find(locationOf(value));
    return it == unitIndex_.end() ? kNoUnit : it->second;
  }

  void buildUnits() {
    const auto add = [&](const ir::Value* value) {
      if (!isCandidate(value)) {
        return;
      }
      const ir::Value* location = locationOf(value);
      auto it = unitIndex_.find(location);
      if (it == unitIndex_.end()) {
        Unit unit;
        unit.regClass = classOf(*value);
        it = unitIndex_.emplace(location, units_.size()).first;
        units_.push_back(std::move(unit));
      }
      units_[it->second].members.push_back(value);
    };

    for (const auto& argument : function_.arguments()) {
      add(argument.get());
    }
    for (const auto& block : function_.blocks()) {
      for (const auto& instruction : block->instructions()) {
        add(instruction.get());
      }
    }

    // Use weights: every mention of a value in a block costs what that block
    // costs to run.
    //
    // A measured execution count when there is one, loop depth when there is
    // not (PGO-08). Loop depth assumes every loop runs about ten times and
    // every branch is taken half the time; a profile replaces both guesses with
    // the number. A loop the compiler statically believes is hot but which ran
    // twice stops stealing registers from the one that ran fifty million times.
    for (const auto& block : function_.blocks()) {
      const double weight = useProfileWeights_ && block->executionCount > 0
                                ? static_cast<double>(block->executionCount)
                                : weightForDepth(loops_.depthOf(block.get()));
      for (const auto& instruction : block->instructions()) {
        const std::size_t defined = unitOf(instruction.get());
        if (defined != kNoUnit) {
          units_[defined].useWeight += weight;
        }
        for (std::size_t i = 0; i < instruction->operandCount(); ++i) {
          const std::size_t used = unitOf(instruction->operand(i));
          if (used != kNoUnit) {
            units_[used].useWeight += weight;
          }
        }
      }
    }

    result_.candidates = units_.size();
  }

  // -------------------------------------------------------------------------
  // Live ranges
  // -------------------------------------------------------------------------

  /// Liveness over allocation units rather than over values.
  ///
  /// `analysis::Liveness` answers a different question than the allocator asks.
  /// It tracks values, and SSA destruction leaves several values sharing one
  /// location: a location is written by every one of a phi's edge copies and
  /// read through the root copy, and no single value's live range describes
  /// that. Taking the union of the members' value ranges over-approximates
  /// badly -- the root copy's self-read drags the location back to the function
  /// entry, so two locations belonging to unrelated loops both look live
  /// everywhere and interfere for no reason.
  ///
  /// The domain here is units, so a location is killed by *any* of its member
  /// definitions, and the answer is exact. Same solver (AN-11), different
  /// domain.
  void computeUnitLiveness() {
    const std::size_t domain = units_.size();
    const analysis::BitSet empty(domain);

    const analysis::DataflowResult solved = analysis::runDataflow(
        function_, analysis::Direction::Backward, domain, empty, empty,
        [](analysis::BitSet& accumulator, const analysis::BitSet& contribution) {
          accumulator.unionWith(contribution);
        },
        [&](const ir::BasicBlock& block, const analysis::BitSet& liveOut,
            analysis::BitSet& liveIn) {
          liveIn = liveOut;
          const auto& instructions = block.instructions();
          for (auto it = instructions.rbegin(); it != instructions.rend(); ++it) {
            const ir::Instruction& instruction = **it;
            if (isTransparentSelfCopy(instruction)) {
              continue;
            }
            const std::size_t defined = unitOf(&instruction);
            if (defined != kNoUnit) {
              liveIn.reset(defined);
            }
            for (std::size_t i = 0; i < instruction.operandCount(); ++i) {
              const std::size_t used = unitOf(instruction.operand(i));
              if (used != kNoUnit) {
                liveIn.set(used);
              }
            }
          }
        });

    for (const auto& block : function_.blocks()) {
      liveOut_.emplace(block.get(), solved.out.at(block.get()));
      liveIn_.emplace(block.get(), solved.in.at(block.get()));
    }
  }

  std::unordered_set<std::size_t> liveOutUnits(const ir::BasicBlock* block) const {
    std::unordered_set<std::size_t> live;
    const auto it = liveOut_.find(block);
    if (it != liveOut_.end()) {
      for (std::size_t unit : it->second.elements()) {
        live.insert(unit);
      }
    }
    return live;
  }

  // -------------------------------------------------------------------------
  // Interference
  // -------------------------------------------------------------------------

  void interfere(std::size_t a, std::size_t b) {
    if (a == kNoUnit || b == kNoUnit || a == b) {
      return;
    }
    // Two values of different classes can never contend for one register, so an
    // edge between them would only inflate degrees and cause spills that are
    // not needed.
    if (units_[a].regClass != units_[b].regClass) {
      return;
    }
    units_[a].neighbours.insert(b);
    units_[b].neighbours.insert(a);
  }

  void buildInterference() {
    for (const auto& block : function_.blocks()) {
      // Walk backwards from the block's live-out set, which is the standard way
      // to get interference exactly: two values interfere when one is live at
      // the point the other is defined.
      std::unordered_set<std::size_t> live = liveOutUnits(block.get());

      const auto& instructions = block->instructions();
      for (auto it = instructions.rbegin(); it != instructions.rend(); ++it) {
        const ir::Instruction& instruction = **it;
        if (isTransparentSelfCopy(instruction)) {
          continue;  // neither a definition nor a use; see isTransparentSelfCopy
        }
        const std::size_t defined = unitOf(&instruction);

        if (defined != kNoUnit) {
          // Everything live after this instruction interferes with what it
          // defines -- including a copy's source.
          //
          // The textbook build step excludes the source of a move, on the
          // grounds that the two hold the same value at that point. That is
          // only sound when the source dies at the copy: if it is live
          // *afterwards* the two are genuinely simultaneous, and the edge that
          // would otherwise be recovered at the source's next definition never
          // materializes here, because SSA destruction's root copy reads its own
          // location and is treated as neither a definition nor a use.
          //
          // A source that does die at the copy is simply not in `live`, so the
          // move is still coalescable and nothing is lost by dropping the
          // exclusion. Found by the differential fuzzer, on a phi whose location
          // was copied while still needed.
          for (std::size_t other : live) {
            interfere(defined, other);
          }
          live.erase(defined);
        }

        if (instruction.opcode() == ir::Opcode::Call) {
          // Whatever is still live here spans the call, so a caller-saved
          // register would not survive it.
          for (std::size_t unit : live) {
            units_[unit].crossesCall = true;
          }
        }

        maxPressure_ = std::max(maxPressure_, live.size());

        for (std::size_t i = 0; i < instruction.operandCount(); ++i) {
          const std::size_t used = unitOf(instruction.operand(i));
          if (used != kNoUnit) {
            live.insert(used);
          }
        }

        if (instruction.opcode() == ir::Opcode::Copy &&
            instruction.operandCount() == 1 && defined != kNoUnit) {
          const std::size_t source = unitOf(instruction.operand(0));
          if (source != kNoUnit && source != defined &&
              units_[source].regClass == units_[defined].regClass) {
            moves_.push_back({defined, source, false});
          }
        }
      }
      maxPressure_ = std::max(maxPressure_, live.size());
    }
    result_.maxPressure = maxPressure_;
  }

  /// Arguments are defined at the function's entry point, by the calling
  /// convention, and so interfere with everything else live there.
  ///
  /// The backward walk cannot see that: it adds an edge where a value is
  /// *defined*, and an argument has no defining instruction to hang that on.
  /// Without this, two arguments both live for the whole function had no edge
  /// between them at all and were cheerfully given the same register.
  void addArgumentInterference() {
    const ir::BasicBlock* entry = function_.entry();
    if (entry == nullptr) {
      return;
    }

    std::vector<std::size_t> liveAtEntry;
    const auto it = liveIn_.find(entry);
    if (it != liveIn_.end()) {
      for (std::size_t unit : it->second.elements()) {
        liveAtEntry.push_back(unit);
      }
    }

    for (const auto& argument : function_.arguments()) {
      const std::size_t defined = unitOf(argument.get());
      if (defined == kNoUnit) {
        continue;
      }
      for (std::size_t other : liveAtEntry) {
        interfere(defined, other);
      }
    }
  }

  // -------------------------------------------------------------------------
  // Coalescing (Briggs' conservative test)
  // -------------------------------------------------------------------------

  /// Merges the two ends of a copy when the merged node is provably colourable:
  /// fewer than K neighbours of significant degree. Conservative in the exact
  /// sense that matters -- it never turns a colourable graph into one that is
  /// not, which aggressive coalescing can.
  /// A unit's neighbours, resolved to the units that currently represent them.
  ///
  /// Merging leaves stale indices behind: an edge recorded against a unit that
  /// has since been absorbed still names the absorbed index. Reading them raw
  /// makes `count(b)` miss an edge that is really there, and coalescing then
  /// merges two units that interfere -- which is a miscompile, and was one.
  /// Every read of a neighbour set goes through here.
  std::size_t rootOf(std::size_t index) const {
    while (units_[index].merged) {
      index = units_[index].mergedInto;
    }
    return index;
  }

  std::set<std::size_t> neighbourRoots(std::size_t index) const {
    std::set<std::size_t> result;
    for (std::size_t neighbour : units_[index].neighbours) {
      std::size_t root = neighbour;
      while (units_[root].merged) {
        root = units_[root].mergedInto;
      }
      if (root != index) {
        result.insert(root);
      }
    }
    return result;
  }

  /// Briggs' conservative test: the merged node is safe to create when it has
  /// fewer than K neighbours of significant degree.
  ///
  /// Conservative in the sense that matters -- it can refuse a merge that would
  /// have been fine, but it never turns a colourable graph into one that is not,
  /// which aggressive coalescing can.
  bool briggsAllows(std::size_t a, std::size_t b, std::size_t k) const {
    std::set<std::size_t> combined = neighbourRoots(a);
    const std::set<std::size_t> other = neighbourRoots(b);
    combined.insert(other.begin(), other.end());

    std::size_t significant = 0;
    for (std::size_t neighbour : combined) {
      if (neighbour == a || neighbour == b || units_[neighbour].removed) {
        continue;
      }
      if (activeNeighbours(neighbour).size() >= k) {
        ++significant;
      }
    }
    return significant < k;
  }

  /// True when this node still has a move that might yet be coalesced. Such a
  /// node is not simplified: removing it would throw away the chance to put the
  /// copy's two ends in one register and delete the copy.
  bool isMoveRelated(std::size_t node) const {
    for (const Move& move : moves_) {
      if (move.settled) {
        continue;
      }
      const std::size_t destination = rootOf(move.destination);
      const std::size_t source = rootOf(move.source);
      if (destination == node || source == node) {
        return true;
      }
    }
    return false;
  }

  /// Tries every pending move of this class once. Returns true if one merged.
  bool coalesceOne(RegClass regClass, std::size_t k) {
    for (Move& move : moves_) {
      if (move.settled) {
        continue;
      }
      const std::size_t a = rootOf(move.destination);
      const std::size_t b = rootOf(move.source);

      if (units_[a].regClass != regClass) {
        continue;  // the other class's turn
      }
      if (a == b) {
        move.settled = true;  // already one node; nothing left to do
        continue;
      }
      if (units_[a].removed || units_[b].removed) {
        continue;  // one end is off the graph; revisit if it comes back
      }
      if (neighbourRoots(a).count(b) != 0 || neighbourRoots(b).count(a) != 0) {
        move.settled = true;  // constrained: they interfere and never can merge
        continue;
      }
      if (!briggsAllows(a, b, k)) {
        continue;  // not now; a later simplify may make it safe
      }

      mergeUnits(a, b);
      move.settled = true;
      ++result_.coalesced;
      return true;
    }
    return false;
  }

  /// Gives up on coalescing the moves of one low-degree node.
  ///
  /// Reached when nothing simplifies and nothing coalesces but there are still
  /// low-degree nodes left, all of them held back only by a move. Freezing the
  /// moves of one makes it an ordinary node that simplifies immediately. The
  /// copy stays in the generated code; that is the price of not spilling.
  bool freezeOne(RegClass regClass, std::size_t k) {
    for (std::size_t node = 0; node < units_.size(); ++node) {
      if (units_[node].merged || units_[node].removed ||
          units_[node].regClass != regClass) {
        continue;
      }
      if (activeNeighbours(node).size() >= k || !isMoveRelated(node)) {
        continue;
      }
      for (Move& move : moves_) {
        if (move.settled) {
          continue;
        }
        if (rootOf(move.destination) == node || rootOf(move.source) == node) {
          move.settled = true;
          ++result_.frozen;
        }
      }
      return true;
    }
    return false;
  }

  void mergeUnits(std::size_t keep, std::size_t gone) {
    Unit& survivor = units_[keep];
    Unit& absorbed = units_[gone];

    survivor.members.insert(survivor.members.end(), absorbed.members.begin(),
                            absorbed.members.end());
    survivor.crossesCall = survivor.crossesCall || absorbed.crossesCall;
    survivor.useWeight += absorbed.useWeight;

    for (std::size_t neighbour : absorbed.neighbours) {
      const std::size_t root = findRoot(units_, neighbour);
      if (root == keep) {
        continue;
      }
      survivor.neighbours.insert(root);
      units_[root].neighbours.insert(keep);
    }
    for (std::size_t neighbour : absorbed.neighbours) {
      units_[findRoot(units_, neighbour)].neighbours.erase(gone);
    }
    survivor.neighbours.erase(gone);

    absorbed.merged = true;
    absorbed.mergedInto = keep;
    absorbed.neighbours.clear();
    absorbed.members.clear();
  }

  // -------------------------------------------------------------------------
  // Simplify / spill / select
  // -------------------------------------------------------------------------

  const std::vector<MReg>& registersFor(RegClass regClass) const {
    return regClass == RegClass::Integer ? target_.allocatableIntRegisters()
                                         : target_.allocatableFloatRegisters();
  }

  /// Neighbours of `index` that are still in the graph.
  std::vector<std::size_t> activeNeighbours(std::size_t index) const {
    std::vector<std::size_t> result;
    for (std::size_t neighbour : neighbourRoots(index)) {
      if (!units_[neighbour].removed) {
        result.push_back(neighbour);
      }
    }
    return result;
  }

  void colorClass(RegClass regClass, const std::vector<MReg>& registers) {
    const std::size_t k = registers.size();

    bool any = false;
    for (std::size_t i = 0; i < units_.size(); ++i) {
      if (!units_[i].merged && units_[i].regClass == regClass) {
        any = true;
        if (k == 0) {
          units_[i].spilled = true;
        }
      }
    }
    if (!any || k == 0) {
      return;  // nothing of this class, or nowhere to put it
    }

    // The Chaitin-Briggs main loop. Each round does the cheapest thing still
    // available, and every branch either removes a node, merges two, or settles
    // a move, so it terminates.
    //
    //   simplify  a low-degree node that no move cares about
    //   coalesce  a copy whose two ends can safely share a register
    //   freeze    give up coalescing one low-degree node so it can simplify
    //   spill     nothing is low-degree; push the cheapest node optimistically
    //
    // Degrees are recomputed rather than maintained incrementally. That is
    // O(n^2) where the textbook is O(n), and at the scale this compiler works
    // at -- tens of units per function -- it is not measurable, while the
    // incremental version is where this algorithm is usually got wrong.
    std::vector<std::size_t> order;

    const auto stillInGraph = [&](std::size_t node) {
      return !units_[node].merged && !units_[node].removed &&
             units_[node].regClass == regClass;
    };

    const auto simplifyOne = [&]() {
      for (std::size_t node = 0; node < units_.size(); ++node) {
        if (!stillInGraph(node) || activeNeighbours(node).size() >= k ||
            isMoveRelated(node)) {
          continue;
        }
        units_[node].removed = true;
        order.push_back(node);
        return true;
      }
      return false;
    };

    // Nothing is low-degree any more, so something has to go to memory. Pick
    // the node that costs least to keep in memory and blocks the most others:
    // low use weight, high degree. Pushing it optimistically rather than
    // spilling it outright usually pays off, because a node's neighbours are
    // rarely all different colours.
    const auto spillOne = [&]() {
      std::size_t victim = kNoUnit;
      double bestScore = 0.0;
      for (std::size_t node = 0; node < units_.size(); ++node) {
        if (!stillInGraph(node)) {
          continue;
        }
        const double degree = static_cast<double>(activeNeighbours(node).size());
        const double score = units_[node].useWeight / (degree > 0.0 ? degree : 1.0);
        if (victim == kNoUnit || score < bestScore) {
          victim = node;
          bestScore = score;
        }
      }
      if (victim == kNoUnit) {
        return false;
      }
      units_[victim].removed = true;
      order.push_back(victim);
      return true;
    };

    while (true) {
      if (simplifyOne()) {
        continue;
      }
      if (coalesceOne(regClass, k)) {
        continue;
      }
      if (freezeOne(regClass, k)) {
        continue;
      }
      if (!spillOne()) {
        break;  // the graph is empty
      }
    }

    // Select: pop in reverse and take a colour no live neighbour holds.
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
      const std::size_t node = *it;
      units_[node].removed = false;

      std::unordered_set<int> taken;
      for (std::size_t neighbour : activeNeighbours(node)) {
        if (units_[neighbour].color != MReg::None) {
          taken.insert(static_cast<int>(units_[neighbour].color));
        }
      }

      MReg chosen = MReg::None;
      for (MReg candidate : registers) {
        if (taken.count(static_cast<int>(candidate)) != 0) {
          continue;
        }
        if (units_[node].crossesCall && !target_.isCalleeSaved(candidate)) {
          continue;  // a call would destroy it
        }
        chosen = candidate;
        break;
      }

      if (chosen == MReg::None) {
        units_[node].spilled = true;  // the optimism did not pay off
      } else {
        units_[node].color = chosen;
      }
    }
  }

  // -------------------------------------------------------------------------
  // Result
  // -------------------------------------------------------------------------

  void publish() {
    std::set<int> calleeSaved;
    for (std::size_t i = 0; i < units_.size(); ++i) {
      const std::size_t root = findRoot(units_, i);
      const Unit& unit = units_[root];
      if (unit.spilled || unit.color == MReg::None) {
        continue;
      }
      for (const ir::Value* member : units_[i].members) {
        result_.assigned.emplace(member, unit.color);
      }
      if (target_.isCalleeSaved(unit.color)) {
        calleeSaved.insert(static_cast<int>(unit.color));
      }
    }

    for (std::size_t i = 0; i < units_.size(); ++i) {
      if (units_[i].merged) {
        continue;
      }
      if (units_[i].spilled || units_[i].color == MReg::None) {
        ++result_.spilled;
      } else {
        ++result_.colored;
      }
    }

    // Ordered, so the prologue is byte-identical across runs (NFR-06).
    for (int reg : calleeSaved) {
      result_.usedCalleeSaved.push_back(static_cast<MReg>(reg));
    }
  }

  static constexpr std::size_t kNoUnit = static_cast<std::size_t>(-1);

  const ir::Function& function_;
  const TargetInfo& target_;
  const analysis::LoopInfo& loops_;
  /// False makes spill cost ignore measured block counts (--disable-pgo=regalloc).
  bool useProfileWeights_ = true;

  std::vector<Unit> units_;
  std::unordered_map<const ir::BasicBlock*, analysis::BitSet> liveOut_;
  std::unordered_map<const ir::BasicBlock*, analysis::BitSet> liveIn_;
  std::unordered_map<const ir::Value*, std::size_t> unitIndex_;
  /// Copies whose two ends would rather share a register. `settled` is set once
  /// a move can never be coalesced again -- merged, proven to interfere, or
  /// frozen to let its nodes simplify.
  struct Move {
    std::size_t destination;
    std::size_t source;
    bool settled = false;
  };
  std::vector<Move> moves_;
  std::size_t maxPressure_ = 0;
  RegisterAssignment result_;
};

}  // namespace

std::string RegisterAssignment::summary() const {
  std::string text = "@" + function + ": " + std::to_string(candidates) +
                     " unit(s), " + std::to_string(colored) + " coloured, " +
                     std::to_string(spilled) + " spilled, " +
                     std::to_string(coalesced) + " coalesced, " +
                     std::to_string(frozen) + " frozen, peak " +
                     std::to_string(maxPressure) + " live";
  if (!usedCalleeSaved.empty()) {
    text += ", saves";
    for (MReg reg : usedCalleeSaved) {
      text += " " + std::string(regName(reg));
    }
  }
  return text;
}

RegisterAssignment allocateRegisters(const ir::Function& function,
                                     analysis::AnalysisManager& manager,
                                     const TargetInfo& target,
                                     bool useProfileWeights) {
  if (function.isDeclaration()) {
    return {};
  }
  Allocator allocator(function, manager, target, useProfileWeights);
  RegisterAssignment assignment = allocator.run();
  assignment.function = function.name();
  return assignment;
}

std::vector<std::string> verifyAssignment(const ir::Function& function,
                                          analysis::AnalysisManager& manager,
                                          const TargetInfo& target,
                                          const RegisterAssignment& assignment) {
  (void)manager;
  std::vector<std::string> errors;
  if (function.isDeclaration() || assignment.assigned.empty()) {
    return errors;
  }

  const std::string prefix = "function @" + function.name();
  const auto nameOf = [](const ir::Value* value) {
    return value->hasName() ? "%" + value->name() : std::string("<unnamed>");
  };

  // Locations, recomputed from the IR rather than taken from the allocator.
  //
  // The allocator adds an interference edge where a location is *defined*; this
  // checks every pair of locations live at every point. The two are equivalent
  // when the live ranges are exact, and they diverge exactly when they are not
  // -- which is the failure this is here to catch. It found the missing edges
  // between function arguments, whose definition point no backward walk over
  // instructions ever reaches.
  std::vector<const ir::Value*> locations;
  std::unordered_map<const ir::Value*, std::size_t> index;
  const auto locate = [&](const ir::Value* value) -> std::size_t {
    if (!isCandidate(value)) {
      return static_cast<std::size_t>(-1);
    }
    const ir::Value* location = locationOf(value);
    const auto it = index.find(location);
    if (it != index.end()) {
      return it->second;
    }
    index.emplace(location, locations.size());
    locations.push_back(location);
    return locations.size() - 1;
  };

  for (const auto& argument : function.arguments()) {
    locate(argument.get());
  }
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block->instructions()) {
      locate(instruction.get());
      for (std::size_t i = 0; i < instruction->operandCount(); ++i) {
        locate(instruction->operand(i));
      }
    }
  }
  if (locations.empty()) {
    return errors;
  }

  const std::size_t domain = locations.size();
  const analysis::BitSet empty(domain);
  const auto indexOf = [&](const ir::Value* value) -> std::size_t {
    if (value == nullptr) {
      return domain;
    }
    const auto it = index.find(locationOf(value));
    return it == index.end() ? domain : it->second;
  };

  const auto transfer = [&](const ir::BasicBlock& block, const analysis::BitSet& liveOut,
                            analysis::BitSet& liveIn) {
    liveIn = liveOut;
    const auto& instructions = block.instructions();
    for (auto it = instructions.rbegin(); it != instructions.rend(); ++it) {
      const ir::Instruction& instruction = **it;
      if (isTransparentSelfCopy(instruction)) {
        continue;
      }
      const std::size_t defined = indexOf(&instruction);
      if (defined < domain) {
        liveIn.reset(defined);
      }
      for (std::size_t i = 0; i < instruction.operandCount(); ++i) {
        const std::size_t used = indexOf(instruction.operand(i));
        if (used < domain) {
          liveIn.set(used);
        }
      }
    }
  };

  const analysis::DataflowResult solved = analysis::runDataflow(
      function, analysis::Direction::Backward, domain, empty, empty,
      [](analysis::BitSet& accumulator, const analysis::BitSet& contribution) {
        accumulator.unionWith(contribution);
      },
      transfer);

  for (const auto& block : function.blocks()) {
    std::unordered_set<std::size_t> live;
    for (std::size_t slot : solved.out.at(block.get()).elements()) {
      live.insert(slot);
    }

    const auto check = [&](const std::string& where) {
      std::unordered_map<int, std::size_t> occupant;
      for (std::size_t slot : live) {
        MReg reg = MReg::None;
        if (!assignment.registerFor(locations[slot], reg)) {
          continue;
        }
        const auto it = occupant.find(static_cast<int>(reg));
        if (it == occupant.end()) {
          occupant.emplace(static_cast<int>(reg), slot);
          continue;
        }
        errors.push_back(prefix + ", block '" + block->label() + "', " + where + ": " +
                         nameOf(locations[slot]) + " and " +
                         nameOf(locations[it->second]) + " are both live in " +
                         std::string(regName(reg)));
      }
    };

    check("block exit");

    const auto& instructions = block->instructions();
    for (auto it = instructions.rbegin(); it != instructions.rend(); ++it) {
      const ir::Instruction& instruction = **it;
      if (isTransparentSelfCopy(instruction)) {
        continue;
      }

      const std::size_t defined = indexOf(&instruction);
      if (defined < domain) {
        live.erase(defined);
      }

      if (instruction.opcode() == ir::Opcode::Call) {
        for (std::size_t slot : live) {
          MReg reg = MReg::None;
          if (assignment.registerFor(locations[slot], reg) && !target.isCalleeSaved(reg)) {
            errors.push_back(prefix + ", block '" + block->label() + "': " +
                             nameOf(locations[slot]) + " lives in caller-saved " +
                             std::string(regName(reg)) + " across a call");
          }
        }
      }

      for (std::size_t i = 0; i < instruction.operandCount(); ++i) {
        const std::size_t used = indexOf(instruction.operand(i));
        if (used < domain) {
          live.insert(used);
        }
      }
      check("before " + std::string(toString(instruction.opcode())));
    }
  }

  return errors;
}

}  // namespace optiforge::backend
