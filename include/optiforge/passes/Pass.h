#pragma once

#include <iosfwd>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace optiforge::ir {
class Function;
class Module;
}  // namespace optiforge::ir

namespace optiforge::analysis {
class AnalysisManager;
}

namespace optiforge::passes {

/// One optimization pass over a function.
///
/// Passes never reference one another (architectural_design.md section 3,
/// rule 5). All coordination goes through the pass manager and the analysis
/// cache, which is what lets a pass be added, removed or reordered without
/// touching any other.
class Pass {
public:
  virtual ~Pass() = default;

  /// Name used by --print-after= and by the disable list. Must be stable.
  virtual std::string_view name() const = 0;

  /// One-line description for --help-passes.
  virtual std::string_view description() const = 0;

  /// Returns true when the IR changed, which is what drives analysis
  /// invalidation and pipeline convergence (OPT-07). Reporting a change that
  /// did not happen costs an iteration; failing to report one that did is a
  /// correctness bug.
  virtual bool run(ir::Function& function, analysis::AnalysisManager& manager) = 0;
};

using PassFactory = std::unique_ptr<Pass> (*)();

/// Name-to-factory map, populated at static-initialization time.
///
/// Registration rather than a hard-wired list is what makes `--print-after=x`
/// and `--disable-pass=x` work for every pass without a lookup table anyone
/// has to remember to update.
class PassRegistry {
public:
  static PassRegistry& instance();

  void add(std::string_view name, PassFactory factory);
  std::unique_ptr<Pass> create(std::string_view name) const;
  bool contains(std::string_view name) const;
  /// Every registered pass, in name order.
  std::vector<std::string> names() const;

private:
  std::map<std::string, PassFactory, std::less<>> factories_;
};

/// Registers a pass at load time.
struct PassRegistration {
  PassRegistration(std::string_view name, PassFactory factory) {
    PassRegistry::instance().add(name, factory);
  }
};

/// Per-pass tallies, the raw material for metrics O-01 and O-02.
struct PassStatistics {
  std::string name;
  unsigned runs = 0;
  unsigned changed = 0;  // times the pass reported a change
};

/// Runs a sequence of passes over every function in a module.
class PassManager {
public:
  void add(std::unique_ptr<Pass> pass);

  /// Runs the pipeline to a fixed point, up to `maxIterations` sweeps.
  /// Returns true if anything changed.
  bool run(ir::Module& module, analysis::AnalysisManager& manager);

  /// Dump IR after every pass that changed something.
  void setPrintAfterAll(bool value) { printAfterAll_ = value; }
  /// Dump IR after this pass specifically.
  void setPrintAfter(std::string name) { printAfter_ = std::move(name); }
  /// Where dumps go. Defaults to std::cerr so they never corrupt --emit output.
  void setPrintStream(std::ostream* stream) { printStream_ = stream; }
  /// Run the IR verifier after every pass, naming the pass that broke it.
  void setVerifyEach(bool value) { verifyEach_ = value; }

  /// Sweeps taken before the pipeline stopped changing. Metric O-10 caps this
  /// so two passes undoing each other cannot loop forever.
  unsigned iterations() const { return iterations_; }
  const std::vector<PassStatistics>& statistics() const { return statistics_; }

  static constexpr unsigned kMaxIterations = 8;

private:
  std::vector<std::unique_ptr<Pass>> passes_;
  std::vector<PassStatistics> statistics_;
  std::string printAfter_;
  std::ostream* printStream_ = nullptr;
  bool printAfterAll_ = false;
  bool verifyEach_ = false;
  unsigned iterations_ = 0;
};

/// Pipeline definitions, declarative and in one place (OPT-04).
///
/// `disabled` names passes to leave out, which is how a suspect pass is
/// bisected out of a miscompile.
void buildPipeline(PassManager& manager, int optimizationLevel,
                   const std::vector<std::string>& disabled);

/// Pass names in each pipeline, for --help-passes and for tests that assert
/// the pipeline is what the documentation claims.
std::vector<std::string> pipelineFor(int optimizationLevel);

}  // namespace optiforge::passes
