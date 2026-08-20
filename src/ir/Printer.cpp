#include "optiforge/ir/Printer.h"

#include <charconv>
#include <ostream>
#include <sstream>
#include <string>

#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Instruction.h"
#include "optiforge/ir/Module.h"

namespace optiforge::ir {

namespace {

std::string formatDouble(double value) {
  char buffer[64];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (result.ec != std::errc{}) {
    return "<unprintable>";
  }
  return std::string(buffer, result.ptr);
}

std::string toHex(std::uint64_t value) {
  static const char* kDigits = "0123456789abcdef";
  std::string out = "0x";
  bool leading = true;
  for (int shift = 60; shift >= 0; shift -= 4) {
    const char digit = kDigits[(value >> shift) & 0xf];
    if (leading && digit == '0' && shift != 0) {
      continue;
    }
    leading = false;
    out += digit;
  }
  return out;
}

/// How a value appears as an operand: a literal for constants, "%name"
/// otherwise. Never an address -- printed output must be deterministic so
/// golden tests mean something (NFR-06).
std::string operandText(const Value* value) {
  if (value == nullptr) {
    return "<null>";
  }
  switch (value->valueKind()) {
    case Value::Kind::ConstantInt:
      return std::to_string(static_cast<const ConstantInt*>(value)->value());
    case Value::Kind::ConstantFloat:
      return formatDouble(static_cast<const ConstantFloat*>(value)->value());
    case Value::Kind::ConstantBool:
      return static_cast<const ConstantBool*>(value)->value() ? "true" : "false";
    default:
      break;
  }
  return value->hasName() ? "%" + value->name() : "%<unnamed>";
}

std::string typedOperand(const Value* value) {
  if (value == nullptr) {
    return "<null>";
  }
  return std::string(value->type()->name()) + " " + operandText(value);
}

void printInstruction(const Instruction& instruction, std::ostream& out) {
  out << "  ";

  if (instruction.hasResult()) {
    out << "%" << instruction.name() << " = ";
  }

  const Opcode opcode = instruction.opcode();
  out << toString(opcode);

  switch (opcode) {
    case Opcode::Alloca:
      out << " " << instruction.allocatedType()->name();
      if (instruction.allocatedCount() != 1) {
        // Printed only for arrays, so every existing dump is byte-identical.
        out << " x " << instruction.allocatedCount();
      }
      break;

    case Opcode::Gep:
      // Element type, then base and index. Printing the type alone would leave
      // the index looking like a dead value in the dump.
      out << " " << instruction.allocatedType()->name() << ", "
          << operandText(instruction.operand(0)) << ", "
          << typedOperand(instruction.operand(1));
      break;

    case Opcode::ProfInc:
      // The index is the whole content of the instruction, so --emit=ir is
      // useless for debugging instrumentation without it.
      out << " #" << instruction.counterIndex();
      break;

    case Opcode::Load:
      out << " " << instruction.type()->name() << ", " << operandText(instruction.operand(0));
      break;

    case Opcode::Store:
      out << " " << typedOperand(instruction.operand(0)) << ", "
          << operandText(instruction.operand(1));
      break;

    case Opcode::ICmp:
    case Opcode::FCmp:
      out << " " << toString(instruction.predicate()) << " "
          << instruction.operand(0)->type()->name() << " "
          << operandText(instruction.operand(0)) << ", "
          << operandText(instruction.operand(1));
      break;

    case Opcode::Br:
      out << " " << instruction.successors()[0]->label();
      break;

    case Opcode::CondBr:
      out << " " << operandText(instruction.operand(0)) << ", "
          << instruction.successors()[0]->label() << ", "
          << instruction.successors()[1]->label();
      break;

    case Opcode::Ret:
      if (instruction.operandCount() == 1) {
        out << " " << typedOperand(instruction.operand(0));
      }
      break;

    case Opcode::Phi: {
      // Show which predecessor each value arrives from; without it a phi is
      // unreadable and its incoming edges cannot be checked by eye.
      out << " " << instruction.type()->name();
      for (std::size_t i = 0; i < instruction.operandCount(); ++i) {
        out << (i == 0 ? " " : ", ") << "[ " << operandText(instruction.operand(i))
            << ", " << instruction.incomingBlock(i)->label() << " ]";
      }
      break;
    }

    case Opcode::Call: {
      out << " " << instruction.type()->name() << " @" << instruction.callee()->name() << "(";
      for (std::size_t i = 0; i < instruction.operandCount(); ++i) {
        if (i != 0) {
          out << ", ";
        }
        out << typedOperand(instruction.operand(i));
      }
      out << ")";
      break;
    }

    default: {
      // Arithmetic, logic and conversions: "<type> <lhs>, <rhs>".
      const Value* first = instruction.operandCount() > 0 ? instruction.operand(0) : nullptr;
      if (first != nullptr) {
        out << " " << first->type()->name();
        for (std::size_t i = 0; i < instruction.operandCount(); ++i) {
          out << (i == 0 ? " " : ", ") << operandText(instruction.operand(i));
        }
      }
      break;
    }
  }

  out << '\n';
}

void printFunction(const Function& function, std::ostream& out) {
  out << "fn @" << function.name() << "(";
  for (std::size_t i = 0; i < function.arguments().size(); ++i) {
    if (i != 0) {
      out << ", ";
    }
    const Argument& argument = *function.arguments()[i];
    out << argument.type()->name() << " %" << argument.name();
  }
  out << ") -> " << function.returnType()->name();

  if (function.isDeclaration()) {
    out << ";\n";
    return;
  }

  out << " {\n";

  for (std::size_t b = 0; b < function.blocks().size(); ++b) {
    const BasicBlock& block = *function.blocks()[b];
    if (b != 0) {
      out << '\n';
    }

    std::ostringstream header;
    header << block.label() << ':';

    if (!block.predecessors().empty()) {
      // Pad so the predecessor comments line up, which makes a diff of two IR
      // dumps far easier to read.
      std::string text = header.str();
      out << text;
      for (std::size_t pad = text.size(); pad < 48; ++pad) {
        out << ' ';
      }
      out << "; preds = ";
      for (std::size_t i = 0; i < block.predecessors().size(); ++i) {
        out << (i == 0 ? "" : ", ") << block.predecessors()[i]->label();
      }
      out << '\n';
    } else {
      out << header.str() << '\n';
    }

    for (const auto& instruction : block.instructions()) {
      printInstruction(*instruction, out);
    }
  }

  out << "}\n";
}

}  // namespace

void printModule(const Module& module, std::ostream& out) {
  out << "module \"" << module.sourceName() << "\" hash=" << toHex(module.sourceHash())
      << "\n";

  for (const auto& function : module.functions()) {
    out << '\n';
    printFunction(*function, out);
  }
}

void printCFG(const Module& module, std::ostream& out) {
  out << "digraph cfg {\n";
  out << "  graph [fontname=\"monospace\"];\n";
  out << "  node  [shape=box, fontname=\"monospace\"];\n";
  out << "  edge  [fontname=\"monospace\"];\n";

  for (const auto& function : module.functions()) {
    if (function->isDeclaration()) {
      continue;
    }

    out << "\n  subgraph cluster_" << function->name() << " {\n";
    out << "    label=\"@" << function->name() << "\";\n";

    for (const auto& block : function->blocks()) {
      out << "    \"" << function->name() << ":" << block->label() << "\" [label=\""
          << block->label() << "\"];\n";
    }

    for (const auto& block : function->blocks()) {
      const Instruction* terminator = block->terminator();
      if (terminator == nullptr) {
        continue;
      }
      const auto& successors = terminator->successors();
      for (std::size_t i = 0; i < successors.size(); ++i) {
        out << "    \"" << function->name() << ":" << block->label() << "\" -> \""
            << function->name() << ":" << successors[i]->label() << "\"";
        // Label the two edges of a conditional so the picture is readable.
        if (terminator->opcode() == Opcode::CondBr) {
          out << " [label=\"" << (i == 0 ? "true" : "false") << "\"]";
        }
        out << ";\n";
      }
    }

    out << "  }\n";
  }

  out << "}\n";
}

}  // namespace optiforge::ir
