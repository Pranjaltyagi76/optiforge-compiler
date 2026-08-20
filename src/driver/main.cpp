#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "optiforge/driver/Options.h"
#include "optiforge/frontend/AST.h"
#include "optiforge/frontend/ASTPrinter.h"
#include "optiforge/frontend/Lexer.h"
#include "optiforge/frontend/Parser.h"
#include "optiforge/frontend/Sema.h"
#include "optiforge/frontend/Symbol.h"
#include "optiforge/frontend/Token.h"
#include "optiforge/ir/Module.h"
#include "optiforge/ir/Printer.h"
#include "optiforge/ir/Verifier.h"
#include "optiforge/irgen/IRGen.h"
#include "optiforge/analysis/AnalysisManager.h"
#include "optiforge/analysis/ProfileAnalysis.h"
#include "optiforge/analysis/AnalysisPrinter.h"
#include "optiforge/analysis/SSAVerifier.h"
#include "optiforge/passes/Pass.h"
#include "optiforge/transforms/Instrument.h"
#include "optiforge/transforms/ProfileAnnotate.h"
#include "optiforge/transforms/SSA.h"
#include "optiforge/backend/CodeGen.h"
#include "optiforge/backend/RegAlloc.h"
#include "optiforge/backend/TargetInfo.h"
#include "optiforge/driver/Toolchain.h"
#include "optiforge/support/Diagnostic.h"
#include "optiforge/profile/Profile.h"
#include "optiforge/support/SourceManager.h"
#include "optiforge/support/Version.h"

using namespace optiforge;

namespace optiforge::transforms {
// Each transform registers itself at load time. These anchors are referenced
// below so the linker keeps those translation units: without them a static
// library drops any object whose symbols nobody names, taking the
// registrations with it.
void anchorScalarPasses();
void anchorSCCP();
void anchorGVN();
void anchorLICM();
void anchorSimplifyCFG();
void anchorInline();
void anchorLoopUnroll();
}  // namespace optiforge::transforms

namespace {

void keepPassRegistrations() {
  transforms::anchorScalarPasses();
  transforms::anchorSCCP();
  transforms::anchorGVN();
  transforms::anchorLICM();
  transforms::anchorSimplifyCFG();
  transforms::anchorInline();
  transforms::anchorLoopUnroll();
}


/// Loads the profile named by --use-profile and says, out loud, how much of it
/// applies to this module.
///
/// Every path through this returns. A profile that is missing, unreadable,
/// stale, or self-contradictory produces warnings and an ordinary build --
/// requirement PGO-11, which is designed in here rather than tested in later:
/// nothing below can change what the program computes, only what the optimizer
/// is told about it.
profile::ProfileData loadAndValidateProfile(const Options& opts, const ir::Module& module,
                                            DiagnosticEngine& diags) {
  profile::ProfileLoadOptions loadOptions;
  loadOptions.hotThresholdPercent = opts.hotThreshold;

  profile::ProfileData data = profile::loadProfile(opts.useProfile, loadOptions);

  for (const std::string& warning : data.warnings()) {
    diags.reportGlobal(DiagSeverity::Warning, warning);
  }
  if (!data.isValid()) {
    diags.reportGlobal(DiagSeverity::Warning,
                       "compiling without profile guidance");
    return data;
  }

  // Staleness. The hash is the cheap, exact check; the match rate is what says
  // whether a mismatched hash still leaves something usable (PGO-12).
  if (!data.matchesSource(module.sourceHash())) {
    diags.reportGlobal(
        DiagSeverity::Warning,
        "profile '" + opts.useProfile + "' was collected from different source "
        "(profile hash does not match this one); matching by name instead");
  }
  if (data.header().optLevel >= 0 && data.header().optLevel != opts.optLevel) {
    // ADR-05 puts instrumentation after optimization, so block names describe
    // the optimized CFG. A profile from a different -O level names a different
    // set of blocks, and the mismatch is worth saying before the match rate
    // makes it obvious.
    diags.reportGlobal(DiagSeverity::Warning,
                       "profile was collected at -O" +
                           std::to_string(data.header().optLevel) +
                           " but this build is -O" + std::to_string(opts.optLevel) +
                           "; block names will not line up");
  }

  for (const std::string& violation : data.flowViolations()) {
    diags.reportGlobal(DiagSeverity::Warning,
                       "profile is internally inconsistent: " + violation +
                           " (counts used as advisory only)");
  }

  // Match rate: how much of *this module* the profile actually describes. Run
  // over the module rather than the profile, so records for blocks that no
  // longer exist do not flatter the number.
  std::size_t functions = 0;
  std::size_t functionsMatched = 0;
  for (const auto& function : module.functions()) {
    if (function->isDeclaration()) {
      continue;
    }
    ++functions;
    if (data.functionCount(function->name()) > 0 ||
        data.functionHeat(function->name()) != profile::Heat::Unknown) {
      ++functionsMatched;
    }
  }

  const double rate = functions == 0
                          ? 1.0
                          : static_cast<double>(functionsMatched) /
                                static_cast<double>(functions);
  if (rate < 0.5) {
    diags.reportGlobal(
        DiagSeverity::Warning,
        "profile appears stale: it describes " + std::to_string(functionsMatched) +
            " of this module's " + std::to_string(functions) +
            " function(s); profile-guided optimization will do little");
  }

  return data;
}

ExitCode runCompilation(const Options& opts, SourceManager& sources, DiagnosticEngine& diags) {
  const std::optional<FileID> file = sources.addFile(opts.inputPath);
  if (!file.has_value()) {
    diags.reportGlobal(DiagSeverity::Error, "cannot open input file '" + opts.inputPath + "'");
    return ExitCode::EnvironmentError;
  }

  // --- Lexical analysis ---
  Lexer lexer(sources, *file, diags);
  const std::vector<Token> tokens = lexer.tokenize();

  if (opts.emit == EmitStage::Tokens) {
    // Dump what was scanned even when there were lexical errors: seeing the
    // token stream is exactly what makes such an error diagnosable.
    printTokens(tokens, std::cout);
    return diags.hadError() ? ExitCode::CompileError : ExitCode::Success;
  }

  if (diags.hadError()) {
    // Parsing a stream with error tokens produces noise, not information.
    return ExitCode::CompileError;
  }

  // --- Parsing ---
  Parser parser(tokens, diags);
  const std::unique_ptr<Program> program = parser.parseProgram();

  if (parser.hadError()) {
    return ExitCode::CompileError;
  }

  // --- Semantic analysis ---
  // The symbol table must outlive this scope's use of the AST: nodes hold raw
  // pointers to symbols it owns.
  SymbolTable symbols;
  Sema sema(diags, symbols);
  sema.analyze(*program);

  // Consult the engine rather than the pass's own flag: -Werror promotion
  // happens inside DiagnosticEngine, so a pass that reported only warnings
  // still believes it succeeded.
  if (diags.hadError()) {
    return ExitCode::CompileError;
  }

  if (opts.emit == EmitStage::Ast) {
    printAST(*program, std::cout);
    return ExitCode::Success;
  }

  // --- IR generation ---
  IRGen irgen(diags, opts.inputPath, sources.contentHash(*file));
  const std::unique_ptr<ir::Module> module = irgen.run(*program);

  ir::Verifier verifier;
  if (!verifier.verify(*module)) {
    // Invalid IR is a compiler bug, not a user error. Say so plainly and name
    // what is wrong rather than letting a later stage crash on it.
    diags.reportGlobal(DiagSeverity::Error,
                       "internal compiler error: generated IR failed verification");
    verifier.printErrors(std::cerr);
    return ExitCode::InternalError;
  }

  // --- SSA construction ---
  //
  // Skipped at -O0, where keeping every local in memory makes the IR map
  // straightforwardly onto the source (ADR-02).
  analysis::AnalysisManager analyses;
  ProfileLayout profileLayout;

  // --- Profile ingestion (Phase 10) ---
  //
  // Loaded before the pipeline runs, so a pass can ask for it. Nothing consumes
  // it yet; Phase 11 is what turns these numbers into decisions.
  std::shared_ptr<profile::ProfileData> profileData;
  if (!opts.useProfile.empty()) {
    profileData = std::make_shared<profile::ProfileData>(
        loadAndValidateProfile(opts, *module, diags));
    for (const auto& function : module->functions()) {
      analyses.provide<analysis::ProfileAnalysis>(*function, profileData);
    }
    // Counts onto the IR as well as into the analysis. The backend has no
    // AnalysisManager to ask, and both register allocation and block layout
    // need per-block frequencies.
    transforms::annotateBlockCounts(*module, *profileData);
  }

  if (opts.optLevel > 0) {
    transforms::promoteMemoryToRegisters(*module, analyses);

    if (!verifier.verify(*module)) {
      diags.reportGlobal(DiagSeverity::Error,
                         "internal compiler error: IR invalid after mem2reg");
      verifier.printErrors(std::cerr);
      return ExitCode::InternalError;
    }
    // --- Optimization pipeline ---
    passes::PassManager pipeline;
    passes::buildPipeline(pipeline, opts.optLevel, opts.disabledPasses);
    pipeline.setPrintAfterAll(opts.printAfterAll);
    pipeline.setPrintAfter(opts.printAfter);
    pipeline.setVerifyEach(opts.verifyEach);
    if (opts.pgoRemarks) {
      pipeline.setRemarkStream(&std::cerr);
    }
    pipeline.run(*module, analyses);

    // Module-level cleanup: inlining leaves the original behind, and no
    // function-scoped pass can see that nothing calls it any more.
    if (opts.optLevel >= 2) {
      transforms::removeUnusedFunctions(*module);
    }

    if (!verifier.verify(*module)) {
      diags.reportGlobal(DiagSeverity::Error,
                         "internal compiler error: IR invalid after optimization");
      verifier.printErrors(std::cerr);
      return ExitCode::InternalError;
    }

    const std::vector<std::string> ssaErrors =
        analysis::verifySSA(*module, analyses);
    if (!ssaErrors.empty()) {
      diags.reportGlobal(DiagSeverity::Error,
                         "internal compiler error: SSA form is invalid");
      for (const std::string& error : ssaErrors) {
        std::cerr << "  " << error << "\n";
      }
      return ExitCode::InternalError;
    }
  }

  if (opts.emit == EmitStage::Ir) {
    ir::printModule(*module, std::cout);
    return ExitCode::Success;
  }
  if (opts.emit == EmitStage::Cfg) {
    ir::printCFG(*module, std::cout);
    return ExitCode::Success;
  }
  if (opts.emit == EmitStage::Analysis) {
    analysis::printAnalyses(*module, std::cout);
    return ExitCode::Success;
  }

  // --- SSA destruction ---
  //
  // Runs last, immediately before code generation: it deliberately produces IR
  // that is no longer in SSA form, so nothing downstream may assume otherwise.
  transforms::destroySSA(*module);
  if (!verifier.verify(*module)) {
    diags.reportGlobal(DiagSeverity::Error,
                       "internal compiler error: IR invalid after SSA destruction");
    verifier.printErrors(std::cerr);
    return ExitCode::InternalError;
  }
  // Destruction rewrote every function, so nothing cached about them still
  // holds -- and the register allocator is about to ask for liveness.
  analyses.invalidateAll();

  // Second pass over the counts. Blocks created since the first one -- split
  // critical edges, preheaders, the copies unrolling made -- are matched by name
  // where the profile knows them and keep what they inherited where it does not.
  if (profileData != nullptr) {
    transforms::annotateBlockCounts(*module, *profileData);
  }

  // --- Where the output goes ---
  //
  // Computed before code generation rather than after, because an instrumented
  // binary has to be told at compile time where to write its profile.
  const std::filesystem::path inputPath(opts.inputPath);
  const std::string stem = inputPath.stem().string();
  const std::filesystem::path outputPath =
      opts.outputPath.empty() ? std::filesystem::path(stem + ".exe")
                              : std::filesystem::path(opts.outputPath);

  // --- Instrumentation (ADR-05: late, so the measured CFG is the optimized
  // --- one the profile-guided build will also see) ---
  if (opts.profile) {
    const std::string profileOut =
        opts.profileOut.empty() ? outputPath.stem().string() + ".prof" : opts.profileOut;

    profileLayout = transforms::instrumentForProfiling(
        *module, analyses, opts.optLevel,
        std::string("optiforge-") + OPTIFORGE_VERSION_STRING, profileOut,
        opts.profileTime);

    if (!verifier.verify(*module)) {
      diags.reportGlobal(DiagSeverity::Error,
                         "internal compiler error: IR invalid after instrumentation");
      verifier.printErrors(std::cerr);
      return ExitCode::InternalError;
    }
    analyses.invalidateAll();
  }

  // --- Code generation ---
  const backend::RegAllocKind allocator = opts.regalloc == RegAllocChoice::Naive
                                              ? backend::RegAllocKind::Naive
                                              : backend::RegAllocKind::Graph;
  backend::CodeGen codegen(backend::x86_64WindowsTarget(), allocator);
  codegen.setProfileLayout(std::move(profileLayout));
  codegen.setProfileGuidedLayout(profileData != nullptr && profileData->isValid());
  const backend::MModule machine = codegen.run(*module, analyses);

  if (opts.pgoRemarks && profileData != nullptr) {
    std::cerr << "layout: " << codegen.layout().moved << " block(s) reordered, "
              << codegen.layout().jumpsRemoved << " jump(s) became fall-through\n";
  }

  if (opts.printRegAlloc) {
    for (const backend::RegisterAssignment& allocation : codegen.allocations()) {
      std::cerr << allocation.summary() << "\n";
    }
  }

  if (!codegen.allocationErrors().empty()) {
    // The allocator already fell back to frame slots for the function it could
    // not vouch for, so the binary that comes out is still correct -- but a
    // violation here is a compiler bug and must not pass silently.
    diags.reportGlobal(DiagSeverity::Error,
                       "internal compiler error: register allocation is unsound");
    for (const std::string& error : codegen.allocationErrors()) {
      std::cerr << "  " << error << "\n";
    }
    return ExitCode::InternalError;
  }

  if (opts.emit == EmitStage::Asm) {
    backend::printAssembly(machine, std::cout);
    return ExitCode::Success;
  }

  // --- Assemble and link ---
  //
  // Intermediates are named after the *output*, not the input: two inputs
  // compiled to different outputs in one directory would otherwise write to
  // the same .s and .o.
  const std::string tempStem = outputPath.stem().string();
  const std::filesystem::path asmPath = outputPath.parent_path() / (tempStem + ".s");
  const std::filesystem::path objectPath = outputPath.parent_path() / (tempStem + ".o");

  {
    std::ofstream asmFile(asmPath);
    if (!asmFile) {
      diags.reportGlobal(DiagSeverity::Error,
                         "cannot write assembly to '" + asmPath.string() + "'");
      return ExitCode::EnvironmentError;
    }
    backend::printAssembly(machine, asmFile);
  }

  Toolchain toolchain(diags, opts.runtimeDir);

  const auto cleanup = [&](bool keepObject) {
    if (opts.keepTemps) {
      return;
    }
    std::error_code ec;
    std::filesystem::remove(asmPath, ec);
    if (!keepObject) {
      std::filesystem::remove(objectPath, ec);
    }
  };

  if (!toolchain.assemble(asmPath.string(), objectPath.string())) {
    cleanup(false);
    return ExitCode::EnvironmentError;
  }

  if (opts.emit == EmitStage::Obj) {
    cleanup(/*keepObject=*/true);
    return ExitCode::Success;
  }

  if (!toolchain.hasRuntime()) {
    toolchain.reportMissingRuntime();
    cleanup(false);
    return ExitCode::EnvironmentError;
  }

  if (opts.profile && !toolchain.hasProfileRuntime()) {
    diags.reportGlobal(DiagSeverity::Error,
                       "cannot locate libofprof.a in " + toolchain.runtimeDir() +
                           "\n  --profile needs the profile runtime, which is built "
                           "alongside libofrt.a");
    cleanup(false);
    return ExitCode::EnvironmentError;
  }

  if (!toolchain.link(objectPath.string(), outputPath.string(), opts.profile)) {
    cleanup(false);
    return ExitCode::EnvironmentError;
  }

  cleanup(false);
  return ExitCode::Success;
}

}  // namespace

int main(int argc, char** argv) {
  // Recorded so the runtime library can be found relative to this executable.
  Toolchain::setExecutablePath(argv[0]);

  keepPassRegistrations();

  Options opts;
  if (!parseOptions(argc, argv, opts, std::cerr)) {
    return toInt(ExitCode::UsageError);
  }

  if (opts.showHelp) {
    printHelp(std::cout);
    return toInt(ExitCode::Success);
  }

  if (opts.showVersion) {
    printVersion(std::cout);
    return toInt(ExitCode::Success);
  }

  // --profile-report is a mode of its own: it reads a .prof and nothing else,
  // so it needs no input program and runs before the check for one.
  if (!opts.profileReport.empty()) {
    profile::ProfileLoadOptions loadOptions;
    loadOptions.hotThresholdPercent = opts.hotThreshold;
    const profile::ProfileData data =
        profile::loadProfile(opts.profileReport, loadOptions);
    profile::printProfileReport(data, std::cout);
    return toInt(data.isValid() ? ExitCode::Success : ExitCode::EnvironmentError);
  }

  if (opts.inputPath.empty()) {
    std::cerr << "optiforge: error: no input file\n"
                 "  try 'optiforge --help'\n";
    return toInt(ExitCode::UsageError);
  }

  SourceManager sources;
  DiagnosticEngine diags(sources, std::cerr);
  diags.setWarningsAsErrors(opts.warningsAsErrors);

  ExitCode result = runCompilation(opts, sources, diags);
  diags.printSummary();

  // Final backstop. The DiagnosticEngine is the single authority on whether
  // compilation failed (architectural_design.md 7.1); a stage that returns
  // Success while the engine holds an error -- a warning promoted by -Werror,
  // for instance -- must not produce a zero exit status (CLI-09).
  if (result == ExitCode::Success && diags.hadError()) {
    result = ExitCode::CompileError;
  }
  return toInt(result);
}
