#include <algorithm>
#include <cmath>
#include <memory>
#include <ostream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "optiforge/analysis/AnalysisManager.h"
#include "optiforge/analysis/LoopInfo.h"
#include "optiforge/analysis/ProfileAnalysis.h"
#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Instruction.h"
#include "optiforge/ir/Module.h"
#include "optiforge/passes/Pass.h"
#include "optiforge/profile/Profile.h"

namespace optiforge::transforms {

namespace {

using passes::Pass;
using passes::PassRegistration;

/// Largest body the unroller will replicate, in IR instructions per copy times
/// the factor. Past this the instruction cache costs more than the branches
/// saved (System_design.md §16.2).
constexpr std::size_t kUnrolledSizeBudget = 400;

/// Loop-carried values above which unrolling is refused, whatever the trip
/// count says (the Phase 13 cost model).
///
/// **Why the carried count and not the body size.** A temporary dies inside the
/// copy that computes it, so replicating a body does not lengthen its live
/// range. A *carried* value -- one a header phi brings round -- is live through
/// every copy, and the exit merge is `carried x factor` wide. So the register
/// demand unrolling adds scales with how many values the loop carries, and not
/// with how big it is.
///
/// **Where the number comes from.** The only target has eight allocatable
/// integer registers. A loop carrying more than half of them leaves the body
/// too few to compute in without spilling, and the spill traffic lands in the
/// hot loop -- which is the one place it cannot be afforded. Half of eight is
/// four.
///
/// A mid-level pass cannot ask the backend for that eight without inverting the
/// layering (architectural_design.md section 3, rule 3), so it is a constant
/// here with its provenance written down, exactly as `kUnrolledSizeBudget` is.
///
/// **This was measured, and two other explanations were rejected first.** On the
/// benchmark corpus, unrolling helps `matmul` (+9%), `sieve` (+8%),
/// `nested_math` (+4%) and `loop_sum` (+4%), and costs `loop_kernel` 6%. Neither
/// emitted instruction count nor dependency-chain length predicts which is
/// which -- `loop_kernel` gets *fewer* instructions per iteration and is slower,
/// and `matmul` is the most latency-bound of the five and gains the most. The
/// carried count is the one signal that separates them: 1, 2, 2, 2 against 6.
constexpr std::size_t kCarriedValueBudget = 4;

/// The shape this unroller handles, and the values it needs from it.
///
/// Deliberately narrow. A loop that does not fit is skipped rather than
/// half-transformed: unrolling rewrites SSA across an exit edge, and the way to
/// be sure that is right is to only attempt it where the whole picture is
/// visible in four pointers.
struct SimpleLoop {
  ir::BasicBlock* header = nullptr;  ///< phis, the test, and the conditional branch
  ir::BasicBlock* body = nullptr;    ///< the one body block, which is also the latch
  ir::BasicBlock* exit = nullptr;    ///< where the header goes when the test fails
  bool exitOnFalse = true;           ///< true when successor 0 is the body
  std::vector<ir::Instruction*> phis;
};

/// Recognizes `while (c) { ... }` after the optimizer has been at it: a header
/// carrying the phis and the test, and a single body block that branches back.
///
/// Returns false for anything else. The conditions are not arbitrary -- each one
/// is a place where the SSA rewrite below would otherwise need a case:
///
///   - one body block, which is also the only latch: so "the value on the next
///     iteration" is a single expression rather than a merge;
///   - one exit, reached only from the header: so the values live after the loop
///     all come through the header's phis;
///   - nothing defined inside the loop is used outside it except those phis: so
///     the only SSA repair needed is at the exit.
bool describe(const analysis::Loop& loop, SimpleLoop& out) {
  if (loop.blocks().size() != 2 || loop.latches().size() != 1) {
    return false;
  }

  out.header = const_cast<ir::BasicBlock*>(loop.header());
  out.body = const_cast<ir::BasicBlock*>(loop.latches()[0]);
  if (out.header == out.body) {
    return false;  // a self-loop has no separate body to replicate
  }

  ir::Instruction* headerTerminator = out.header->terminator();
  ir::Instruction* bodyTerminator = out.body->terminator();
  if (headerTerminator == nullptr || bodyTerminator == nullptr ||
      headerTerminator->opcode() != ir::Opcode::CondBr ||
      bodyTerminator->opcode() != ir::Opcode::Br ||
      headerTerminator->successors().size() != 2 ||
      bodyTerminator->successors()[0] != out.header) {
    return false;
  }

  const bool bodyIsFirst = headerTerminator->successors()[0] == out.body;
  const bool bodyIsSecond = headerTerminator->successors()[1] == out.body;
  if (bodyIsFirst == bodyIsSecond) {
    return false;  // both or neither: not a shape with one exit
  }
  out.exitOnFalse = bodyIsFirst;
  out.exit = headerTerminator->successors()[bodyIsFirst ? 1 : 0];
  if (loop.exits().size() != 1 || loop.exits()[0] != out.exit) {
    return false;
  }
  if (out.body->predecessors().size() != 1) {
    return false;  // something else jumps into the body
  }

  for (const auto& instruction : out.header->instructions()) {
    if (instruction->opcode() == ir::Opcode::Phi) {
      // Every phi must take exactly one value from the latch, or "the next
      // iteration's value" is not a single expression.
      bool fromBody = false;
      for (std::size_t i = 0; i < instruction->incomingCount(); ++i) {
        if (instruction->incomingBlock(i) == out.body) {
          if (fromBody) {
            return false;
          }
          fromBody = true;
        }
      }
      if (!fromBody) {
        return false;
      }
      out.phis.push_back(instruction.get());
    }
  }

  // A body that calls something is not worth unrolling. What unrolling removes
  // is one back-edge jump per copy; next to the cost of a call that is nothing,
  // and the copies still grow the code by a factor. Measured: unrolling
  // bench/programs/branchy.of, whose loop is a call and two adds, made it 0.9%
  // *slower* before this check existed.
  for (const auto& instruction : out.body->instructions()) {
    if (instruction->opcode() == ir::Opcode::Call) {
      return false;
    }
  }

  // Nothing defined in the body may be read after the loop. With one body block
  // and one exit that is rare, and handling it would mean building phis for
  // values the header does not carry.
  for (const auto& instruction : out.body->instructions()) {
    for (const ir::Instruction* user : instruction->users()) {
      if (user->parent() != out.header && user->parent() != out.body) {
        return false;
      }
    }
  }

  // The exit block must have no phis of its own yet. It has one predecessor
  // before unrolling, so in practice it never does; if it somehow does, the
  // rewrite below would have to merge them and does not.
  for (const auto& instruction : out.exit->instructions()) {
    if (instruction->opcode() == ir::Opcode::Phi) {
      return false;
    }
    break;
  }

  return true;
}

/// A phi's incoming value along the back edge: what it becomes next time round.
ir::Value* latchValue(const ir::Instruction& phi, const ir::BasicBlock* body) {
  for (std::size_t i = 0; i < phi.incomingCount(); ++i) {
    if (phi.incomingBlock(i) == body) {
      return phi.operand(i);
    }
  }
  return nullptr;
}

using ValueMap = std::unordered_map<const ir::Value*, ir::Value*>;

ir::Value* translate(const ValueMap& map, ir::Value* value) {
  const auto it = map.find(value);
  return it == map.end() ? value : it->second;
}

/// Copies one instruction, rewriting its operands through `map`.
std::unique_ptr<ir::Instruction> cloneInstruction(const ir::Instruction& original,
                                                  const ValueMap& map,
                                                  ir::Function& function) {
  auto clone = std::make_unique<ir::Instruction>(original.opcode(), original.type());
  clone->setPredicate(original.predicate());
  clone->setCallee(original.callee());
  clone->setAllocatedType(original.allocatedType());
  if (original.hasResult()) {
    clone->setName(function.nextTempName());
  }
  for (std::size_t i = 0; i < original.operandCount(); ++i) {
    clone->addOperand(translate(map, original.operand(i)));
  }
  return clone;
}


std::size_t bodySize(const SimpleLoop& loop) {
  return loop.body->instructions().size() + loop.header->instructions().size();
}

/// Factor to unroll a loop with this measured trip count
/// (System_design.md §16.2).
///
/// The trip count is an *average*, not a bound: a loop entered twice for one
/// iteration and once for a thousand averages 334, and no factor is right for
/// both. That is why the shape below is conservative at the low end and capped
/// at the high one -- getting the factor wrong costs code size, never
/// correctness, because every copy re-tests the condition.
unsigned factorFor(double tripCount, std::size_t body) {
  if (std::isnan(tripCount) || tripCount < 4.0) {
    return 1;  // too short for the extra code to pay for itself
  }

  unsigned factor = 2;
  while (factor < 8 && static_cast<double>(factor) * 2.0 <= tripCount / 8.0) {
    factor *= 2;
  }
  if (tripCount >= 32.0) {
    factor = 8;
  } else if (tripCount >= 8.0) {
    factor = std::max(factor, 4u);
  }

  while (factor > 1 && body * factor > kUnrolledSizeBudget) {
    factor /= 2;
  }
  return factor;
}

/// Replicates a loop body, re-testing the exit condition between copies.
///
/// The result runs the same number of iterations; what disappears is one back
/// edge per copy. The condition is still evaluated every iteration, which is
/// what makes this safe without knowing the trip count exactly -- and the
/// measured trip count is an average, so an exact one is not available.
///
///     header:  p = phi ...;  c = test(p);  condbr c, body, X
///     body:    ...;          c1 = test(p1); condbr c1, body.1, X
///     body.1:  ...;          c2 = test(p2); condbr c2, body.2, X
///     body.2:  ...;                         br header
///     X:       px = phi [p, header], [p1, body], [p2, body.1];  br exit
///
/// `X` is new. Introducing it rather than letting every copy branch straight to
/// the original exit is what keeps the SSA repair in one place: the exit block
/// keeps its single predecessor, and the values that escape the loop are merged
/// exactly once.
class Unroller {
public:
  Unroller(ir::Function& function, SimpleLoop& loop) : function_(function), loop_(loop) {}

  void run(unsigned factor) {
    ir::BasicBlock* exitMerge = function_.createBlock("unroll.exit");
    exitMerge->executionCount = loop_.exit->executionCount;

    // Values of each header phi as of the end of each copy. Index 0 is the phi
    // itself -- its value at the top of the original body.
    std::vector<ValueMap> phiValues;
    ValueMap identity;
    for (ir::Instruction* phi : loop_.phis) {
      identity[phi] = phi;
    }
    phiValues.push_back(identity);

    std::vector<ir::BasicBlock*> copies{loop_.body};

    for (unsigned copy = 1; copy < factor; ++copy) {
      // What the phis hold at the top of this copy: the previous copy's
      // next-iteration values.
      ValueMap incoming;
      for (ir::Instruction* phi : loop_.phis) {
        incoming[phi] = translate(phiValues.back(), latchValue(*phi, loop_.body));
      }

      // Re-test at the end of the previous copy, so it can leave early.
      ir::BasicBlock* previous = copies.back();
      ValueMap testMap = incoming;
      ir::Value* condition = nullptr;
      for (const auto& instruction : loop_.header->instructions()) {
        if (instruction->opcode() == ir::Opcode::Phi || instruction->isTerminator()) {
          continue;
        }
        ir::Instruction* cloned = previous->insertBeforeTerminator(
            cloneInstruction(*instruction, testMap, function_));
        testMap[instruction.get()] = cloned;
      }
      condition = translate(testMap, loop_.header->terminator()->operand(0));

      // The copy itself.
      ir::BasicBlock* next = function_.createBlock("unroll.body");
      // Every copy runs about as often as the others, and the second annotation
      // pass will not find these blocks in the profile, so what they inherit
      // here is what the backend sees. Keeping the original count rather than
      // dividing it keeps all the copies equally hot relative to everything
      // else, which is the only thing layout and spill weighting compare.
      next->executionCount = loop_.body->executionCount;
      ValueMap bodyMap = incoming;
      for (const auto& instruction : loop_.body->instructions()) {
        if (instruction->isTerminator()) {
          continue;
        }
        ir::Instruction* cloned =
            next->append(cloneInstruction(*instruction, bodyMap, function_));
        bodyMap[instruction.get()] = cloned;
      }

      // Previous copy now chooses between carrying on and leaving.
      //
      // Only copy 0 -- the original body -- arrives here with a terminator. The
      // copies made on earlier rounds were built body-first and are still open,
      // and asking an open block for its terminator gets null.
      if (ir::Instruction* previousBranch = previous->terminator()) {
        previousBranch->eraseFromParent();
      }
      auto branch =
          std::make_unique<ir::Instruction>(ir::Opcode::CondBr, ir::Type::getVoid());
      branch->addOperand(condition);
      branch->addSuccessor(loop_.exitOnFalse ? next : exitMerge);
      branch->addSuccessor(loop_.exitOnFalse ? exitMerge : next);
      previous->append(std::move(branch));

      copies.push_back(next);
      phiValues.push_back(std::move(bodyMap));
    }

    // The last copy closes the loop.
    ir::BasicBlock* last = copies.back();
    if (last != loop_.body) {
      auto backEdge =
          std::make_unique<ir::Instruction>(ir::Opcode::Br, ir::Type::getVoid());
      backEdge->addSuccessor(loop_.header);
      last->append(std::move(backEdge));
    }

    // The header now leaves through the merge block rather than straight out.
    ir::Instruction* headerBranch = loop_.header->terminator();
    for (std::size_t i = 0; i < headerBranch->successors().size(); ++i) {
      if (headerBranch->successors()[i] == loop_.exit) {
        headerBranch->setSuccessor(i, exitMerge);
      }
    }

    // Merge what escapes: one phi per header phi, taking the value as of
    // whichever copy the loop left from.
    std::vector<std::pair<ir::Instruction*, ir::Instruction*>> escaping;
    for (ir::Instruction* phi : loop_.phis) {
      auto merged = std::make_unique<ir::Instruction>(ir::Opcode::Phi, phi->type());
      merged->setName(function_.nextTempName());
      merged->addIncoming(phi, loop_.header);
      for (unsigned copy = 0; copy + 1 < factor; ++copy) {
        merged->addIncoming(translate(phiValues[copy], latchValue(*phi, loop_.body)),
                            copies[copy]);
      }
      escaping.push_back({phi, exitMerge->insertAtTop(std::move(merged))});
    }

    auto toExit = std::make_unique<ir::Instruction>(ir::Opcode::Br, ir::Type::getVoid());
    toExit->addSuccessor(loop_.exit);
    exitMerge->append(std::move(toExit));

    // The header's phis come round from the last copy now, not the original body.
    for (ir::Instruction* phi : loop_.phis) {
      ir::Value* carried = translate(phiValues.back(), latchValue(*phi, loop_.body));
      for (std::size_t i = 0; i < phi->incomingCount(); ++i) {
        if (phi->incomingBlock(i) == loop_.body) {
          phi->setOperand(i, carried);
          phi->setSuccessor(i, last);
        }
      }
    }

    // Anything reading a header phi from outside the loop must read the merge
    // instead. Done last, so the rewiring above is not caught by it.
    for (const auto& [phi, merged] : escaping) {
      const std::vector<ir::Instruction*> users = phi->users();
      for (ir::Instruction* user : users) {
        if (user == merged || user->parent() == loop_.header ||
            user->parent() == exitMerge) {
          continue;
        }
        bool insideLoop = user->parent() == loop_.body;
        for (ir::BasicBlock* copy : copies) {
          insideLoop = insideLoop || user->parent() == copy;
        }
        if (insideLoop) {
          continue;
        }
        for (std::size_t i = 0; i < user->operandCount(); ++i) {
          if (user->operand(i) == phi) {
            user->setOperand(i, merged);
          }
        }
      }
    }

    function_.recomputePredecessors();
  }

private:
  ir::Function& function_;
  SimpleLoop& loop_;
};

/// Unrolls hot loops by their measured trip count (PGO-07).
///
/// **Without a profile this pass does nothing at all.** That is the documented
/// fallback and it is deliberate: Phase 7 deferred static unrolling precisely
/// because a static guess at "is this hot" and "how many iterations" is worth
/// less than the code size it costs. With a profile both are measured, which is
/// the clearest illustration of what profile guidance actually buys.
class LoopUnroll final : public Pass {
public:
  std::string_view name() const override { return "loop-unroll"; }
  std::string_view description() const override {
    return "replicate hot loop bodies using measured trip counts";
  }

  bool run(ir::Function& function, analysis::AnalysisManager& manager) override {
    // `--disable-pgo=unroll` withholds the profile, and this pass does nothing
    // at all without one -- so disabling it removes unrolling entirely rather
    // than falling back to a static guess (G-05, G-07).
    const profile::ProfileData* profile =
        pgo().unrolling ? manager.getCached<analysis::ProfileAnalysis>(function)
                        : nullptr;
    if (profile == nullptr || !profile->isValid()) {
      return false;  // nothing measured, nothing to unroll on
    }

    const analysis::LoopInfo& loops = manager.get<analysis::LoopAnalysis>(function);
    if (loops.empty()) {
      return false;
    }

    // Innermost first, and one loop per run: unrolling rewrites the CFG, so the
    // loop information the rest of this list came from is stale afterwards. The
    // pipeline sweeps to a fixed point, so the next loop is reached on the next
    // sweep.
    std::vector<const analysis::Loop*> ordered;
    for (const auto& loop : loops.allLoops()) {
      ordered.push_back(loop.get());
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const analysis::Loop* a, const analysis::Loop* b) {
                return a->depth() > b->depth();
              });

    for (const analysis::Loop* loop : ordered) {
      const std::string header = loop->header()->label();
      if (unrolled_.count(function.name() + " " + header) != 0) {
        continue;  // already done on an earlier sweep
      }
      if (profile->blockHeat(function.name(), header) != profile::Heat::Hot) {
        remark(function, header, "not hot; left alone");
        continue;
      }

      const profile::LoopProfile* measured = profile->loop(function.name(), header);
      if (measured == nullptr) {
        remark(function, header, "hot, but the profile has no trip count for it");
        continue;
      }

      SimpleLoop simple;
      if (!describe(*loop, simple)) {
        remark(function, header, "hot, but its shape is not one this unroller handles");
        continue;
      }

      if (simple.phis.size() > kCarriedValueBudget) {
        remark(function, header,
               "hot, but it carries " + std::to_string(simple.phis.size()) +
                   " values across the back edge; unrolling would keep all of them "
                   "live through every copy");
        continue;
      }

      const unsigned factor = factorFor(measured->tripCount(), bodySize(simple));
      if (factor < 2) {
        remark(function, header, "trip count " + trimmed(measured->tripCount()) +
                                     " is too low to be worth unrolling");
        continue;
      }

      remark(function, header, "trip count " + trimmed(measured->tripCount()) +
                                   " measured; unrolling by " + std::to_string(factor));
      Unroller(function, simple).run(factor);
      unrolled_.insert(function.name() + " " + header);
      return true;
    }

    return false;
  }

private:
  static std::string trimmed(double value) {
    std::string text = std::to_string(value);
    while (text.size() > 1 && text.back() == '0') {
      text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
      text.pop_back();
    }
    return text;
  }

  void remark(const ir::Function& function, const std::string& header,
              const std::string& what) {
    if (remarks() != nullptr) {
      *remarks() << "loop-unroll: @" << function.name() << ":" << header << ": " << what
                 << "\n";
    }
  }

  std::set<std::string> unrolled_;
};

std::unique_ptr<Pass> makeLoopUnroll() { return std::make_unique<LoopUnroll>(); }
const PassRegistration kUnroll{"loop-unroll", makeLoopUnroll};

}  // namespace

void anchorLoopUnroll() {}

}  // namespace optiforge::transforms
