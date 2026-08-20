#include <cstring>
#include <ostream>
#include <string>

#include "optiforge/backend/CodeGen.h"
#include "optiforge/backend/MachineIR.h"

namespace optiforge::backend {

namespace {

std::string hex64(std::uint64_t value) {
  static const char* kDigits = "0123456789abcdef";
  std::string out = "0x";
  for (int shift = 60; shift >= 0; shift -= 4) {
    out += kDigits[(value >> shift) & 0xf];
  }
  return out;
}

std::uint64_t bitsOf(double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

std::string renderOperand(const MOperand& operand) {
  switch (operand.kind) {
    case MOperand::Kind::Reg:
      return std::string(regName(operand.reg));

    case MOperand::Kind::Imm:
      return "$" + std::to_string(operand.imm);

    case MOperand::Kind::Mem: {
      std::string out;
      if (operand.disp != 0) {
        out += std::to_string(operand.disp);
      }
      out += "(" + std::string(regName(operand.base)) + ")";
      return out;
    }

    case MOperand::Kind::Label:
      return operand.label;

    case MOperand::Kind::RipLabel:
      return operand.label + "(%rip)";
  }
  return "<?>";
}

/// Which operands of an instruction must be spelled as 8-bit registers.
///
/// `setcc` writes a single byte, so its only operand is 8-bit. `movzbq` widens
/// a byte into a quadword, so its *source* is 8-bit while its destination is
/// not -- naming both the same way is an operand-size mismatch the assembler
/// rejects.
bool operandIsByteSized(const char* mnemonic, std::size_t index) {
  if (std::strncmp(mnemonic, "set", 3) == 0) {
    return true;
  }
  if (std::strncmp(mnemonic, "movzb", 5) == 0) {
    return index == 0;
  }
  // The byte-wise and/or that combine a setcc with its parity guard.
  if (std::strcmp(mnemonic, "andb") == 0 || std::strcmp(mnemonic, "orb") == 0) {
    return true;
  }
  // A variable shift names its count register as %cl, never %rcx.
  if (std::strcmp(mnemonic, "shlq") == 0 || std::strcmp(mnemonic, "sarq") == 0) {
    return index == 0;
  }
  return false;
}

void printInstruction(const MInstr& instruction, std::ostream& out) {
  if (instruction.isLabel) {
    out << instruction.comment << ":\n";
    return;
  }

  std::string text = "\t";
  text += instruction.mnemonic;

  if (!instruction.operands.empty()) {
    // Pad so operand columns line up, which makes the output readable next to
    // objdump.
    while (text.size() < 12) {
      text += ' ';
    }
    for (std::size_t i = 0; i < instruction.operands.size(); ++i) {
      if (i != 0) {
        text += ", ";
      }
      const MOperand& operand = instruction.operands[i];
      if (operandIsByteSized(instruction.mnemonic, i) &&
          operand.kind == MOperand::Kind::Reg) {
        text += regName8(operand.reg);
      } else {
        text += renderOperand(operand);
      }
    }
  }

  out << text;
  if (!instruction.comment.empty()) {
    while (text.size() < 44) {
      text += ' ';
      out << ' ';
    }
    out << "# " << instruction.comment;
  }
  out << '\n';
}

/// Escapes a string for `.asciz`. Paths on this platform contain backslashes,
/// which the assembler would otherwise read as escape sequences.
std::string quoted(const std::string& text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '\\' || c == '"') {
      out += '\\';
    }
    out += c;
  }
  out += '"';
  return out;
}

/// Emits the profile counter array, the table that interprets it, and the
/// header fields that let a later build detect a stale profile.
///
/// The counters live in `.bss` because they start at zero and there are as many
/// of them as there are basic blocks; everything else is read-only. Every symbol
/// here is one `libofprof` declares `extern`, so a rename on either side is a
/// link error rather than a silent misread.
void printProfileData(const ProfileLayout& profile, std::ostream& out) {
  if (!profile.enabled) {
    return;
  }

  out << "\n# --- Profile instrumentation (Phase 9) ---\n";
  out << "\t.bss\n";
  out << "\t.align\t8\n";
  out << "\t.globl\t__ofprof_counters\n";
  out << "__ofprof_counters:\n";
  out << "\t.zero\t" << (static_cast<std::uint64_t>(profile.counterCount) * 8)
      << "\t# " << profile.counterCount << " basic blocks\n";

  {
    // Accumulated ticks, the re-entry depth that stops a recursive function
    // counting its own nested calls twice, and where the outermost call began.
    //
    // Emitted even when timing is off, with one unused slot. PE/COFF has no
    // usable weak undefined symbol, so the runtime cannot simply declare these
    // and check whether they resolved -- it would be a link error. Twenty-four
    // bytes is the price of one arrangement that works in both builds; the
    // runtime's guard is __ofprof_num_times, which is zero when timing is off.
    const std::size_t slots =
        profile.timedFunctions.empty() ? 1 : profile.timedFunctions.size();
    for (const char* symbol :
         {"__ofprof_times", "__ofprof_time_depth", "__ofprof_time_start"}) {
      out << "\t.align\t8\n";
      out << "\t.globl\t" << symbol << "\n";
      out << symbol << ":\n";
      out << "\t.zero\t" << (slots * 8) << "\n";
    }
  }

  out << "\n\t.section\t.rdata,\"dr\"\n";
  out << "\t.align\t8\n";
  out << "\t.globl\t__ofprof_num_counters\n";
  out << "__ofprof_num_counters:\t.quad\t" << profile.counterCount << "\n";
  out << "\t.globl\t__ofprof_num_times\n";
  out << "__ofprof_num_times:\t.quad\t" << profile.timedFunctions.size() << "\n";
  out << "\t.globl\t__ofprof_num_records\n";
  out << "__ofprof_num_records:\t.quad\t" << profile.records.size() << "\n";
  out << "\t.globl\t__ofprof_src_hash\n";
  out << "__ofprof_src_hash:\t.quad\t" << hex64(profile.sourceHash) << "\n";
  out << "\t.globl\t__ofprof_opt_level\n";
  out << "__ofprof_opt_level:\t.quad\t" << profile.optLevel << "\n";

  out << "\t.globl\t__ofprof_source\n";
  out << "__ofprof_source:\t.asciz\t" << quoted(profile.sourceName) << "\n";
  out << "\t.globl\t__ofprof_compiler\n";
  out << "__ofprof_compiler:\t.asciz\t" << quoted(profile.compiler) << "\n";
  out << "\t.globl\t__ofprof_default_path\n";
  out << "__ofprof_default_path:\t.asciz\t" << quoted(profile.defaultOutputPath) << "\n";

  // One record per line the runtime will print: the kind, the one or two
  // counters it reads, and the offset of the text between keyword and numbers.
  out << "\n\t.align\t8\n";
  out << "\t.globl\t__ofprof_records\n";
  out << "__ofprof_records:\n";
  for (std::size_t i = 0; i < profile.records.size(); ++i) {
    const ProfileRecord& record = profile.records[i];
    out << "\t.quad\t" << static_cast<std::uint64_t>(record.kind) << ", "
        << record.counterA << ", " << record.counterB << ", .LPN" << i
        << " - __ofprof_names\t# " << record.name << "\n";
  }

  out << "\n\t.globl\t__ofprof_names\n";
  out << "__ofprof_names:\n";
  for (std::size_t i = 0; i < profile.records.size(); ++i) {
    out << ".LPN" << i << ":\t.asciz\t" << quoted(profile.records[i].name) << "\n";
  }
  if (profile.records.empty()) {
    out << "\t.byte\t0\n";  // never indexed, but the symbol has to exist
  }

  // Counter names as comments only. Nothing reads them; they are here so the
  // array can be checked by hand against the .prof, which is exactly what
  // Phase 9's exit criterion asks for.
  out << "\n# counter index -> function:block\n";
  for (std::size_t i = 0; i < profile.counterNames.size(); ++i) {
    out << "#   " << i << "\t" << profile.counterNames[i] << "\n";
  }
}

}  // namespace

void printAssembly(const MModule& module, std::ostream& out) {
  out << "# Generated by OptiForge from \"" << module.sourceName << "\"\n";
  out << "# Target: x86-64 Windows (Microsoft x64 ABI)\n";
  out << "\t.text\n";

  for (const MFunction& function : module.functions) {
    out << '\n';
    out << "\t.globl\t" << function.name << '\n';
    // COFF symbol metadata; harmless if the linker does not need it.
    out << "\t.def\t" << function.name << ";\t.scl\t2;\t.type\t32;\t.endef\n";
    out << function.name << ":\n";

    for (const MBasicBlock& block : function.blocks) {
      out << block.label << ":\n";
      for (const MInstr& instruction : block.instructions) {
        printInstruction(instruction, out);
      }
    }
  }

  if (!module.floatConstants.empty() || module.needsNegateMask) {
    out << "\n\t.section\t.rdata,\"dr\"\n";

    if (module.needsNegateMask) {
      // xorpd reads 16 bytes and requires 16-byte alignment.
      out << "\t.align\t16\n";
      out << ".LCnegmask:\n";
      out << "\t.quad\t" << hex64(0x8000000000000000ULL) << "\n";
      out << "\t.quad\t0\n";
    }

    for (const FloatConstant& constant : module.floatConstants) {
      out << "\t.align\t8\n";
      out << constant.label << ":\n";
      // Emit the exact bit pattern: a decimal rendering would depend on the
      // assembler's float parsing and could round.
      out << "\t.quad\t" << hex64(bitsOf(constant.value)) << "\t# " << constant.value
          << "\n";
    }
  }

  printProfileData(module.profile, out);
}

}  // namespace optiforge::backend
