#include "optiforge/driver/Options.h"

#include <ostream>

#include "optiforge/support/Version.h"

namespace optiforge {

std::string_view toString(EmitStage stage) {
  switch (stage) {
    case EmitStage::Executable:
      return "executable";
    case EmitStage::Tokens:
      return "tokens";
    case EmitStage::Ast:
      return "ast";
    case EmitStage::Ir:
      return "ir";
    case EmitStage::Cfg:
      return "cfg";
    case EmitStage::Analysis:
      return "analysis";
    case EmitStage::Asm:
      return "asm";
    case EmitStage::Obj:
      return "obj";
  }
  return "unknown";
}

bool parseEmitStage(std::string_view name, EmitStage& out) {
  if (name == "tokens") {
    out = EmitStage::Tokens;
  } else if (name == "ast") {
    out = EmitStage::Ast;
  } else if (name == "ir") {
    out = EmitStage::Ir;
  } else if (name == "cfg") {
    out = EmitStage::Cfg;
  } else if (name == "analysis") {
    out = EmitStage::Analysis;
  } else if (name == "asm") {
    out = EmitStage::Asm;
  } else if (name == "obj") {
    out = EmitStage::Obj;
  } else {
    return false;
  }
  return true;
}

namespace {

constexpr std::string_view kEmitPrefix = "--emit=";

bool startsWith(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

}  // namespace

bool parseOptions(int argc, const char* const* argv, Options& out, std::ostream& err) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];

    if (arg == "--help" || arg == "-h") {
      out.showHelp = true;
      return true;  // Nothing after --help can change the outcome.
    }

    if (arg == "--version") {
      out.showVersion = true;
      return true;
    }

    if (arg == "-o") {
      if (i + 1 >= argc) {
        err << "optiforge: error: '-o' requires an argument\n";
        return false;
      }
      out.outputPath = argv[++i];
      continue;
    }

    if (arg == "-O0" || arg == "-O1" || arg == "-O2") {
      out.optLevel = arg[2] - '0';
      continue;
    }

    if (startsWith(arg, kEmitPrefix)) {
      const std::string_view name = arg.substr(kEmitPrefix.size());
      if (!parseEmitStage(name, out.emit)) {
        err << "optiforge: error: unknown stage '" << name << "' for --emit\n"
            << "  expected one of: tokens, ast, ir, cfg, analysis, asm, obj\n";
        return false;
      }
      continue;
    }

    if (startsWith(arg, "--runtime-dir=")) {
      out.runtimeDir = std::string(arg.substr(std::string_view("--runtime-dir=").size()));
      continue;
    }

    if (arg == "--print-after-all") {
      out.printAfterAll = true;
      continue;
    }

    if (startsWith(arg, "--print-after=")) {
      out.printAfter = std::string(arg.substr(std::string_view("--print-after=").size()));
      continue;
    }

    if (arg == "--verify-each") {
      out.verifyEach = true;
      continue;
    }

    if (startsWith(arg, "--disable-pass=")) {
      out.disabledPasses.emplace_back(
          arg.substr(std::string_view("--disable-pass=").size()));
      continue;
    }

    if (arg == "--keep-temps") {
      out.keepTemps = true;
      continue;
    }

    if (arg == "-Werror") {
      out.warningsAsErrors = true;
      continue;
    }

    if (startsWith(arg, "-")) {
      // A lone "-" is not a valid input path either, so this covers it.
      err << "optiforge: error: unknown option '" << arg << "'\n"
          << "  try 'optiforge --help'\n";
      return false;
    }

    if (!out.inputPath.empty()) {
      err << "optiforge: error: more than one input file given ('" << out.inputPath << "' and '"
          << arg << "')\n";
      return false;
    }
    out.inputPath = arg;
  }

  return true;
}

void printVersion(std::ostream& out) {
  out << "optiforge " << OPTIFORGE_VERSION_STRING << '\n';
}

void printHelp(std::ostream& out) {
  out << "OptiForge " << OPTIFORGE_VERSION_STRING
      << " - a profile-guided optimizing compiler\n"
         "\n"
         "usage: optiforge <input.of> [options]\n"
         "\n"
         "Output:\n"
         "  -o <file>            Write output to <file>\n"
         "  --emit=<stage>       Stop after <stage> and dump its result\n"
         "                       (tokens, ast, ir, cfg, asm, obj)\n"
         "\n"
         "Optimization:\n"
         "  -O0                  No optimization (default)\n"
         "  -O1                  Basic optimization\n"
         "  -O2                  Full optimization\n"
         "\n"
         "Diagnostics:\n"
         "  -Werror              Treat warnings as errors\n"
         "\n"
         "General:\n"
         "  -h, --help           Show this message\n"
         "      --version        Show the version\n";
}

}  // namespace optiforge
