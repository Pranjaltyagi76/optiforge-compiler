#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "optiforge/backend/CodeGen.h"
#include "optiforge/backend/MachineIR.h"

namespace optiforge::backend {

namespace {

/// The label a jump goes to, or empty when the instruction is not a jump.
std::string jumpTarget(const MInstr& instruction) {
  if (instruction.isLabel || instruction.operands.empty()) {
    return {};
  }
  if (std::strncmp(instruction.mnemonic, "j", 1) != 0) {
    return {};
  }
  const MOperand& operand = instruction.operands.front();
  return operand.kind == MOperand::Kind::Label ? operand.label : std::string{};
}

/// Successors of a block, recovered from the jumps it ends with.
///
/// The machine IR records branch targets as label strings rather than block
/// pointers, so this reads them back. Cheap, and it means layout needs no
/// parallel structure that could fall out of sync with the instructions.
std::vector<std::string> successorsOf(const MBasicBlock& block) {
  std::vector<std::string> targets;
  for (const MInstr& instruction : block.instructions) {
    const std::string target = jumpTarget(instruction);
    if (!target.empty()) {
      targets.push_back(target);
    }
  }
  return targets;
}

/// Greedy chain construction (System_design.md §16.4).
///
/// Start at the entry, repeatedly append the hottest successor not yet placed,
/// and when the chain runs out start a new one from the hottest block left.
/// Zero-count blocks end up last, which is the whole point: code that never ran
/// should not sit between two pieces of code that did.
std::vector<std::size_t> orderByFrequency(const MFunction& function) {
  std::unordered_map<std::string, std::size_t> byLabel;
  for (std::size_t i = 0; i < function.blocks.size(); ++i) {
    byLabel.emplace(function.blocks[i].label, i);
  }

  std::vector<std::size_t> order;
  std::vector<bool> placed(function.blocks.size(), false);

  const auto place = [&](std::size_t index) {
    placed[index] = true;
    order.push_back(index);
  };

  // The entry block always leads: it carries the prologue.
  if (function.blocks.empty()) {
    return order;
  }
  place(0);

  while (order.size() < function.blocks.size()) {
    // Extend the current chain with its hottest unplaced successor.
    bool extended = true;
    while (extended) {
      extended = false;
      std::size_t best = function.blocks.size();
      std::uint64_t bestCount = 0;
      for (const std::string& target : successorsOf(function.blocks[order.back()])) {
        const auto it = byLabel.find(target);
        if (it == byLabel.end() || placed[it->second]) {
          continue;
        }
        const std::uint64_t count = function.blocks[it->second].executionCount;
        if (best == function.blocks.size() || count > bestCount) {
          best = it->second;
          bestCount = count;
        }
      }
      if (best != function.blocks.size()) {
        place(best);
        extended = true;
      }
    }

    // Chain ended. Start the next one at the hottest block still unplaced,
    // breaking ties by original order so the result is reproducible.
    std::size_t next = function.blocks.size();
    std::uint64_t nextCount = 0;
    for (std::size_t i = 0; i < function.blocks.size(); ++i) {
      if (placed[i]) {
        continue;
      }
      if (next == function.blocks.size() ||
          function.blocks[i].executionCount > nextCount) {
        next = i;
        nextCount = function.blocks[i].executionCount;
      }
    }
    if (next == function.blocks.size()) {
      break;
    }
    place(next);
  }

  return order;
}

/// The opposite jump, for inverting a conditional so the hot side falls through.
///
/// Every conditional the code generator emits appears here, in both directions.
/// A jump missing from this table is not wrong, only a missed fall-through --
/// but the signed and unsigned families must not be mixed, because `jl` and
/// `jb` test different flags and swapping one for the other silently
/// miscompiles every comparison it touches.
const char* inverted(const char* mnemonic) {
  static const struct {
    const char* from;
    const char* to;
  } kPairs[] = {
      {"je", "jne"},   {"jne", "je"},    // equality
      {"jl", "jge"},   {"jge", "jl"},    // signed
      {"jg", "jle"},   {"jle", "jg"},
      {"jb", "jae"},   {"jae", "jb"},    // unsigned, which is what comisd needs
      {"ja", "jbe"},   {"jbe", "ja"},
      {"jp", "jnp"},   {"jnp", "jp"},    // parity, for unordered floats
  };
  for (const auto& pair : kPairs) {
    if (std::strcmp(mnemonic, pair.from) == 0) {
      return pair.to;
    }
  }
  return nullptr;
}

/// Removes jumps that go to the very next block.
///
/// A conditional branch is emitted as `jcc taken` followed by `jmp not-taken`.
/// Whichever of those two lands next in the layout costs nothing to reach, so
/// the jump to it can go -- and when it is the *taken* side, inverting the
/// condition is what makes that possible.
///
/// Independent of any profile: this is a straight win on every build, and doing
/// it only for profile-guided ones would flatter the comparison.
std::size_t elideFallThroughJumps(MFunction& function) {
  std::size_t removed = 0;

  for (std::size_t i = 0; i < function.blocks.size(); ++i) {
    MBasicBlock& block = function.blocks[i];
    if (block.instructions.empty()) {
      continue;
    }
    const std::string next =
        i + 1 < function.blocks.size() ? function.blocks[i + 1].label : std::string{};
    if (next.empty()) {
      continue;
    }

    MInstr& last = block.instructions.back();
    if (std::strcmp(last.mnemonic, "jmp") == 0 && jumpTarget(last) == next) {
      block.instructions.pop_back();
      ++removed;
      continue;
    }

    // `jcc taken; jmp not-taken` where the taken side is next: invert.
    if (block.instructions.size() < 2 || std::strcmp(last.mnemonic, "jmp") != 0) {
      continue;
    }
    MInstr& conditional = block.instructions[block.instructions.size() - 2];
    const char* opposite = inverted(conditional.mnemonic);
    if (opposite == nullptr || jumpTarget(conditional) != next) {
      continue;
    }

    conditional.mnemonic = opposite;
    conditional.operands = last.operands;
    conditional.comment = conditional.comment.empty()
                              ? std::string("inverted: hot side falls through")
                              : conditional.comment + "  inverted";
    block.instructions.pop_back();
    ++removed;
  }

  return removed;
}

}  // namespace

LayoutResult layoutBlocks(MFunction& function, bool useProfile) {
  LayoutResult result;

  if (useProfile && function.blocks.size() > 2) {
    const std::vector<std::size_t> order = orderByFrequency(function);
    if (order.size() == function.blocks.size()) {
      std::vector<MBasicBlock> reordered;
      reordered.reserve(order.size());
      for (std::size_t index : order) {
        reordered.push_back(std::move(function.blocks[index]));
      }
      for (std::size_t i = 0; i < order.size(); ++i) {
        result.moved += (order[i] != i) ? 1 : 0;
      }
      function.blocks = std::move(reordered);
    }
  }

  result.jumpsRemoved = elideFallThroughJumps(function);
  return result;
}

}  // namespace optiforge::backend
