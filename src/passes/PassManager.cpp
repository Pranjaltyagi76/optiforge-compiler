#include "optiforge/passes/Pass.h"

#include <algorithm>
#include <iostream>
#include <string_view>
#include <utility>

#include "optiforge/analysis/AnalysisManager.h"
#include "optiforge/analysis/ProfileAnalysis.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Module.h"
#include "optiforge/ir/Printer.h"
#include "optiforge/ir/Verifier.h"

namespace optiforge::passes {

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

PassRegistry& PassRegistry::instance() {
  static PassRegistry registry;
  return registry;
}

void PassRegistry::add(std::string_view name, PassFactory factory) {
  factories_.emplace(std::string(name), factory);
}

std::unique_ptr<Pass> PassRegistry::create(std::string_view name) const {
  const auto it = factories_.find(name);
  return it == factories_.end() ? nullptr : it->second();
}

bool PassRegistry::contains(std::string_view name) const {
  return factories_.find(name) != factories_.end();
}

std::vector<std::string> PassRegistry::names() const {
  std::vector<std::string> result;
  result.reserve(factories_.size());
  for (const auto& [name, factory] : factories_) {
    result.push_back(name);
  }
  return result;  // std::map keeps these ordered, so output is deterministic
}

// ---------------------------------------------------------------------------
// Manager
// ---------------------------------------------------------------------------

void PassManager::add(std::unique_ptr<Pass> pass) {
  statistics_.push_back({std::string(pass->name()), 0, 0});
  passes_.push_back(std::move(pass));
}

namespace {

/// Passes a cold function still gets: everything the -O1 pipeline runs.
///
/// The list is the -O1 pipeline rather than a second hand-maintained one, so
/// "cold code is compiled at -O1" stays true by construction rather than by
/// someone remembering to update two places.
bool runsOnColdCode(std::string_view name) {
  for (const std::string& pass : pipelineFor(1)) {
    if (pass == name) {
      return true;
    }
  }
  return false;
}

/// True when the profile says this function is cold. False whenever there is no
/// profile, which is the fallback every profile-guided decision must have: no
/// measurement means no reason to treat the function differently.
bool isCold(const ir::Function& function, analysis::AnalysisManager& manager) {
  const profile::ProfileData* profile =
      manager.getCached<analysis::ProfileAnalysis>(function);
  return profile != nullptr && profile->isValid() &&
         profile->functionHeat(function.name()) == profile::Heat::Cold;
}

}  // namespace

bool PassManager::run(ir::Module& module, analysis::AnalysisManager& manager) {
  std::ostream& out = printStream_ != nullptr ? *printStream_ : std::cerr;
  bool changedOverall = false;

  // Passes enable one another -- folding exposes dead code, which exposes more
  // folding -- so the pipeline is swept until nothing changes. The cap stops
  // two passes that undo each other from looping forever (metric O-10).
  for (iterations_ = 0; iterations_ < kMaxIterations; ++iterations_) {
    bool changedThisSweep = false;

    for (const auto& function : module.functions()) {
      if (function->isDeclaration()) {
        continue;
      }

      // Cold functions are compiled at an effective -O1 (PGO-10). The gain is
      // instruction-cache pressure rather than direct execution time, and code
      // that never runs should not be spending compile time or code size on
      // unrolling and inlining.
      const bool cold = pgo_.coldSize && isCold(*function, manager);

      for (std::size_t i = 0; i < passes_.size(); ++i) {
        Pass& pass = *passes_[i];
        if (cold && !runsOnColdCode(pass.name())) {
          continue;
        }
        ++statistics_[i].runs;

        pass.setRemarkStream(remarkStream_);
        pass.setPgoControls(pgo_);
        const bool changed = pass.run(*function, manager);
        if (!changed) {
          continue;
        }

        ++statistics_[i].changed;
        changedThisSweep = true;
        changedOverall = true;

        // A pass that changed the IR invalidates every analysis of that
        // function. Declaring preserved analyses individually would be
        // faster; being wrong about one would be a miscompile, so the blunt
        // rule stays until there is a measurement saying it matters.
        manager.invalidate(*function);

        if (verifyEach_) {
          ir::Verifier verifier;
          if (!verifier.verify(*function)) {
            out << "IR verification failed after pass '" << pass.name()
                << "' on function @" << function->name() << ":\n";
            verifier.printErrors(out);
          }
        }

        if (printAfterAll_ || printAfter_ == pass.name()) {
          out << "\n; --- after " << pass.name() << " on @" << function->name()
              << " ---\n";
          ir::printModule(module, out);
        }
      }
    }

    if (!changedThisSweep) {
      break;
    }
  }

  return changedOverall;
}

// ---------------------------------------------------------------------------
// Pipelines (OPT-04)
// ---------------------------------------------------------------------------

std::vector<std::string> pipelineFor(int optimizationLevel) {
  if (optimizationLevel <= 0) {
    // -O0 runs nothing: the IR should map onto the source so it can be read
    // against it while debugging.
    return {};
  }

  if (optimizationLevel == 1) {
    return {
        "simplify-cfg",
        "constant-folding",
        "sccp",
        "copy-propagation",
        "dce",
    };
  }

  // -O2. Order matters: fold first so later passes see constants, inline so
  // the callee's body meets the caller's constants, then re-run the scalar
  // passes over what inlining exposed, and finish with a cleanup sweep.
  return {
      "simplify-cfg",
      "constant-folding",
      "sccp",
      "copy-propagation",
      "gvn",
      "dce",
      "inline",
      "constant-folding",
      "sccp",
      "gvn",
      // Unrolling comes after LICM so the loop it replicates has already had
      // everything invariant lifted out of it -- copying that work `factor`
      // times would be exactly the wrong thing. It does nothing at all without
      // a profile, so this addition leaves the -O2 pipeline unchanged for an
      // ordinary build.
      "licm",
      "loop-unroll",
      "constant-folding",
      "gvn",
      "strength-reduction",
      "dce",
      "simplify-cfg",
  };
}

void buildPipeline(PassManager& manager, int optimizationLevel,
                   const std::vector<std::string>& disabled) {
  for (const std::string& name : pipelineFor(optimizationLevel)) {
    if (std::find(disabled.begin(), disabled.end(), name) != disabled.end()) {
      continue;
    }
    if (std::unique_ptr<Pass> pass = PassRegistry::instance().create(name)) {
      manager.add(std::move(pass));
    }
  }
}

}  // namespace optiforge::passes
