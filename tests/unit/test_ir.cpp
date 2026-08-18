#include <memory>
#include <sstream>
#include <string>

#include "TestHarness.h"
#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/IRBuilder.h"
#include "optiforge/ir/Instruction.h"
#include "optiforge/ir/Module.h"
#include "optiforge/ir/Printer.h"
#include "optiforge/ir/Type.h"
#include "optiforge/ir/Verifier.h"

using namespace optiforge;
using namespace optiforge::ir;

namespace {

/// A module with one function whose entry block is open for insertion.
struct Fixture {
  Module module{"t.of"};
  Function* fn;
  BasicBlock* entry;
  IRBuilder builder{module};

  explicit Fixture(const Type* returnType = Type::getI64())
      : fn(module.createFunction("f", returnType)) {
    entry = fn->createBlock("entry");
    builder.setInsertPoint(entry);
  }

  std::string text() {
    std::ostringstream out;
    printModule(module, out);
    return out.str();
  }

  bool verifies() {
    Verifier verifier;
    return verifier.verify(module);
  }

  std::string verifierErrors() {
    Verifier verifier;
    verifier.verify(module);
    std::ostringstream out;
    verifier.printErrors(out);
    return out.str();
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

TEST("IR types are interned singletons") {
  CHECK_EQ(Type::getI64(), Type::getI64());
  CHECK(Type::getI64() != Type::getF64());
  CHECK_EQ(std::string(Type::getI1()->name()), std::string("i1"));
  CHECK_EQ(std::string(Type::getPtr()->name()), std::string("ptr"));
}

TEST("IR type sizes match the frontend's model") {
  CHECK_EQ(Type::getI64()->sizeInBytes(), 8u);
  CHECK_EQ(Type::getF64()->sizeInBytes(), 8u);
  CHECK_EQ(Type::getI1()->sizeInBytes(), 1u);
  CHECK_EQ(Type::getVoid()->sizeInBytes(), 0u);
}

// ---------------------------------------------------------------------------
// Constant interning
// ---------------------------------------------------------------------------

TEST("identical constants are shared") {
  Module module{"t.of"};
  CHECK_EQ(module.getInt(42), module.getInt(42));
  CHECK(module.getInt(42) != module.getInt(43));
  CHECK_EQ(module.getBool(true), module.getBool(true));
  CHECK(module.getBool(true) != module.getBool(false));
  CHECK_EQ(module.getFloat(1.5), module.getFloat(1.5));
}

TEST("float constants are keyed by bit pattern, so -0.0 stays distinct") {
  Module module{"t.of"};
  CHECK(module.getFloat(0.0) != module.getFloat(-0.0));
}

// ---------------------------------------------------------------------------
// Use lists and replaceAllUsesWith
// ---------------------------------------------------------------------------

TEST("operands record their users") {
  Fixture fx;
  Value* a = fx.module.getInt(1);
  Value* slot = fx.builder.createEntryAlloca(Type::getI64(), "x");
  fx.builder.createStore(a, slot);

  CHECK_EQ(a->useCount(), std::size_t{1});
  CHECK_EQ(slot->useCount(), std::size_t{1});
}

TEST("an instruction using the same value twice counts twice") {
  Fixture fx;
  Value* slot = fx.builder.createEntryAlloca(Type::getI64(), "x");
  Value* loaded = fx.builder.createLoad(slot, Type::getI64());
  fx.builder.createBinary(Opcode::Add, loaded, loaded);

  CHECK_EQ(loaded->useCount(), std::size_t{2});
}

TEST("replaceAllUsesWith repoints every operand slot") {
  Fixture fx;
  Value* slot = fx.builder.createEntryAlloca(Type::getI64(), "x");
  Value* first = fx.builder.createLoad(slot, Type::getI64());
  Value* second = fx.builder.createLoad(slot, Type::getI64());
  Value* sum = fx.builder.createBinary(Opcode::Add, first, first);
  fx.builder.createRet(sum);

  CHECK_EQ(first->useCount(), std::size_t{2});
  first->replaceAllUsesWith(second);

  CHECK_EQ(first->useCount(), std::size_t{0});
  CHECK_EQ(second->useCount(), std::size_t{2});
  CHECK(fx.verifies());
}

TEST("replaceAllUsesWith with itself is a no-op") {
  Fixture fx;
  Value* slot = fx.builder.createEntryAlloca(Type::getI64(), "x");
  Value* loaded = fx.builder.createLoad(slot, Type::getI64());
  fx.builder.createRet(loaded);

  loaded->replaceAllUsesWith(loaded);
  CHECK_EQ(loaded->useCount(), std::size_t{1});
}

// ---------------------------------------------------------------------------
// Constant folding in the builder
// ---------------------------------------------------------------------------

TEST("constant arithmetic is folded at creation") {
  Fixture fx;
  Value* folded = fx.builder.createBinary(Opcode::Add, fx.module.getInt(2), fx.module.getInt(3));
  CHECK(folded->isConstant());
  CHECK_EQ(static_cast<ConstantInt*>(folded)->value(), std::int64_t{5});
  // Nothing was emitted.
  CHECK_EQ(fx.entry->size(), std::size_t{0});
}

TEST("division by zero is left for the target rather than folded") {
  Fixture fx;
  Value* result =
      fx.builder.createBinary(Opcode::SDiv, fx.module.getInt(1), fx.module.getInt(0));
  CHECK(!result->isConstant());
}

TEST("INT64_MIN divided by -1 is not folded") {
  Fixture fx;
  Value* result = fx.builder.createBinary(Opcode::SDiv, fx.module.getInt(INT64_MIN),
                                          fx.module.getInt(-1));
  CHECK(!result->isConstant());
}

TEST("converting a literal to float is folded") {
  Fixture fx;
  Value* result = fx.builder.createSIToFP(fx.module.getInt(2));
  CHECK(result->isConstant());
  CHECK(result->type()->isF64());
  CHECK_EQ(fx.entry->size(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// CFG
// ---------------------------------------------------------------------------

TEST("successors are derived from the terminator") {
  Fixture fx(Type::getVoid());
  BasicBlock* a = fx.fn->createBlock("a");
  BasicBlock* b = fx.fn->createBlock("b");

  fx.builder.createCondBr(fx.module.getBool(true), a, b);

  const auto successors = fx.entry->successors();
  CHECK_EQ(successors.size(), std::size_t{2});
  CHECK_EQ(successors[0], a);
  CHECK_EQ(successors[1], b);
}

TEST("appending a terminator registers predecessors") {
  Fixture fx(Type::getVoid());
  BasicBlock* target = fx.fn->createBlock("target");
  fx.builder.createBr(target);

  CHECK_EQ(target->predecessors().size(), std::size_t{1});
  CHECK_EQ(target->predecessors()[0], fx.entry);
  CHECK(fx.entry->predecessors().empty());
}

TEST("a block with no terminator reports none") {
  Fixture fx;
  CHECK_EQ(fx.entry->terminator(), nullptr);
  CHECK(!fx.entry->isTerminated());
  fx.builder.createRet(fx.module.getInt(0));
  CHECK(fx.entry->isTerminated());
}

TEST("recomputePredecessors reproduces the incremental lists") {
  Fixture fx(Type::getVoid());
  BasicBlock* a = fx.fn->createBlock("a");
  BasicBlock* b = fx.fn->createBlock("b");
  fx.builder.createCondBr(fx.module.getBool(true), a, b);
  fx.builder.setInsertPoint(a);
  fx.builder.createBr(b);
  fx.builder.setInsertPoint(b);
  fx.builder.createRetVoid();

  const std::size_t beforeA = a->predecessors().size();
  const std::size_t beforeB = b->predecessors().size();
  fx.fn->recomputePredecessors();

  CHECK_EQ(a->predecessors().size(), beforeA);
  CHECK_EQ(b->predecessors().size(), beforeB);
  CHECK_EQ(beforeB, std::size_t{2});
}

// ---------------------------------------------------------------------------
// Deterministic naming (IR-11)
// ---------------------------------------------------------------------------

TEST("block labels are deterministic and entry is unnumbered") {
  Module module{"t.of"};
  Function* fn = module.createFunction("f", Type::getVoid());
  CHECK_EQ(fn->createBlock("entry")->label(), std::string("entry"));
  CHECK_EQ(fn->createBlock("while.cond")->label(), std::string("while.cond.1"));
  CHECK_EQ(fn->createBlock("while.body")->label(), std::string("while.body.2"));
  CHECK_EQ(fn->createBlock("while.end")->label(), std::string("while.end.3"));
}

TEST("block numbering is per function, not global") {
  // Profile records are keyed by function:block, so the counter must restart
  // for each function or a new function would renumber every later one.
  Module module{"t.of"};
  Function* a = module.createFunction("a", Type::getVoid());
  a->createBlock("entry");
  a->createBlock("x");

  Function* b = module.createFunction("b", Type::getVoid());
  b->createBlock("entry");
  CHECK_EQ(b->createBlock("x")->label(), std::string("x.1"));
}

TEST("temporary names are sequential per function") {
  Module module{"t.of"};
  Function* fn = module.createFunction("f", Type::getVoid());
  CHECK_EQ(fn->nextTempName(), std::string("t0"));
  CHECK_EQ(fn->nextTempName(), std::string("t1"));
}

// ---------------------------------------------------------------------------
// Unreachable-block pruning
// ---------------------------------------------------------------------------

TEST("unreachable blocks are pruned") {
  Fixture fx(Type::getVoid());
  BasicBlock* orphan = fx.fn->createBlock("orphan");
  fx.builder.createRetVoid();

  fx.builder.setInsertPoint(orphan);
  fx.builder.createRetVoid();

  CHECK_EQ(fx.fn->blocks().size(), std::size_t{2});
  CHECK_EQ(fx.fn->pruneUnreachableBlocks(), std::size_t{1});
  CHECK_EQ(fx.fn->blocks().size(), std::size_t{1});
  CHECK(fx.verifies());
}

TEST("pruning drops references so survivors keep clean user lists") {
  Fixture fx(Type::getVoid());
  Value* slot = fx.builder.createEntryAlloca(Type::getI64(), "x");
  BasicBlock* orphan = fx.fn->createBlock("orphan");
  fx.builder.createRetVoid();

  // The orphan block uses the alloca, so pruning must remove that use.
  fx.builder.setInsertPoint(orphan);
  fx.builder.createLoad(slot, Type::getI64());
  fx.builder.createRetVoid();

  CHECK_EQ(slot->useCount(), std::size_t{1});
  fx.fn->pruneUnreachableBlocks();
  CHECK_EQ(slot->useCount(), std::size_t{0});
  CHECK(fx.verifies());
}

TEST("pruning a fully reachable function changes nothing") {
  Fixture fx(Type::getVoid());
  fx.builder.createRetVoid();
  CHECK_EQ(fx.fn->pruneUnreachableBlocks(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// Verifier
// ---------------------------------------------------------------------------

TEST("a well-formed function verifies") {
  Fixture fx;
  fx.builder.createRet(fx.module.getInt(0));
  CHECK(fx.verifies());
}

TEST("a block with no terminator is rejected") {
  Fixture fx;
  fx.builder.createEntryAlloca(Type::getI64(), "x");
  CHECK(!fx.verifies());
  CHECK(fx.verifierErrors().find("does not end in a terminator") != std::string::npos);
}

TEST("an empty block is rejected") {
  Fixture fx;
  fx.builder.createRet(fx.module.getInt(0));
  fx.fn->createBlock("dangling");
  CHECK(!fx.verifies());
}

TEST("an instruction after the terminator is rejected") {
  Fixture fx;
  fx.builder.createRet(fx.module.getInt(0));
  // Append past the terminator, which no well-behaved caller would do.
  auto stray = std::make_unique<Instruction>(Opcode::Alloca, Type::getPtr());
  stray->setAllocatedType(Type::getI64());
  stray->setName("stray.addr");
  fx.entry->append(std::move(stray));

  CHECK(!fx.verifies());
  CHECK(fx.verifierErrors().find("is not the last instruction") != std::string::npos);
}

TEST("a return type mismatch is rejected") {
  Fixture fx(Type::getI64());
  fx.builder.createRet(fx.module.getFloat(1.0));
  CHECK(!fx.verifies());
  CHECK(fx.verifierErrors().find("does not match the function return type") !=
        std::string::npos);
}

TEST("a void function returning a value is rejected") {
  Fixture fx(Type::getVoid());
  fx.builder.createRet(fx.module.getInt(0));
  CHECK(!fx.verifies());
  CHECK(fx.verifierErrors().find("void function must not return a value") !=
        std::string::npos);
}

TEST("mismatched arithmetic operand types are rejected") {
  Fixture fx;
  auto bad = std::make_unique<Instruction>(Opcode::Add, Type::getI64());
  bad->setName("bad");
  bad->addOperand(fx.module.getInt(1));
  bad->addOperand(fx.module.getFloat(1.0));
  fx.entry->append(std::move(bad));
  fx.builder.createRet(fx.module.getInt(0));

  CHECK(!fx.verifies());
  CHECK(fx.verifierErrors().find("integer arithmetic requires i64 operands") !=
        std::string::npos);
}

TEST("an alloca outside the entry block is rejected") {
  Fixture fx(Type::getVoid());
  BasicBlock* other = fx.fn->createBlock("other");
  fx.builder.createBr(other);
  fx.builder.setInsertPoint(other);

  // Built by hand: IRBuilder::createEntryAlloca makes this mistake impossible,
  // which is the point of routing every slot through it. The verifier is still
  // the backstop for IR produced by a future pass that bypasses the builder.
  auto late = std::make_unique<Instruction>(Opcode::Alloca, Type::getPtr());
  late->setAllocatedType(Type::getI64());
  late->setName("late.addr");
  other->append(std::move(late));
  fx.builder.createRetVoid();

  CHECK(!fx.verifies());
  CHECK(fx.verifierErrors().find("must be in the entry block") != std::string::npos);
}

TEST("a slot declared inside a loop still lands in the entry block") {
  // Regression: createEntryAlloca used to append, so a declaration reached
  // after the entry block was terminated placed its alloca *after* that
  // block's terminator and produced malformed IR.
  Fixture fx(Type::getVoid());
  BasicBlock* body = fx.fn->createBlock("body");
  fx.builder.createBr(body);

  fx.builder.setInsertPoint(body);
  Value* slot = fx.builder.createEntryAlloca(Type::getI64(), "inner");
  fx.builder.createStore(fx.module.getInt(1), slot);
  fx.builder.createRetVoid();

  CHECK(fx.verifies());
  CHECK_EQ(static_cast<Instruction*>(slot)->parent(), fx.entry);
  // ... and before the terminator, not after it.
  CHECK(fx.entry->terminator() != nullptr);
  CHECK_EQ(fx.entry->instructions().front().get(), slot);
}

TEST("an out-of-sync predecessor list is detected") {
  Fixture fx(Type::getVoid());
  BasicBlock* target = fx.fn->createBlock("target");
  fx.builder.createBr(target);
  fx.builder.setInsertPoint(target);
  fx.builder.createRetVoid();
  CHECK(fx.verifies());

  // Corrupt the bookkeeping the way a buggy pass would.
  target->addPredecessor(target);
  CHECK(!fx.verifies());
  CHECK(fx.verifierErrors().find("out of sync") != std::string::npos);
}

TEST("an unreachable block is reported when not pruned") {
  Fixture fx(Type::getVoid());
  BasicBlock* orphan = fx.fn->createBlock("orphan");
  fx.builder.createRetVoid();
  fx.builder.setInsertPoint(orphan);
  fx.builder.createRetVoid();

  CHECK(!fx.verifies());
  CHECK(fx.verifierErrors().find("unreachable") != std::string::npos);
}

TEST("a condbr on a non-boolean condition is rejected") {
  Fixture fx(Type::getVoid());
  BasicBlock* a = fx.fn->createBlock("a");
  BasicBlock* b = fx.fn->createBlock("b");
  auto bad = std::make_unique<Instruction>(Opcode::CondBr, Type::getVoid());
  bad->addOperand(fx.module.getInt(1));  // i64, not i1
  bad->addSuccessor(a);
  bad->addSuccessor(b);
  fx.entry->append(std::move(bad));

  fx.builder.setInsertPoint(a);
  fx.builder.createRetVoid();
  fx.builder.setInsertPoint(b);
  fx.builder.createRetVoid();

  CHECK(!fx.verifies());
  CHECK(fx.verifierErrors().find("i1 condition") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Printing
// ---------------------------------------------------------------------------

TEST("printed IR is deterministic across two identical builds") {
  const auto build = []() {
    Fixture fx;
    Value* slot = fx.builder.createEntryAlloca(Type::getI64(), "x");
    fx.builder.createStore(fx.module.getInt(1), slot);
    Value* loaded = fx.builder.createLoad(slot, Type::getI64());
    fx.builder.createRet(loaded);
    return fx.text();
  };
  CHECK_EQ(build(), build());
}

TEST("printed IR contains no pointer addresses") {
  Fixture fx;
  Value* slot = fx.builder.createEntryAlloca(Type::getI64(), "x");
  fx.builder.createStore(fx.module.getInt(7), slot);
  fx.builder.createRet(fx.module.getInt(0));

  const std::string text = fx.text();
  // "0x" appears only in the module hash line, never in an operand.
  CHECK_EQ(text.find("0x", text.find('\n')), std::string::npos);
}
