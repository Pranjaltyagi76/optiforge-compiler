#pragma once

#include <algorithm>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "optiforge/analysis/BitSet.h"
#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"

namespace optiforge::analysis {

enum class Direction { Forward, Backward };

/// In and out sets for every block.
struct DataflowResult {
  std::unordered_map<const ir::BasicBlock*, BitSet> in;
  std::unordered_map<const ir::BasicBlock*, BitSet> out;
  /// Worklist iterations taken to converge, for tests and for spotting an
  /// analysis that fails to reach a fixed point.
  unsigned iterations = 0;
};

/// Generic worklist dataflow solver.
///
/// Requirement AN-11 asks that a new analysis need no changes to existing
/// ones; this is what delivers that. Liveness and reaching definitions are each
/// about fifty lines on top of it -- they supply a meet and a transfer function
/// and nothing else.
///
///   meet(accumulator, contribution)  combines information from neighbours
///   transfer(block, input, output)   computes a block's output from its input
///
/// Forward analyses flow along CFG edges and meet over predecessors; backward
/// analyses flow against them and meet over successors.
template <class MeetFn, class TransferFn>
DataflowResult runDataflow(const ir::Function& function, Direction direction,
                           std::size_t domainSize, const BitSet& boundary,
                           const BitSet& initial, MeetFn meet, TransferFn transfer) {
  DataflowResult result;
  const bool forward = direction == Direction::Forward;

  for (const auto& block : function.blocks()) {
    result.in.emplace(block.get(), initial);
    result.out.emplace(block.get(), initial);
  }
  if (function.blocks().empty()) {
    return result;
  }

  // The boundary condition applies at the entry for a forward analysis and at
  // every exit block for a backward one.
  if (forward) {
    result.in[function.entry()] = boundary;
  } else {
    for (const auto& block : function.blocks()) {
      if (block->successors().empty()) {
        result.out[block.get()] = boundary;
      }
    }
  }

  // Seed the worklist in the order that converges fastest: reverse postorder
  // for forward analyses, its reverse for backward ones.
  std::vector<const ir::BasicBlock*> order;
  order.reserve(function.blocks().size());
  for (const auto& block : function.blocks()) {
    order.push_back(block.get());
  }
  if (!forward) {
    std::reverse(order.begin(), order.end());
  }

  // successors() yields non-const pointers and predecessors() const ones, so
  // both are normalized here rather than at each use.
  const auto successorsOf = [](const ir::BasicBlock* block) {
    std::vector<const ir::BasicBlock*> list;
    for (const ir::BasicBlock* successor : block->successors()) {
      list.push_back(successor);
    }
    return list;
  };
  const auto predecessorsOf = [](const ir::BasicBlock* block) {
    return std::vector<const ir::BasicBlock*>(block->predecessors().begin(),
                                              block->predecessors().end());
  };

  std::deque<const ir::BasicBlock*> worklist(order.begin(), order.end());
  std::unordered_set<const ir::BasicBlock*> queued(order.begin(), order.end());

  while (!worklist.empty()) {
    const ir::BasicBlock* block = worklist.front();
    worklist.pop_front();
    queued.erase(block);
    ++result.iterations;

    // Meet over neighbours in the incoming direction.
    const bool isBoundary = forward ? (block == function.entry())
                                    : block->successors().empty();
    if (!isBoundary) {
      BitSet incoming(domainSize);
      bool first = true;
      const std::vector<const ir::BasicBlock*> neighbours =
          forward ? predecessorsOf(block) : successorsOf(block);

      for (const ir::BasicBlock* neighbour : neighbours) {
        const BitSet& contribution =
            forward ? result.out[neighbour] : result.in[neighbour];
        if (first) {
          incoming = contribution;
          first = false;
        } else {
          meet(incoming, contribution);
        }
      }
      if (first) {
        incoming = initial;
      }
      (forward ? result.in : result.out)[block] = incoming;
    }

    BitSet produced(domainSize);
    transfer(*block, (forward ? result.in : result.out)[block], produced);

    BitSet& stored = (forward ? result.out : result.in)[block];
    if (stored != produced) {
      stored = produced;
      // Only neighbours downstream of this block can be affected.
      const std::vector<const ir::BasicBlock*> affected =
          forward ? successorsOf(block) : predecessorsOf(block);
      for (const ir::BasicBlock* neighbour : affected) {
        if (queued.insert(neighbour).second) {
          worklist.push_back(neighbour);
        }
      }
    }
  }

  return result;
}

}  // namespace optiforge::analysis
